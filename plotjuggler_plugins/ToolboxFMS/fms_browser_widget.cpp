#include "fms_browser_widget.h"

#include <QCheckBox>
#include <QDebug>
#include <QComboBox>
#include <QDate>
#include <QDateEdit>
#include <QFormLayout>
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
#include <QProgressBar>
#include <QProgressDialog>
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
constexpr int FIELDS_PER_REQUEST = 50;
// Field count alone is a poor batch size: within one flight log the per-series
// sample count spans more than an order of magnitude, and since the tree groups
// same-dataset fields together, a hot dataset fills whole batches with its
// longest series. The download is only done when that straggler is, so cap the
// estimated payload too, using the sample counts from ulog-info.
//
// 32 MB rather than something tighter because each request costs ~0.37 s of
// server load and round trip: simulated on flight 9761 (2919 series, 691 MB),
// a 32 MB cap takes 23.7 s against 26.7 s uncapped, while an 8 MB cap turns
// 73 batches into 124 and ends up slower at 27.8 s. The tail matters more the
// more connections there are - the same cap is worth 33% at 8 connections.
// PJS1 size: float64 timestamp + float64 value. PJS2 shares timestamps and
// keeps native dtypes, so this is an upper bound there, which is the safe
// side for both the batch cap and the progress weights.
constexpr qint64 BYTES_PER_SAMPLE = 16;
constexpr qint64 MAX_BATCH_BYTES = 32 * 1024 * 1024;
// The FMS server answers 500 once the request URL passes ~3.6 kB, so cap the
// query too: field specs vary in length, and 50 long ones would overshoot.
constexpr int MAX_QUERY_CHARS = 1800;
// One connection tops out around 4 MB/s against FMS while the link has plenty
// of headroom, so keep several batches in flight: measured 4.2 MB/s on one
// connection against 12.7 MB/s across four.
//
// Simulated on flight 9761 (2919 series, 691 MB) with the 32 MB cap and gzip,
// the whole download takes 23.7 s at four connections and 12.6 s at eight.
//
// Qt's HTTP/1.1 connection cache is per QNetworkAccessManager and, as of Qt 5,
// holds six channels per host, so the last two batches of eight may sit queued
// inside Qt rather than on the wire. That shows up in the log as a batch whose
// time is long but whose rate is low, since the timer starts when we hand the
// request over. If it does, a second manager to round-robin across is the way
// out, not a bigger number here.
constexpr int MAX_CONCURRENT_REQUESTS = 8;
// Resolution of the progress bar; the weights behind it are bytes, far too
// many for a QProgressBar's int range on a long flight.
constexpr int PROGRESS_STEPS = 1000;
// Hardcoded on purpose. The deep link carries only a flight id, so a crafted
// plotjuggler://fms/flight/... URL cannot point this plugin - and the API token
// it sends - at somebody else's server.
constexpr const char* FMS_SERVER = "https://fms.aviant.no";

// Width in bytes of a numpy dtype string such as "<f4" or "|u1".
int dtypeSize(const QString& dtype)
{
  return dtype.mid(2).toInt();
}

// Widen `count` little-endian values of numpy dtype `dtype` to double. The
// payload is not necessarily aligned, so copy before use. False for a dtype
// the server never sends.
bool widenValues(const char* bytes, const QString& dtype, int count, std::vector<double>& out)
{
  auto widen = [&](auto tag) {
    using T = decltype(tag);
    std::vector<T> raw(static_cast<size_t>(count));
    std::memcpy(raw.data(), bytes, sizeof(T) * size_t(count));
    out.assign(raw.begin(), raw.end());
  };
  if (dtype == "<f8")
    widen(double{});
  else if (dtype == "<f4")
    widen(float{});
  else if (dtype == "<i8")
    widen(qint64{});
  else if (dtype == "<u8")
    widen(quint64{});
  else if (dtype == "<i4")
    widen(qint32{});
  else if (dtype == "<u4")
    widen(quint32{});
  else if (dtype == "<i2")
    widen(qint16{});
  else if (dtype == "<u2")
    widen(quint16{});
  else if (dtype == "|i1")
    widen(qint8{});
  else if (dtype == "|u1" || dtype == "|b1")
    widen(quint8{});
  else
    return false;
  return true;
}
}  // namespace

