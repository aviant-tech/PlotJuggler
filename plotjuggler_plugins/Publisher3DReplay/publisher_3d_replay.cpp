#include "publisher_3d_replay.h"

#include <QDesktopServices>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QMessageBox>
#include <QRegularExpression>
#include <QSettings>
#include <QTcpSocket>
#include <algorithm>
#include <cmath>

namespace
{
// a fixed port lets an open page reconnect after PlotJuggler is restarted
constexpr quint16 kDefaultPort = 8642;
// the page interpolates between samples, no need to send more than this
constexpr double kMinSampleInterval = 0.02;
constexpr int kMaxRequestHeaderSize = 16 * 1024;
const char* kIonTokenSetting = "Publisher3DReplay/ion_token";
// a .glb on disk replacing the built-in Notus 4 model
const char* kModelFileSetting = "Publisher3DReplay/model_file";
const char* kBuiltInModel = ":/replay3d/notus4.glb";

struct SeriesSpec
{
  const char* topic;
  std::vector<const char*> fields;
  std::vector<double> scales;
};

// position sources in order of preference: lat [deg], lon [deg], alt AMSL [m]
const std::vector<SeriesSpec> kPositionSpecs = {
  { "vehicle_global_position", { "lat", "lon", "alt" }, { 1, 1, 1 } },
  { "vehicle_gps_position", { "latitude_deg", "longitude_deg", "altitude_msl_m" }, { 1, 1, 1 } },
  // before PX4 v1.15
  { "vehicle_gps_position", { "lat", "lon", "alt" }, { 1e-7, 1e-7, 1e-3 } },
};
const SeriesSpec kAttitudeSpec = { "vehicle_attitude",
                                   { "q.00", "q.01", "q.02", "q.03" },
                                   { 1, 1, 1, 1 } };
const SeriesSpec kNavStateSpec = { "vehicle_status", { "nav_state" }, { 1 } };
// paid out line [m], in the winch log FMS merges into a "combined" ULog
const SeriesSpec kWinchSpec = { "aviant_winch", { "Position" }, { 1 } };
// winch states, one-hot: lowering the package, and the hook released it
const SeriesSpec kWinchDroppingSpec = { "aviant_winch", { "State_State.DROPPING" }, { 1 } };
const SeriesSpec kWinchDroppedSpec = { "aviant_winch", { "State_State.DROPPED" }, { 1 } };
// motor commands [0, 1], enough to show which props turn how fast. A stopped
// motor is NaN, which PlotData drops: the series have gaps, not aligned samples.
constexpr double kMotorSampleInterval = 0.1;
const std::vector<const char*> kMotorFields = { "control.00", "control.01", "control.02",
                                                "control.03", "control.04", "control.05",
                                                "control.06", "control.07", "control.08",
                                                "control.09", "control.10", "control.11" };

bool endsWith(const std::string& str, const std::string& suffix)
{
  return str.size() >= suffix.size() &&
         str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
}

/// Find "<topic>/<field>", also when the loader prefixed it (e.g. with the file
/// name) or appended a multi-instance id ("<topic>.00/<field>").
const PJ::PlotData* findSeries(const PJ::PlotDataMapRef& datamap, const std::string& topic,
                               const std::string& field, std::string* key)
{
  const std::string plain = topic + "/" + field;
  const std::string first_instance = topic + ".00/" + field;
  const PJ::PlotData* found = nullptr;
  for (const auto& [name, series] : datamap.numeric)
  {
    const bool match = name == plain || endsWith(name, "/" + plain) || name == first_instance ||
                       endsWith(name, "/" + first_instance);
    // prefer the shortest match: the least decorated name
    if (match && (!found || name.size() < key->size()))
    {
      found = &series;
      *key = name;
    }
  }
  return found;
}

/// The names of all fields of a spec, or empty when one is missing. The series
/// may still be empty: a source that loads on demand (FMS) only registers them.
std::vector<std::string> findKeys(const PJ::PlotDataMapRef& datamap, const SeriesSpec& spec)
{
  std::vector<std::string> keys;
  for (const char* field : spec.fields)
  {
    std::string key;
    if (!findSeries(datamap, spec.topic, field, &key))
    {
      return {};
    }
    keys.push_back(key);
  }
  return keys;
}

/// All fields of a spec, or empty when one is missing, empty or they are not aligned.
std::vector<const PJ::PlotData*> findSpec(const PJ::PlotDataMapRef& datamap, const SeriesSpec& spec)
{
  std::vector<const PJ::PlotData*> result;
  for (const auto& key : findKeys(datamap, spec))
  {
    const PJ::PlotData& series = datamap.numeric.at(key);
    if (series.size() == 0 || (!result.empty() && series.size() != result[0]->size()))
    {
      return {};
    }
    result.push_back(&series);
  }
  return result;
}

/// actuator_motors with as many motors as the data has
SeriesSpec motorSpec(const PJ::PlotDataMapRef& datamap)
{
  SeriesSpec spec = { "actuator_motors", {}, {} };
  for (const char* field : kMotorFields)
  {
    std::string key;
    if (!findSeries(datamap, spec.topic, field, &key))
    {
      break;
    }
    spec.fields.push_back(field);
    spec.scales.push_back(1);
  }
  return spec;
}

/// Time of the first sample of a one-hot series that is set, NaN if none is.
double firstSet(const std::vector<const PJ::PlotData*>& series)
{
  if (!series.empty())
  {
    for (size_t i = 0; i < series[0]->size(); i++)
    {
      if (series[0]->at(i).y > 0.5)
      {
        return series[0]->at(i).x;
      }
    }
  }
  return std::numeric_limits<double>::quiet_NaN();
}

/// Append {"t":[...],"v":[...]} with one flattened row of values per kept sample.
/// Samples closer than min_interval to the previous one are dropped, as
/// are rows containing NaN (e.g. no GPS fix yet).
void appendSamples(QByteArray& out, const std::vector<const PJ::PlotData*>& series,
                   const std::vector<double>& scales, int decimals, double min_interval)
{
  QByteArray times;
  QByteArray values;
  const size_t count = series[0]->size();
  double last_time = -std::numeric_limits<double>::infinity();
  for (size_t i = 0; i < count; i++)
  {
    const double t = series[0]->at(i).x;
    if (t - last_time < min_interval && i + 1 < count)
    {
      continue;
    }
    bool valid = true;
    for (const auto* s : series)
    {
      valid = valid && std::isfinite(s->at(i).y);
    }
    if (!valid)
    {
      continue;
    }
    last_time = t;
    times += QByteArray::number(t, 'f', 6) + ',';
    for (size_t k = 0; k < series.size(); k++)
    {
      values += QByteArray::number(series[k]->at(i).y * scales[k], 'f', decimals) + ',';
    }
  }
  times.chop(1);
  values.chop(1);
  out += "{\"t\":[" + times + "],\"v\":[" + values + "]}";
}

QByteArray contentType(const QString& path)
{
  if (path.endsWith(".html"))
  {
    return "text/html; charset=utf-8";
  }
  if (path.endsWith(".js"))
  {
    return "text/javascript; charset=utf-8";
  }
  if (path.endsWith(".glb"))
  {
    return "model/gltf-binary";
  }
  return "application/octet-stream";
}
}  // namespace

