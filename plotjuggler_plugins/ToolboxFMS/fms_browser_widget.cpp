#include "fms_browser_widget.h"

#include <QCheckBox>
#include <QDate>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSplitter>
#include <QTreeWidget>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>

#include <algorithm>
#include <cstring>
#include <vector>

namespace
{
constexpr int SPEC_ROLE = Qt::UserRole;  // '<dataset>_<multi_id>.<field>' spec
constexpr int FIELDS_PER_REQUEST = 100;
}  // namespace

FmsBrowserWidget::FmsBrowserWidget(QWidget* parent) : QWidget(parent)
{
  _network = new QNetworkAccessManager(this);

  QSettings settings;
  QString default_server = qEnvironmentVariable("FMS_SERVER");
  if (default_server.isEmpty())
  {
    default_server = settings.value("ToolboxFMS/server", "https://fms.aviant.no").toString();
  }
  QString default_token = qEnvironmentVariable("FMS_API_TOKEN");
  if (default_token.isEmpty())
  {
    default_token = settings.value("ToolboxFMS/token").toString();
  }

  _server_edit = new QLineEdit(default_server, this);
  _token_edit = new QLineEdit(default_token, this);
  _token_edit->setEchoMode(QLineEdit::Password);
  _token_edit->setPlaceholderText("FMS API token (or set FMS_API_TOKEN)");

  _filter_edit = new QLineEdit(this);
  _filter_edit->setPlaceholderText("e.g. date_after=2026-06-01&aircraft=3");
  _filter_edit->setText("date_after=" + QDate::currentDate().addDays(-30).toString(Qt::ISODate));
  _search_button = new QPushButton("Search", this);

  _flight_list = new QListWidget(this);
  _flight_list->setSelectionMode(QAbstractItemView::SingleSelection);

  _field_filter_edit = new QLineEdit(this);
  _field_filter_edit->setPlaceholderText("Filter fields...");
  _field_tree = new QTreeWidget(this);
  _field_tree->setHeaderHidden(true);

  _parameters_check = new QCheckBox("Import parameters", this);
  _parameters_check->setChecked(true);
  _prefix_check = new QCheckBox("Prefix series with flight ID", this);

  _load_button = new QPushButton("Load selected series", this);
  _load_button->setEnabled(false);
  auto* close_button = new QPushButton("Close", this);

  _status_label = new QLabel(this);
  _status_label->setWordWrap(true);

  auto* server_row = new QHBoxLayout();
  server_row->addWidget(new QLabel("Server:", this));
  server_row->addWidget(_server_edit, 1);

  auto* token_row = new QHBoxLayout();
  token_row->addWidget(new QLabel("Token:", this));
  token_row->addWidget(_token_edit, 1);

  auto* filter_row = new QHBoxLayout();
  filter_row->addWidget(_filter_edit, 1);
  filter_row->addWidget(_search_button);

  auto* tree_container = new QWidget(this);
  auto* tree_layout = new QVBoxLayout(tree_container);
  tree_layout->setContentsMargins(0, 0, 0, 0);
  tree_layout->addWidget(_field_filter_edit);
  tree_layout->addWidget(_field_tree);

  auto* splitter = new QSplitter(Qt::Vertical, this);
  splitter->addWidget(_flight_list);
  splitter->addWidget(tree_container);
  splitter->setStretchFactor(0, 1);
  splitter->setStretchFactor(1, 2);

  auto* options_row = new QHBoxLayout();
  options_row->addWidget(_parameters_check);
  options_row->addWidget(_prefix_check);

  auto* buttons_row = new QHBoxLayout();
  buttons_row->addWidget(_load_button, 1);
  buttons_row->addWidget(close_button);

  auto* main_layout = new QVBoxLayout(this);
  main_layout->addLayout(server_row);
  main_layout->addLayout(token_row);
  main_layout->addLayout(filter_row);
  main_layout->addWidget(splitter, 1);
  main_layout->addLayout(options_row);
  main_layout->addLayout(buttons_row);
  main_layout->addWidget(_status_label);

  connect(_search_button, &QPushButton::clicked, this, &FmsBrowserWidget::searchFlights);
  connect(_filter_edit, &QLineEdit::returnPressed, this, &FmsBrowserWidget::searchFlights);
  connect(_flight_list, &QListWidget::itemSelectionChanged, this,
          &FmsBrowserWidget::onFlightSelected);
  connect(_load_button, &QPushButton::clicked, this, &FmsBrowserWidget::loadSelectedSeries);
  connect(close_button, &QPushButton::clicked, this, [this]() { emit closed(); });
  connect(_field_filter_edit, &QLineEdit::textChanged, this,
          &FmsBrowserWidget::applyFieldFilter);

  connect(_field_tree, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int) {
    // Toggling a topic toggles all its (visible) fields
    if (item->childCount() > 0 && item->checkState(0) != Qt::PartiallyChecked)
    {
      _field_tree->blockSignals(true);
      for (int i = 0; i < item->childCount(); i++)
      {
        if (!item->child(i)->isHidden())
        {
          item->child(i)->setCheckState(0, item->checkState(0));
        }
      }
      _field_tree->blockSignals(false);
    }
    updateLoadButton();
  });
}