FmsBrowserWidget::FmsBrowserWidget(QWidget* parent) : QWidget(parent)
{
  _network = new QNetworkAccessManager(this);

  QSettings settings;
  QString default_token = qEnvironmentVariable("FMS_API_TOKEN");
  if (default_token.isEmpty())
  {
    default_token = settings.value("ToolboxFMS/token").toString();
  }

  _token_edit = new QLineEdit(default_token, this);
  _token_edit->setEchoMode(QLineEdit::Password);
  _token_edit->setPlaceholderText("FMS API token (or set FMS_API_TOKEN)");

  _aircraft_combo = new QComboBox(this);
  _aircraft_combo->addItem("Any aircraft", 0);

  _date_after_check = new QCheckBox("From:", this);
  _date_after_check->setChecked(true);
  _date_after_edit = new QDateEdit(QDate::currentDate().addDays(-30), this);
  _date_after_edit->setCalendarPopup(true);
  _date_after_edit->setDisplayFormat("yyyy-MM-dd");

  _date_before_check = new QCheckBox("To:", this);
  _date_before_edit = new QDateEdit(QDate::currentDate(), this);
  _date_before_edit->setCalendarPopup(true);
  _date_before_edit->setDisplayFormat("yyyy-MM-dd");
  _date_before_edit->setEnabled(false);

  _oneliner_edit = new QLineEdit(this);
  _oneliner_edit->setPlaceholderText("Oneliner contains...");

  _flight_id_edit = new QLineEdit(this);
  _flight_id_edit->setPlaceholderText("Flight ID");
  _flight_id_edit->setToolTip("Look up a single flight by ID; ignores the other filters");

  _ground_tests_check = new QCheckBox("Hide ground tests", this);

  _filter_edit = new QLineEdit(this);
  _filter_edit->setPlaceholderText("Extra filters, e.g. px4_version=1.14&payload=1");
  _filter_edit->setToolTip("Raw query filters, same syntax as check_remote_flights --filter.\n"
                           "Appended to the form above.");

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
  _download_all_button = new QPushButton("Download all", this);
  _cancel_button = new QPushButton("Cancel", this);
  _cancel_button->setEnabled(false);
  _download_all_button->setEnabled(false);
  _download_all_button->setToolTip("Download every series of this flight");
  auto* close_button = new QPushButton("Close", this);

  _status_label = new QLabel(this);
  _progress_bar = new QProgressBar(this);
  _progress_bar->setRange(0, PROGRESS_STEPS);
  _progress_bar->setTextVisible(true);
  _progress_bar->hide();
  _status_label->setWordWrap(true);

  auto* token_row = new QHBoxLayout();
  token_row->addWidget(new QLabel("Token:", this));
  token_row->addWidget(_token_edit, 1);

  auto* dates_row = new QHBoxLayout();
  dates_row->addWidget(_date_after_check);
  dates_row->addWidget(_date_after_edit, 1);
  dates_row->addWidget(_date_before_check);
  dates_row->addWidget(_date_before_edit, 1);

  auto* id_row = new QHBoxLayout();
  id_row->addWidget(_flight_id_edit, 1);
  id_row->addWidget(_ground_tests_check);

  auto* filter_form = new QFormLayout();
  filter_form->setContentsMargins(0, 0, 0, 0);
  filter_form->addRow("Aircraft:", _aircraft_combo);
  filter_form->addRow("Date:", dates_row);
  filter_form->addRow("Text:", _oneliner_edit);
  filter_form->addRow("Flight:", id_row);
  filter_form->addRow("More:", _filter_edit);

  auto* filter_row = new QHBoxLayout();
  filter_row->addStretch(1);
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
  buttons_row->addWidget(_download_all_button);
  buttons_row->addWidget(_cancel_button);
  buttons_row->addWidget(close_button);

  auto* main_layout = new QVBoxLayout(this);
  main_layout->addLayout(token_row);
  main_layout->addLayout(filter_form);
  main_layout->addLayout(filter_row);
  main_layout->addWidget(splitter, 1);
  main_layout->addLayout(options_row);
  main_layout->addLayout(buttons_row);
  main_layout->addWidget(_progress_bar);
  main_layout->addWidget(_status_label);

  connect(_search_button, &QPushButton::clicked, this, &FmsBrowserWidget::searchFlights);
  connect(_filter_edit, &QLineEdit::returnPressed, this, &FmsBrowserWidget::searchFlights);
  connect(_oneliner_edit, &QLineEdit::returnPressed, this, &FmsBrowserWidget::searchFlights);
  connect(_flight_id_edit, &QLineEdit::returnPressed, this, &FmsBrowserWidget::searchFlights);
  connect(_date_after_check, &QCheckBox::toggled, _date_after_edit, &QWidget::setEnabled);
  connect(_date_before_check, &QCheckBox::toggled, _date_before_edit, &QWidget::setEnabled);
  connect(_flight_list, &QListWidget::itemSelectionChanged, this,
          &FmsBrowserWidget::onFlightSelected);
  connect(_load_button, &QPushButton::clicked, this, &FmsBrowserWidget::loadSelectedSeries);
  connect(_download_all_button, &QPushButton::clicked, this, &FmsBrowserWidget::downloadAll);
  connect(_cancel_button, &QPushButton::clicked, this, &FmsBrowserWidget::cancelDownload);
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
  if (_flight_list->count() == 0 && !_link_in_progress)
  {
    searchFlights();
  }
}