Publisher3DReplay::Publisher3DReplay()
  : _ws_server("PlotJuggler 3D Replay", QWebSocketServer::NonSecureMode)
{
  connect(&_http_server, &QTcpServer::newConnection, this, &Publisher3DReplay::onNewHttpConnection);
  connect(&_ws_server, &QWebSocketServer::newConnection, this, &Publisher3DReplay::onNewWebSocket);

  _poll_timer.setInterval(1000);
  connect(&_poll_timer, &QTimer::timeout, this, &Publisher3DReplay::pollData);

  auto open_action = new QAction("Open in browser", this);
  connect(open_action, &QAction::triggered, this, [this]() {
    if (_enabled)
    {
      openBrowser();
    }
  });
  auto model_action = new QAction("Set 3D model...", this);
  connect(model_action, &QAction::triggered, this, &Publisher3DReplay::chooseModelFile);
  auto builtin_model_action = new QAction("Use the built-in 3D model", this);
  connect(builtin_model_action, &QAction::triggered, this, [this]() {
    QSettings().remove(kModelFileSetting);
    sendTrajectoryToAll();
  });
  auto token_action = new QAction("Set Cesium ion token...", this);
  connect(token_action, &QAction::triggered, this, &Publisher3DReplay::editIonToken);
  _actions = { open_action, model_action, builtin_model_action, token_action };
}