void FmsBrowserWidget::onShow()
{
  const QString env_flight = qEnvironmentVariable("FMS_FLIGHT_ID");
  if (!env_flight.isEmpty() && !_env_flight_consumed)
  {
    _env_flight_consumed = true;
    _filter_edit->setText("id=" + env_flight);
    searchFlights();
  }
  else if (_flight_list->count() == 0)
  {
    searchFlights();
  }
}

QNetworkReply* FmsBrowserWidget::apiGet(const QString& path_and_query)
{
  QString server = _server_edit->text().trimmed();
  while (server.endsWith('/'))
  {
    server.chop(1);
  }
  QNetworkRequest request(QUrl(server + path_and_query));
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                       QNetworkRequest::NoLessSafeRedirectPolicy);
  const QString token = _token_edit->text().trimmed();
  if (!token.isEmpty())
  {
    request.setRawHeader("Authorization", ("Token " + token).toUtf8());
  }
  return _network->get(request);
}

void FmsBrowserWidget::searchFlights()
{
  QSettings settings;
  settings.setValue("ToolboxFMS/server", _server_edit->text().trimmed());
  settings.setValue("ToolboxFMS/token", _token_edit->text().trimmed());

  setStatus("Fetching flights...");
  _search_button->setEnabled(false);

  if (!_aircraft_names.empty())
  {
    requestFlightList();
    return;
  }
  // resolve aircraft ids to names once, like check_remote_flights does
  QNetworkReply* reply = apiGet("/api/flight/help/");
  connect(reply, &QNetworkReply::finished, this, [this, reply]() {
    reply->deleteLater();
    if (reply->error() == QNetworkReply::NoError)
    {
      const QJsonObject aircraft =
          QJsonDocument::fromJson(reply->readAll()).object()["aircraft"].toObject();
      for (auto it = aircraft.begin(); it != aircraft.end(); ++it)
      {
        _aircraft_names[it.key().toInt()] = it.value().toString();
      }
    }
    requestFlightList();
  });
}

void FmsBrowserWidget::requestFlightList()
{
  QString filter = _filter_edit->text().trimmed();
  if (!filter.isEmpty())
  {
    filter += "&";
  }
  QNetworkReply* reply = apiGet("/api/flight/?" + filter + "no_page=1");
  connect(reply, &QNetworkReply::finished, this, [this, reply]() {
    reply->deleteLater();
    _search_button->setEnabled(true);
    if (reply->error() != QNetworkReply::NoError)
    {
      setStatus("Flight list error: " + reply->errorString() + " " +
                    QString::fromUtf8(reply->readAll().left(200)),
                true);
      return;
    }
    populateFlightList(reply->readAll());
  });
}