void FmsBrowserWidget::openFlightFromLink(const QString& flight_id)
{
  _link_in_progress = true;
  _flight_id_edit->setText(flight_id);
  if (_token_edit->text().trimmed().isEmpty())
  {
    // Nothing can be fetched without a token, and the panel is the only place
    // to type one. Once typed, Search runs the flight-id filter as usual.
    setStatus("Enter the FMS API token to open flight " + flight_id, true);
    // setStatus() raised the panel and dropped the flag; keep it so the flight
    // still downloads in full once the token is typed and Search is pressed.
    _link_in_progress = true;
    return;
  }
  searchFlights();
}

QNetworkReply* FmsBrowserWidget::apiGet(const QString& path_and_query)
{
  QNetworkRequest request(QUrl(QString(FMS_SERVER) + path_and_query));
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                       QNetworkRequest::NoLessSafeRedirectPolicy);
  // Deliberately NOT setting Accept-Encoding. Qt adds "gzip, deflate" itself
  // and only then sets autoDecompress on the request, so asking for gzip by
  // hand gets the compressed bytes handed back undecoded - which reads as a
  // corrupt payload, not as an error.
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
      populateAircraftCombo();
    }
    requestFlightList();
  });
}

void FmsBrowserWidget::populateAircraftCombo()
{
  const int selected = _aircraft_combo->currentData().toInt();
  _aircraft_combo->clear();
  _aircraft_combo->addItem("Any aircraft", 0);
  for (const auto& [id, name] : _aircraft_names)
  {
    _aircraft_combo->addItem(name, id);
  }
  const int index = _aircraft_combo->findData(selected);
  _aircraft_combo->setCurrentIndex(index >= 0 ? index : 0);
}

QString FmsBrowserWidget::buildFilterQuery() const
{
  QStringList terms;
  // An explicit flight ID is a lookup, not a filter: everything else would
  // only narrow a single-flight result.
  const QString flight_id = _flight_id_edit->text().trimmed();
  if (!flight_id.isEmpty())
  {
    terms << "id=" + QString::fromUtf8(QUrl::toPercentEncoding(flight_id));
  }
  else
  {
    const int aircraft_id = _aircraft_combo->currentData().toInt();
    if (aircraft_id > 0)
    {
      terms << QString("aircraft=%1").arg(aircraft_id);
    }
    if (_date_after_check->isChecked())
    {
      terms << "date_after=" + _date_after_edit->date().toString(Qt::ISODate);
    }
    if (_date_before_check->isChecked())
    {
      terms << "date_before=" + _date_before_edit->date().toString(Qt::ISODate);
    }
    const QString oneliner = _oneliner_edit->text().trimmed();
    if (!oneliner.isEmpty())
    {
      terms << "oneliner=" + QString::fromUtf8(QUrl::toPercentEncoding(oneliner));
    }
    if (_ground_tests_check->isChecked())
    {
      terms << "hide_ground_tests=1";
    }
  }
  const QString extra = _filter_edit->text().trimmed();
  if (!extra.isEmpty())
  {
    terms << extra;
  }
  terms << "no_page=1";
  return terms.join("&");
}