Publisher3DReplay::~Publisher3DReplay()
{
  setEnabled(false);
}

void Publisher3DReplay::setEnabled(bool enabled)
{
  if (enabled == _enabled)
  {
    return;
  }
  _enabled = enabled;

  if (!enabled)
  {
    _poll_timer.stop();
    for (auto* client : _clients)
    {
      client->disconnect(this);
      client->close();
      client->deleteLater();
    }
    _clients.clear();
    _http_server.close();
    return;
  }

  if (!_http_server.listen(QHostAddress::LocalHost, kDefaultPort) &&
      !_http_server.listen(QHostAddress::LocalHost, 0))
  {
    QMessageBox::warning(nullptr, name(),
                         "Cannot start the 3D replay server: " + _http_server.errorString());
    _enabled = false;
    emit closed();
    return;
  }

  _data_signature.clear();
  pollData();
  _poll_timer.start();

  // a page left open from earlier reconnects by itself; only open a new one if none does
  QTimer::singleShot(1500, this, [this]() {
    if (_enabled && _clients.empty())
    {
      openBrowser();
    }
  });
}

QUrl Publisher3DReplay::pageUrl() const
{
  return QUrl(QString("http://127.0.0.1:%1/").arg(port()));
}

void Publisher3DReplay::openBrowser()
{
  if (!QDesktopServices::openUrl(pageUrl()))
  {
    QMessageBox::information(nullptr, name(),
                             "Open " + pageUrl().toString() + " in a web browser.");
  }
}

void Publisher3DReplay::editIonToken()
{
  QSettings settings;
  bool ok = false;
  const QString token = QInputDialog::getText(
      nullptr, name(),
      "Cesium ion access token for terrain and imagery\n"
      "(from https://ion.cesium.com/tokens, leave empty to use the CesiumJS default):",
      QLineEdit::Normal, settings.value(kIonTokenSetting).toString(), &ok);
  if (ok)
  {
    settings.setValue(kIonTokenSetting, token.trimmed());
    sendTrajectoryToAll();
  }
}

QString Publisher3DReplay::modelFile() const
{
  const QString file = QSettings().value(kModelFileSetting).toString();
  return !file.isEmpty() && QFileInfo(file).isReadable() ? file : kBuiltInModel;
}

void Publisher3DReplay::chooseModelFile()
{
  const QString file = QFileDialog::getOpenFileName(
      nullptr, "3D model of the vehicle", QSettings().value(kModelFileSetting).toString(),
      "glTF binary, +X left, +Y up, +Z forward (*.glb)");
  if (!file.isEmpty())
  {
    QSettings().setValue(kModelFileSetting, file);
    sendTrajectoryToAll();
  }
}

void Publisher3DReplay::updateState(double current_time)
{
  _current_time = current_time;
  sendTime();
}

void Publisher3DReplay::play(double current_time)
{
  updateState(current_time);
}

void Publisher3DReplay::onNewHttpConnection()
{
  while (QTcpSocket* socket = _http_server.nextPendingConnection())
  {
    connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
    connect(socket, &QTcpSocket::readyRead, this, [this, socket]() { handleHttpRequest(socket); });
  }
}

