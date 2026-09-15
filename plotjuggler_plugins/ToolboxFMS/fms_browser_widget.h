#pragma once

#include <QElapsedTimer>
#include <QWidget>
#include <map>
#include <set>

#include "PlotJuggler/plotdata.h"

class QCheckBox;
class QComboBox;
class QDateEdit;
class QLabel;
class QLineEdit;
class QListWidget;
class QNetworkAccessManager;
class QNetworkReply;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

/**
 * Browser for flights stored in the Aviant FMS.
 *
 * Lists flights through the regular flight API, then uses the
 * api/analysis/flights/<id>/ulog-info and ulog-series endpoints to show the
 * available ULog fields and download only the selected time series, instead
 * of downloading the entire .ulg file.
 */
class FmsBrowserWidget : public QWidget
{
  Q_OBJECT

public:
  explicit FmsBrowserWidget(QWidget* parent = nullptr);

  /// Called every time the toolbox is opened from the Tools menu.
  void onShow();

  /// Opened from an FMS deep link: select the flight and download all of it
  /// without the panel being shown. Emits showRequested() only if something
  /// needs the user - no token, no such flight, or an error.
  void openFlightFromLink(const QString& flight_id);

signals:
  void importData(PJ::PlotDataMapRef& data, bool remove_old);
  void closed();
  /// The panel has something the user has to see (an error, a token prompt).
  void showRequested();

private slots:
  void searchFlights();
  void onFlightSelected();
  void loadSelectedSeries();
  void downloadAllSeries();

private:
  QNetworkReply* apiGet(const QString& path_and_query);
  void requestFlightList();
  QString buildFilterQuery() const;
  void populateAircraftCombo();
  void populateFlightList(const QByteArray& flights_json);
  void populateFieldTree(const QByteArray& info_json);
  void applyFieldFilter(const QString& text);
  void pumpRequests();
  void requestNextBatch();
  qint64 estimatedBytes(const QString& spec) const;
  void startDownload(const QStringList& specs, int already_loaded);
  QStringList allSpecs() const;
  void downloadAll(bool confirm);
  void importSeriesPayload(const QByteArray& payload);
  void importParameters(PJ::PlotDataMapRef& map);
  void emitImport(PJ::PlotDataMapRef& map);
  void setStatus(const QString& text, bool error = false);
  void updateLoadButton();

  QString topicLabel(const QString& dataset, int multi_id) const;
  static QString fieldLabel(const QString& field);
  QString seriesPrefix() const;

  QLineEdit* _token_edit;
  QLineEdit* _filter_edit;  // extra raw "key=value&..." filters
  QComboBox* _aircraft_combo;
  QDateEdit* _date_after_edit;
  QDateEdit* _date_before_edit;
  QCheckBox* _date_after_check;
  QCheckBox* _date_before_check;
  QCheckBox* _ground_tests_check;
  QLineEdit* _oneliner_edit;
  QLineEdit* _flight_id_edit;
  QLineEdit* _field_filter_edit;
  QListWidget* _flight_list;
  QTreeWidget* _field_tree;
  QCheckBox* _parameters_check;
  QCheckBox* _prefix_check;
  QPushButton* _search_button;
  QPushButton* _load_button;
  QPushButton* _download_all_button;
  QLabel* _status_label;

  QNetworkAccessManager* _network;

  int _current_flight_id = -1;
  int _last_imported_flight_id = -1;
  std::map<int, QString> _aircraft_names;
  // number of multi-id instances per dataset, used for the ".00" suffix
  std::map<QString, int> _instance_count;
  // '<dataset>_<multi_id>' -> samples, from ulog-info, for sizing batches
  std::map<QString, qint64> _sample_counts;
  // specs already imported for the current flight, to avoid duplicated points
  std::set<QString> _loaded_specs;
  // parameters of the currently selected flight, imported as one-point series
  std::map<QString, double> _parameters;
  double _log_start_time_s = 0.0;
  bool _parameters_imported = false;

  QStringList _pending_specs;
  int _in_flight = 0;
  // profiling: see qDebug() output tagged [ToolboxFMS]
  QElapsedTimer _download_timer;
  qint64 _downloaded_bytes = 0;
  qint64 _wire_bytes = 0;  // after gzip, i.e. what the link actually carried
  int _downloaded_series = 0;
  int _downloaded_samples = 0;
  qint64 _wait_ms = 0;
  qint64 _parse_ms = 0;
  qint64 _import_ms = 0;
  bool _loading = false;
  // set while a deep link drives the widget with the panel hidden: the flight
  // it named loads without a click, and problems surface via showRequested()
  bool _link_in_progress = false;
};