void FmsBrowserWidget::requestFlightList()
{
  QNetworkReply* reply = apiGet("/api/flight/?" + buildFilterQuery());
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
    auto* item = new QListWidgetItem(QString("%1  %2  %3: %4").arg(flight["name"].toString(),
                                                                   start_time, aircraft,
                                                                   flight["oneliner"].toString()),
                                     _flight_list);
    item->setData(Qt::UserRole, id);
    item->setToolTip(QString("Flight %1").arg(id));
  }
  // No flight for a deep link's id is a dead end the user has to see.
  setStatus(QString("%1 flight(s) found").arg(flights.size()),
            flights.isEmpty() && _link_in_progress);

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
  _sample_counts.clear();
  for (const QJsonValue& value : datasets)
  {
    const QJsonObject dataset = value.toObject();
    const QString name = dataset["name"].toString();
    _instance_count[name] += 1;
    _sample_counts[QString("%1_%2").arg(name).arg(dataset["multi_id"].toInt())] =
        dataset["sample_count"].toVariant().toLongLong();
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
  updateLoadButton();
  setStatus(QString("Flight %1: %2 topics. Check fields, then load.")
                .arg(_current_flight_id)
                .arg(datasets.size()));

  if (_link_in_progress)
  {
    // Opened from an FMS deep link: load the whole flight without waiting for
    // a click. Only for the flight the link named, not for later selections.
    downloadAll();
  }
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
  _download_all_button->setEnabled(_field_tree->topLevelItemCount() > 0 && !_loading);
  _cancel_button->setEnabled(_loading);
}

QStringList FmsBrowserWidget::allSpecs() const
{
  QStringList specs;
  for (int t = 0; t < _field_tree->topLevelItemCount(); t++)
  {
    QTreeWidgetItem* topic_item = _field_tree->topLevelItem(t);
    for (int f = 0; f < topic_item->childCount(); f++)
    {
      specs.append(topic_item->child(f)->data(0, SPEC_ROLE).toString());
    }
  }
  return specs;
}

void FmsBrowserWidget::startDownload(const QStringList& specs, int already_loaded)
{
  if (specs.isEmpty())
  {
    setStatus(already_loaded ? "All selected series are already loaded." : "Nothing selected.");
    return;
  }
  _pending_specs = specs;
  _loading = true;
  _progress_total = 0;
  _progress_done = 0;
  for (const QString& spec : specs)
  {
    _progress_total += progressWeight(spec);
  }
  _progress_bar->setValue(0);
  _progress_bar->show();
  if (!isVisible())
  {
    // Deep link: the panel is hidden and the user is looking at the plot view,
    // so give the download a face of its own there. Non-modal, so the plots
    // that have already landed stay usable while the rest arrives.
    delete _progress_dialog;
    _progress_dialog =
        new QProgressDialog(QString("Downloading flight %1...").arg(_current_flight_id), "Cancel",
                            0, PROGRESS_STEPS, window());
    _progress_dialog->setWindowTitle("FMS Flight Browser");
    _progress_dialog->setWindowModality(Qt::NonModal);
    _progress_dialog->setMinimumDuration(0);
    _progress_dialog->setAutoClose(false);
    _progress_dialog->setAutoReset(false);
    _progress_dialog->setValue(0);
    connect(_progress_dialog, &QProgressDialog::canceled, this, &FmsBrowserWidget::cancelDownload);
  }
  _downloaded_bytes = 0;
  _wire_bytes = 0;
  _downloaded_series = 0;
  _downloaded_samples = 0;
  _wait_ms = 0;
  _parse_ms = 0;
  _import_ms = 0;
  _download_timer.start();
  qDebug() << "[ToolboxFMS] download start: flight" << _current_flight_id << "series"
           << specs.size() << "already loaded" << already_loaded << "concurrency"
           << MAX_CONCURRENT_REQUESTS;
  updateLoadButton();
  pumpRequests();
}

void FmsBrowserWidget::loadSelectedSeries()
{
  QStringList specs;
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
        specs.append(spec);
      }
    }
  }
  startDownload(specs, skipped);
}