void Publisher3DReplay::handleHttpRequest(QTcpSocket* socket)
{
  // peek, so the websocket server can read the upgrade request itself
  const QByteArray request = socket->peek(kMaxRequestHeaderSize);
  const int header_end = request.indexOf("\r\n\r\n");
  if (header_end < 0)
  {
    if (request.size() >= kMaxRequestHeaderSize)
    {
      socket->abort();
    }
    return;
  }
  const QByteArray header = request.left(header_end).toLower();

  if (header.contains("\nupgrade: websocket"))
  {
    socket->disconnect(this);
    _ws_server.handleConnection(socket);
    return;
  }

  socket->read(header_end + 4);
  const QList<QByteArray> request_line = request.left(request.indexOf("\r\n")).split(' ');
  QString path = request_line.size() >= 2 ? QString::fromLatin1(request_line[1]) : QString();
  path = path.section('?', 0, 0);
  if (path == "/")
  {
    path = "/index.html";
  }

  QFile file(path == "/model.glb" ? modelFile() : ":/replay3d" + path);
  const bool found = request_line[0] == "GET" && path.startsWith('/') && !path.contains("..") &&
                     file.open(QIODevice::ReadOnly);
  const QByteArray body = found ? file.readAll() : QByteArray("Not found");

  QByteArray response = found ? "HTTP/1.1 200 OK\r\n" : "HTTP/1.1 404 Not Found\r\n";
  response += "Content-Type: " + (found ? contentType(path) : QByteArray("text/plain")) + "\r\n";
  response += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
  response += "Cache-Control: no-cache\r\nConnection: close\r\n\r\n";
  socket->write(response + body);
  socket->disconnectFromHost();
}

void Publisher3DReplay::onNewWebSocket()
{
  while (QWebSocket* client = _ws_server.nextPendingConnection())
  {
    // only our own page may read the flight data, not any website open in the browser
    const QString origin = client->origin();
    if (origin != QString("http://127.0.0.1:%1").arg(port()) &&
        origin != QString("http://localhost:%1").arg(port()))
    {
      client->close(QWebSocketProtocol::CloseCodePolicyViolated, "Origin not allowed");
      client->deleteLater();
      continue;
    }

    connect(client, &QWebSocket::disconnected, this, [this, client]() {
      _clients.erase(std::remove(_clients.begin(), _clients.end(), client), _clients.end());
      client->deleteLater();
    });
    _clients.push_back(client);

    client->sendTextMessage(QString::fromUtf8(trajectoryMessage()));
    sendTime();
  }
}

void Publisher3DReplay::sendTrajectoryToAll()
{
  if (_clients.empty())
  {
    return;
  }
  const QString trajectory = QString::fromUtf8(trajectoryMessage());
  for (auto* client : _clients)
  {
    client->sendTextMessage(trajectory);
  }
  sendTime();
}

void Publisher3DReplay::sendTime()
{
  if (std::isnan(_current_time))
  {
    return;
  }
  const QString message =
      QString("{\"type\":\"time\",\"t\":%1}").arg(QString::number(_current_time, 'f', 6));
  for (auto* client : _clients)
  {
    client->sendTextMessage(message);
  }
}

QByteArray Publisher3DReplay::dataSignature() const
{
  // the number of series and the length of the ones the trajectory is built from
  QByteArray signature = QByteArray::number(qulonglong(_datamap->numeric.size()));
  for (const auto& key : _watched_keys)
  {
    const auto it = _datamap->numeric.find(key);
    signature +=
        ':' + QByteArray::number(it == _datamap->numeric.end() ? -1 : qlonglong(it->second.size()));
  }
  return signature;
}

void Publisher3DReplay::watchSeries()
{
  _watched_keys.clear();
  // the first position source the log has, not every one of them
  for (const auto& spec : kPositionSpecs)
  {
    const auto keys = findKeys(*_datamap, spec);
    if (!keys.empty())
    {
      _watched_keys = keys;
      break;
    }
  }
  const SeriesSpec motors = motorSpec(*_datamap);
  for (const SeriesSpec* spec : { &kAttitudeSpec, &kNavStateSpec, &motors, &kWinchSpec,
                                  &kWinchDroppingSpec, &kWinchDroppedSpec })
  {
    const auto keys = findKeys(*_datamap, *spec);
    _watched_keys.insert(_watched_keys.end(), keys.begin(), keys.end());
  }
  for (const auto& key : _watched_keys)
  {
    if (_datamap->numeric.at(key).size() == 0)
    {
      // the source fetches the whole topic and ignores repeated requests
      emit seriesRequested(key);
    }
  }
}