void FmsBrowserWidget::populateFlightList(const QByteArray& flights_json)
{
  const QJsonDocument doc = QJsonDocument::fromJson(flights_json);
  QJsonArray flights = doc.isArray() ? doc.array() : doc.object()["results"].toArray();

  _flight_list->clear();
  _field_tree->clear();
  updateLoadButton();

  for (const QJsonValue& value : flights)
  {
    const QJsonObject flight = value.toObject();
    const int id = flight["id"].toInt();
    const QString start_time = flight["start_time"].toString().left(16).replace("T", " ");
    auto aircraft_it = _aircraft_names.find(flight["aircraft"].toInt());
    const QString aircraft =
        aircraft_it != _aircraft_names.end() ? aircraft_it->second : QString("?");
    auto* item = new QListWidgetItem(QString("%1  %2: %3").arg(start_time, aircraft,
                                                               flight["oneliner"].toString()),
                                     _flight_list);
    item->setData(Qt::UserRole, id);
    item->setToolTip(QString("Flight %1\n%2").arg(id).arg(flight["name"].toString()));
  }
  setStatus(QString("%1 flight(s) found").arg(flights.size()));

  if (flights.size() == 1)
  {
    _flight_list->setCurrentRow(0);
  }
}

void FmsBrowserWidget::onFlightSelected()
{
  auto* item = _flight_list->currentItem();
  if (!item)
  {
    return;
  }
  _current_flight_id = item->data(Qt::UserRole).toInt();
  _loaded_specs.clear();
  _parameters_imported = false;
  _field_tree->clear();
  updateLoadButton();
  setStatus(QString("Fetching ULog info for flight %1...").arg(_current_flight_id));

  const int requested_flight_id = _current_flight_id;
  QNetworkReply* reply =
      apiGet(QString("/api/analysis/flights/%1/ulog-info/").arg(_current_flight_id));
  connect(reply, &QNetworkReply::finished, this, [this, reply, requested_flight_id]() {
    reply->deleteLater();
    if (requested_flight_id != _current_flight_id)
    {
      return;  // user already selected another flight
    }
    if (reply->error() != QNetworkReply::NoError)
    {
      setStatus("ULog info error: " + reply->errorString() + " " +
                    QString::fromUtf8(reply->readAll().left(200)),
                true);
      return;
    }
    populateFieldTree(reply->readAll());
  });
}

void FmsBrowserWidget::populateFieldTree(const QByteArray& info_json)
{
  const QJsonObject info = QJsonDocument::fromJson(info_json).object();
  const QJsonArray datasets = info["datasets"].toArray();

  _instance_count.clear();
  for (const QJsonValue& value : datasets)
  {
    _instance_count[value.toObject()["name"].toString()] += 1;
  }

  _parameters.clear();
  const QJsonObject parameters = info["parameters"].toObject();
  for (auto it = parameters.begin(); it != parameters.end(); ++it)
  {
    _parameters[it.key()] = it.value().toDouble();
  }
  _log_start_time_s = info["start_timestamp_s"].toDouble();

  _field_tree->blockSignals(true);
  _field_tree->clear();
  for (const QJsonValue& value : datasets)
  {
    const QJsonObject dataset = value.toObject();
    const QString name = dataset["name"].toString();
    const int multi_id = dataset["multi_id"].toInt();
    const QString topic = topicLabel(name, multi_id);

    auto* topic_item = new QTreeWidgetItem(_field_tree, { topic });
    topic_item->setFlags(topic_item->flags() | Qt::ItemIsUserCheckable);
    topic_item->setCheckState(0, Qt::Unchecked);

    for (const QJsonValue& field_value : dataset["fields"].toArray())
    {
      const QJsonObject field = field_value.toObject();
      const QString field_name = field["name"].toString();
      if (field_name == "timestamp" || field["type"].toString() == "char")
      {
        continue;  // timestamps come with every series; char fields can't be plotted
      }
      auto* field_item = new QTreeWidgetItem(topic_item, { fieldLabel(field_name) });
      field_item->setFlags(field_item->flags() | Qt::ItemIsUserCheckable);
      field_item->setCheckState(0, Qt::Unchecked);
      field_item->setData(0, SPEC_ROLE,
                          QString("%1_%2.%3").arg(name).arg(multi_id).arg(field_name));
      field_item->setToolTip(0, field_name + " (" + field["type"].toString() + ")");
    }
  }
  _field_tree->blockSignals(false);
  applyFieldFilter(_field_filter_edit->text());
  setStatus(QString("Flight %1: %2 topics. Check fields, then load.")
                .arg(_current_flight_id)
                .arg(datasets.size()));
}