void FmsBrowserWidget::downloadAll()
{
  QStringList specs;
  int skipped = 0;
  for (const QString& spec : allSpecs())
  {
    if (_loaded_specs.count(spec))
    {
      skipped++;
    }
    else
    {
      specs.append(spec);
    }
  }
  if (specs.isEmpty())
  {
    setStatus(skipped ? "All series of this flight are already loaded." : "Nothing to download.");
    _link_in_progress = false;
    return;
  }
  // The whole flight is what was asked for, so there is nothing left to do in
  // the panel: go back to the plots and let the progress dialog take over.
  // The host hides this widget synchronously on closed(), which is what makes
  // startDownload() put up the dialog.
  emit closed();
  startDownload(specs, skipped);
}

void FmsBrowserWidget::pumpRequests()
{
  while (_in_flight < MAX_CONCURRENT_REQUESTS && !_pending_specs.isEmpty())
  {
    requestNextBatch();
  }
  if (_in_flight == 0)
  {
    _loading = false;
    _link_in_progress = false;
    finishProgress();
    const qint64 total_ms = qMax<qint64>(_download_timer.elapsed(), 1);
    qDebug().nospace() << "[ToolboxFMS] download done: " << _downloaded_series << " series, "
                       << _downloaded_samples << " samples, " << _downloaded_bytes / 1024 / 1024
                       << " MiB (" << _wire_bytes / 1024 / 1024 << " MiB on the wire) in "
                       << total_ms << " ms (" << (_wire_bytes / 1024 * 1000 / total_ms)
                       << " KiB/s) - summed across connections: waiting " << _wait_ms
                       << " ms, parsing " << _parse_ms << " ms, importing " << _import_ms << " ms";
    updateLoadButton();
  }
}

qint64 FmsBrowserWidget::estimatedBytes(const QString& spec) const
{
  // A spec is '<dataset>_<multi_id>.<field>', and dataset names have no dot,
  // so the key is everything before the first one.
  // 0 when the server is older than the sample_count field, which just falls
  // back to batching by field count.
  const auto it = _sample_counts.find(spec.left(spec.indexOf('.')));
  return it == _sample_counts.end() ? 0 : it->second * BYTES_PER_SAMPLE;
}

qint64 FmsBrowserWidget::progressWeight(const QString& spec) const
{
  // At least one unit per series, so an old server without sample counts
  // still moves the bar - by series instead of by bytes.
  return std::max<qint64>(estimatedBytes(spec), 1);
}