void Publisher3DReplay::pollData()
{
  if (dataSignature() == _data_signature)
  {
    return;
  }
  watchSeries();
  _data_signature = dataSignature();
  sendTrajectoryToAll();
}

QByteArray Publisher3DReplay::trajectoryMessage()
{
  std::vector<const PJ::PlotData*> position;
  const SeriesSpec* position_spec = nullptr;
  for (const auto& spec : kPositionSpecs)
  {
    position = findSpec(*_datamap, spec);
    if (!position.empty())
    {
      position_spec = &spec;
      break;
    }
  }
  const auto attitude = findSpec(*_datamap, kAttitudeSpec);
  const auto nav_state = findSpec(*_datamap, kNavStateSpec);

  // tokens are JWTs; dropping anything else keeps the JSON valid
  QString token = QSettings().value(kIonTokenSetting).toString();
  token.remove(QRegularExpression("[^A-Za-z0-9._-]"));
  QByteArray out = "{\"type\":\"trajectory\",\"ion_token\":" +
                   (token.isEmpty() ? QByteArray("null") : '"' + token.toUtf8() + '"');
  // a new query for a new model file, or the browser keeps showing its cached one
  const QFileInfo model(modelFile());
  out += ",\"model\":\"model.glb?" +
         QByteArray::number(
             qHash(model.absoluteFilePath() + model.lastModified().toString(Qt::ISODateWithMs))) +
         '"';
  if (!position_spec || attitude.empty())
  {
    const bool downloading =
        std::any_of(_watched_keys.begin(), _watched_keys.end(), [this](const std::string& key) {
          const auto it = _datamap->numeric.find(key);
          return it != _datamap->numeric.end() && it->second.size() == 0;
        });
    return out + ",\"missing\":\"" +
           (downloading ? "Downloading the position and attitude..." :
                          "The data has no vehicle_global_position lat/lon/alt (or "
                          "vehicle_gps_position) and vehicle_attitude q to replay") +
           "\"}";
  }

  out += ",\"position\":";
  appendSamples(out, position, position_spec->scales, 8, kMinSampleInterval);
  out += ",\"attitude\":";
  appendSamples(out, attitude, kAttitudeSpec.scales, 5, kMinSampleInterval);
  if (!nav_state.empty())
  {
    out += ",\"nav_state\":";
    // discrete: keep every sample, a decimated one could be a mode change
    appendSamples(out, nav_state, kNavStateSpec.scales, 0, 0);
  }
  // one series per motor, in actuator_motors order
  const SeriesSpec motor_spec = motorSpec(*_datamap);
  QByteArray motors;
  for (const char* field : motor_spec.fields)
  {
    const auto motor = findSpec(*_datamap, { motor_spec.topic, { field }, { 1 } });
    if (motor.empty())
    {
      motors += "{\"t\":[],\"v\":[]}";
    }
    else
    {
      appendSamples(motors, motor, { 1 }, 2, kMotorSampleInterval);
    }
    motors += ',';
  }
  if (!motors.isEmpty())
  {
    motors.chop(1);
    out += ",\"motors\":[" + motors + "]";
  }
  const auto winch = findSpec(*_datamap, kWinchSpec);
  if (!winch.empty())
  {
    out += ",\"winch\":";
    appendSamples(out, winch, kWinchSpec.scales, 3, kMinSampleInterval);
    // a package hangs from the hook until it is released, on flights that lower one
    const double dropping = firstSet(findSpec(*_datamap, kWinchDroppingSpec));
    const double released = firstSet(findSpec(*_datamap, kWinchDroppedSpec));
    if (std::isfinite(dropping))
    {
      out += ",\"package\":{\"released\":" +
             (std::isfinite(released) ? QByteArray::number(released, 'f', 6) : QByteArray("null")) +
             "}";
    }
  }
  return out + "}";
}