void FmsBrowserWidget::applyFieldFilter(const QString& text)
{
  for (int t = 0; t < _field_tree->topLevelItemCount(); t++)
  {
    QTreeWidgetItem* topic_item = _field_tree->topLevelItem(t);
    const QString topic = topic_item->text(0);
    const bool topic_match = topic.contains(text, Qt::CaseInsensitive);
    bool any_visible = false;
    for (int f = 0; f < topic_item->childCount(); f++)
    {
      QTreeWidgetItem* field_item = topic_item->child(f);
      const bool visible =
          topic_match || field_item->text(0).contains(text, Qt::CaseInsensitive);
      field_item->setHidden(!visible);
      any_visible |= visible;
    }
    topic_item->setHidden(!any_visible);
    if (!text.isEmpty() && any_visible && !topic_match)
    {
      topic_item->setExpanded(true);
    }
  }
}

void FmsBrowserWidget::updateLoadButton()
{
  int checked = 0;
  for (int t = 0; t < _field_tree->topLevelItemCount(); t++)
  {
    QTreeWidgetItem* topic_item = _field_tree->topLevelItem(t);
    for (int f = 0; f < topic_item->childCount(); f++)
    {
      checked += (topic_item->child(f)->checkState(0) == Qt::Checked) ? 1 : 0;
    }
  }
  _load_button->setEnabled(checked > 0 && !_loading);
  _load_button->setText(checked > 0 ? QString("Load %1 selected series").arg(checked) :
                                      "Load selected series");
}

void FmsBrowserWidget::loadSelectedSeries()
{
  _pending_specs.clear();
  int skipped = 0;
  for (int t = 0; t < _field_tree->topLevelItemCount(); t++)
  {
    QTreeWidgetItem* topic_item = _field_tree->topLevelItem(t);
    for (int f = 0; f < topic_item->childCount(); f++)
    {
      QTreeWidgetItem* field_item = topic_item->child(f);
      if (field_item->checkState(0) != Qt::Checked)
      {
        continue;
      }
      const QString spec = field_item->data(0, SPEC_ROLE).toString();
      if (_loaded_specs.count(spec))
      {
        skipped++;
      }
      else
      {
        _pending_specs.append(spec);
      }
    }
  }
  if (_pending_specs.isEmpty())
  {
    setStatus(skipped ? "All selected series are already loaded." : "Nothing selected.");
    return;
  }
  _loading = true;
  updateLoadButton();
  requestNextBatch();
}

void FmsBrowserWidget::requestNextBatch()
{
  QUrlQuery query;
  const int batch_size = std::min<int>(_pending_specs.size(), FIELDS_PER_REQUEST);
  for (int i = 0; i < batch_size; i++)
  {
    query.addQueryItem("field", _pending_specs[i]);
  }
  _pending_specs.erase(_pending_specs.begin(), _pending_specs.begin() + batch_size);

  setStatus(QString("Downloading %1 series (%2 remaining)...")
                .arg(batch_size)
                .arg(_pending_specs.size()));

  QNetworkReply* reply = apiGet(QString("/api/analysis/flights/%1/ulog-series/?%2")
                                    .arg(_current_flight_id)
                                    .arg(query.toString(QUrl::FullyEncoded)));
  connect(reply, &QNetworkReply::finished, this, [this, reply]() {
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError)
    {
      setStatus("Series download error: " + reply->errorString() + " " +
                    QString::fromUtf8(reply->readAll().left(200)),
                true);
      _pending_specs.clear();
      _loading = false;
      updateLoadButton();
      return;
    }
    importSeriesPayload(reply->readAll());
    if (!_pending_specs.isEmpty())
    {
      requestNextBatch();
    }
    else
    {
      _loading = false;
      updateLoadButton();
    }
  });
}