void FmsBrowserWidget::requestNextBatch()
{
  QUrlQuery query;
  // Shared timestamps and native dtypes; an older server ignores this and
  // answers PJS1, which importSeriesPayload() still takes. Not "format":
  // Django REST Framework owns that parameter and 404s on unknown values.
  query.addQueryItem("layout", "pjs2");
  const int max_batch = std::min<int>(_pending_specs.size(), FIELDS_PER_REQUEST);
  int batch_size = 0;
  qint64 batch_bytes = 0;
  qint64 batch_weight = 0;
  while (batch_size < max_batch)
  {
    const qint64 spec_bytes = estimatedBytes(_pending_specs[batch_size]);
    QUrlQuery candidate = query;
    candidate.addQueryItem("field", _pending_specs[batch_size]);
    // always send at least one spec, however long or large it is
    if (batch_size > 0 && (candidate.toString(QUrl::FullyEncoded).size() > MAX_QUERY_CHARS ||
                           batch_bytes + spec_bytes > MAX_BATCH_BYTES))
    {
      break;
    }
    query = candidate;
    batch_bytes += spec_bytes;
    batch_weight += progressWeight(_pending_specs[batch_size]);
    batch_size++;
  }
  _pending_specs.erase(_pending_specs.begin(), _pending_specs.begin() + batch_size);

  setStatus(QString("Downloading %1 series (%2 remaining)...")
                .arg(batch_size)
                .arg(_pending_specs.size()));

  // Relative to the download timer, so each in-flight batch can be timed
  // without a timer per request.
  const qint64 request_started_ms = _download_timer.elapsed();
  _in_flight++;
  QNetworkReply* reply = apiGet(QString("/api/analysis/flights/%1/ulog-series/?%2")
                                    .arg(_current_flight_id)
                                    .arg(query.toString(QUrl::FullyEncoded)));
  connect(reply, &QNetworkReply::finished, this,
          [this, reply, request_started_ms, batch_weight]() {
    reply->deleteLater();
    const qint64 waited_ms = _download_timer.elapsed() - request_started_ms;
    _in_flight--;
    _progress_done += batch_weight;
    if (reply->error() != QNetworkReply::NoError)
    {
      setStatus("Series download error: " + reply->errorString() + " " +
                    QString::fromUtf8(reply->readAll().left(200)),
                true);
      // Stop sending more, but let the batches already in flight land: their
      // data is fine, and they have been paid for already.
      _pending_specs.clear();
      updateProgress();
      pumpRequests();
      return;
    }
    _wait_ms += waited_ms;
    const QByteArray payload = reply->readAll();
    // Qt hands back decompressed bytes, so take the wire size from the header
    // the server sent. Qt may drop Content-Length once it decodes, and the two
    // are equal when the response was not compressed at all, so only believe
    // it when it is actually smaller - otherwise report the payload and say so.
    const qint64 header_bytes = reply->rawHeader("Content-Length").toLongLong();
    const bool wire_known = header_bytes > 0 && header_bytes < payload.size();
    const qint64 sent = wire_known ? header_bytes : payload.size();
    _wire_bytes += sent;
    qDebug().nospace().noquote() << "[ToolboxFMS] batch: " << payload.size() / 1024 << " KiB ("
                       << (wire_known ? QString("%1 KiB on the wire, %2%")
                                            .arg(sent / 1024)
                                            .arg(payload.size() > 0 ? sent * 100 / payload.size() : 100)
                                      : QString("wire size unknown"))
                       << ") in " << waited_ms << " ms ("
                       << (waited_ms > 0 ? sent / 1024 * 1000 / waited_ms : 0) << " KiB/s )";
    importSeriesPayload(payload);
    updateProgress();
    pumpRequests();
  });
}

void FmsBrowserWidget::addSeries(PJ::PlotDataMapRef& map, const QString& dataset, int multi_id,
                                 const QString& field, const double* timestamps,
                                 const double* values, int count)
{
  const QString prefix = seriesPrefix();
  const QString topic = topicLabel(dataset, multi_id);
  const QString name = prefix + topic + "/" + fieldLabel(field);
  auto group = map.getOrCreateGroup((prefix + topic).toStdString());
  auto plot_it = map.addNumeric(name.toStdString(), group);
  for (int i = 0; i < count; i++)
  {
    plot_it->second.pushBack(PJ::PlotData::Point(timestamps[i], values[i]));
  }
  _downloaded_samples += count;
}

