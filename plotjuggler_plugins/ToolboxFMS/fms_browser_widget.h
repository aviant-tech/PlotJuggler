#pragma once

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

signals:
  void importData(PJ::PlotDataMapRef& data, bool remove_old);
  void closed();

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
  void requestNextBatch();
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
  // specs already imported for the current flight, to avoid duplicated points
  std::set<QString> _loaded_specs;
  // parameters of the currently selected flight, imported as one-point series
  std::map<QString, double> _parameters;
  double _log_start_time_s = 0.0;
  bool _parameters_imported = false;

  QStringList _pending_specs;
  bool _loading = false;
  bool _env_flight_consumed = false;
  // set when a deep link picked the flight, so its series load without a click
  bool _auto_download_all = false;
};