void FmsBrowserWidget::importSeriesPayload(const QByteArray& payload)
{
  if (payload.size() < 8 || !payload.startsWith("PJS1"))
  {
    setStatus("Unexpected series response format", true);
    return;
  }
  quint32 header_len = 0;
  std::memcpy(&header_len, payload.constData() + 4, 4);
  if (payload.size() < 8 + qint64(header_len))
  {
    setStatus("Truncated series response", true);
    return;
  }
  const QJsonObject header =
      QJsonDocument::fromJson(payload.mid(8, int(header_len))).object();

  PJ::PlotDataMapRef map;
  qint64 offset = 8 + header_len;
  int imported = 0;
  const QString prefix = seriesPrefix();

  for (const QJsonValue& value : header["series"].toArray())
  {
    const QJsonObject series = value.toObject();
    const int count = series["count"].toInt();
    if (payload.size() < offset + qint64(count) * 16)
    {
      setStatus("Truncated series response", true);
      return;
    }
    const QString topic =
        topicLabel(series["dataset"].toString(), series["multi_id"].toInt());
    const QString name = prefix + topic + "/" + fieldLabel(series["field"].toString());

    auto group = map.getOrCreateGroup((prefix + topic).toStdString());
    auto plot_it = map.addNumeric(name.toStdString(), group);

    // the payload is not necessarily 8-byte aligned, so copy before use
    std::vector<double> data(size_t(count) * 2);
    std::memcpy(data.data(), payload.constData() + offset, size_t(count) * 16);
    const double* timestamps = data.data();
    const double* values = data.data() + count;
    for (int i = 0; i < count; i++)
    {
      plot_it->second.pushBack(PJ::PlotData::Point(timestamps[i], values[i]));
    }
    offset += qint64(count) * 16;
    _loaded_specs.insert(series["spec"].toString());
    imported++;
  }

  if (_parameters_check->isChecked() && !_parameters_imported)
  {
    importParameters(map);
    _parameters_imported = true;
  }

  emitImport(map);

  QString message = QString("Imported %1 series from flight %2").arg(imported).arg(_current_flight_id);
  const QJsonArray missing = header["missing"].toArray();
  if (!missing.isEmpty())
  {
    message += QString(" (%1 unavailable)").arg(missing.size());
  }
  setStatus(message);
}

void FmsBrowserWidget::importParameters(PJ::PlotDataMapRef& map)
{
  const QString prefix = seriesPrefix();
  for (const auto& [name, value] : _parameters)
  {
    auto plot_it = map.addNumeric((prefix + "_parameters/" + name).toStdString());
    plot_it->second.pushBack(PJ::PlotData::Point(_log_start_time_s, value));
  }
}

void FmsBrowserWidget::emitImport(PJ::PlotDataMapRef& map)
{
  bool remove_old = false;
  if (_last_imported_flight_id != -1 && _last_imported_flight_id != _current_flight_id &&
      !_prefix_check->isChecked())
  {
    const auto answer = QMessageBox::question(
        this, "FMS Flight Browser",
        QString("Data from flight %1 is already loaded and the series names will collide.\n"
                "Clear the existing data?")
            .arg(_last_imported_flight_id),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
    remove_old = (answer == QMessageBox::Yes);
  }
  emit importData(map, remove_old);
  _last_imported_flight_id = _current_flight_id;
}

QString FmsBrowserWidget::topicLabel(const QString& dataset, int multi_id) const
{
  auto it = _instance_count.find(dataset);
  const bool multi = (it != _instance_count.end() && it->second > 1);
  if (!multi)
  {
    return dataset;
  }
  // same suffix convention as the ULog file loader: "topic.00", "topic.01", ...
  return dataset + QString(".%1").arg(multi_id, 2, 10, QChar('0'));
}

QString FmsBrowserWidget::fieldLabel(const QString& field)
{
  // convert array indices to the ULog file loader convention: "q[0]" -> "q.00"
  QString out = field;
  const QRegularExpression re("\\[(\\d+)\\]");
  int from = 0;
  QRegularExpressionMatch match;
  while ((match = re.match(out, from)).hasMatch())
  {
    const QString replacement =
        QString(".%1").arg(match.captured(1).toInt(), 2, 10, QChar('0'));
    out.replace(match.capturedStart(), match.capturedLength(), replacement);
    from = match.capturedStart() + replacement.size();
  }
  return out;
}

QString FmsBrowserWidget::seriesPrefix() const
{
  return _prefix_check->isChecked() ? QString("fms_%1/").arg(_current_flight_id) : QString();
}

void FmsBrowserWidget::setStatus(const QString& text, bool error)
{
  _status_label->setStyleSheet(error ? "color: red" : "");
  _status_label->setText(text);
}
