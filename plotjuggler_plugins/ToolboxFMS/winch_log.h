#ifndef FMS_WINCH_LOG_H
#define FMS_WINCH_LOG_H

#include <QByteArray>
#include <QString>
#include <map>
#include <utility>
#include <vector>

namespace fms
{
/// The contents of the first file of a zip archive (stored or deflated, zip64
/// sizes too), as FMS' download_logs sends a log. Empty on error, with the
/// reason in *error.
QByteArray unzipFirstFile(const QByteArray& zip, QString* error);

using Samples = std::vector<std::pair<double, double>>;

/// The winch log CSV (winch-control's telemetry logger) as series named like
/// the "aviant_winch" dataset FMS merges into a combined ULog: the numeric
/// columns, State_<state> and MotorState_<state> one-hot, Vendor_Mode and
/// Vendor_Fault. Times are UTC seconds minus utc_at_boot_s, so PX4 boot time;
/// samples before boot are dropped, as FMS does.
std::map<QString, Samples> parseWinchLog(const QByteArray& csv, double utc_at_boot_s);
}  // namespace fms

#endif  // FMS_WINCH_LOG_H