void FmsBrowserWidget::importSeriesPayload(const QByteArray& payload)
{
  const bool pjs2 = payload.startsWith("PJS2");
  if (payload.size() < 8 || !(pjs2 || payload.startsWith("PJS1")))
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

  QElapsedTimer parse_timer;
  parse_timer.start();
  PJ::PlotDataMapRef map;
  qint64 offset = 8 + header_len;
  int imported = 0;
  auto remaining = [&](qint64 needed) {
    if (payload.size() < offset + needed)
    {
      setStatus("Truncated series response", true);
      return false;
    }
    return true;
  };

  if (pjs2)
  {
    // Per dataset: int64 microsecond deltas (first absolute), then each field
    // in its own dtype. See ULogSeriesView in flight-management.
    std::vector<qint64> deltas;
    std::vector<double> timestamps;
    std::vector<double> values;
    for (const QJsonValue& value : header["datasets"].toArray())
    {
      const QJsonObject dataset = value.toObject();
      const int count = dataset["count"].toInt();
      if (!remaining(qint64(count) * 8))
      {
        return;
      }
      deltas.resize(size_t(count));
      std::memcpy(deltas.data(), payload.constData() + offset, size_t(count) * 8);
      offset += qint64(count) * 8;
      timestamps.resize(size_t(count));
      qint64 t_us = 0;
      for (int i = 0; i < count; i++)
      {
        t_us += deltas[size_t(i)];
        timestamps[size_t(i)] = double(t_us) / 1e6;
      }
      for (const QJsonValue& field_value : dataset["fields"].toArray())
      {
        const QJsonObject field = field_value.toObject();
        const QString dtype = field["dtype"].toString();
        const qint64 bytes = qint64(count) * dtypeSize(dtype);
        if (!remaining(bytes))
        {
          return;
        }
        if (!widenValues(payload.constData() + offset, dtype, count, values))
        {
          setStatus("Unsupported series dtype " + dtype, true);
          return;
        }
        offset += bytes;
        addSeries(map, dataset["dataset"].toString(), dataset["multi_id"].toInt(),
                  field["field"].toString(), timestamps.data(), values.data(), count);
        _loaded_specs.insert(field["spec"].toString());
        imported++;
      }
    }
  }
  else
  {
    // Per series: float64 seconds, then float64 values.
    std::vector<double> data;
    for (const QJsonValue& value : header["series"].toArray())
    {
      const QJsonObject series = value.toObject();
      const int count = series["count"].toInt();
      if (!remaining(qint64(count) * 16))
      {
        return;
      }
      data.resize(size_t(count) * 2);
      std::memcpy(data.data(), payload.constData() + offset, size_t(count) * 16);
      offset += qint64(count) * 16;
      addSeries(map, series["dataset"].toString(), series["multi_id"].toInt(),
                series["field"].toString(), data.data(), data.data() + count, count);
      _loaded_specs.insert(series["spec"].toString());
      imported++;
    }
  }

  if (_parameters_check->isChecked() && !_parameters_imported)
  {
    importParameters(map);
    _parameters_imported = true;
  }
  _parse_ms += parse_timer.elapsed();
  _downloaded_bytes += payload.size();
  _downloaded_series += imported;

  QElapsedTimer import_timer;
  import_timer.start();
  emitImport(map);
  _import_ms += import_timer.elapsed();
  qDebug() << "[ToolboxFMS]  " << imported << "series parsed in" << parse_timer.elapsed()
           << "ms, imported in" << import_timer.elapsed() << "ms";

  QString message =
      QString("Imported %1 series from flight %2").arg(imported).arg(_current_flight_id);
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

void FmsBrowserWidget::cancelDownload()
{
  if (!_loading)
  {
    return;
  }
  // Batches already in flight still land and import: their data is fine.
  const int dropped = _pending_specs.size();
  _pending_specs.clear();
  _link_in_progress = false;
  setStatus(QString("Cancelled, %1 series not downloaded. Waiting for %2 batch(es) in flight...")
                .arg(dropped)
                .arg(_in_flight));
  if (_progress_dialog)
  {
    _progress_dialog->setLabelText("Cancelling, waiting for batches in flight...");
  }
  pumpRequests();
}

void FmsBrowserWidget::updateProgress()
{
  const int steps = _progress_total > 0 ?
                        static_cast<int>(_progress_done * PROGRESS_STEPS / _progress_total) :
                        PROGRESS_STEPS;
  _progress_bar->setValue(std::min(steps, PROGRESS_STEPS));
  if (_progress_dialog && !_progress_dialog->wasCanceled())
  {
    _progress_dialog->setValue(std::min(steps, PROGRESS_STEPS));
    _progress_dialog->setLabelText(QString("Downloading flight %1: %2 series, %3 MiB so far "
                                           "(%4 remaining)")
                                       .arg(_current_flight_id)
                                       .arg(_downloaded_series)
                                       .arg(_downloaded_bytes / 1024 / 1024)
                                       .arg(_pending_specs.size() + _in_flight));
  }
}

void FmsBrowserWidget::finishProgress()
{
  _progress_bar->hide();
  if (_progress_dialog)
  {
    _progress_dialog->deleteLater();
    _progress_dialog = nullptr;
  }
}

void FmsBrowserWidget::setStatus(const QString& text, bool error)
{
  _status_label->setStyleSheet(error ? "color: red" : "");
  _status_label->setText(text);
  if (error && _link_in_progress)
  {
    // The user clicked a link and is looking at an empty plot view: a red
    // label in a hidden panel would go unseen, so bring the panel up.
    _link_in_progress = false;
    emit showRequested();
  }
}
