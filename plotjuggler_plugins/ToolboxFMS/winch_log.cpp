#include "winch_log.h"

#include <QDateTime>
#include <QList>
#include <QRegularExpression>
#include <QSet>
#include <cmath>
#include <cstring>
#include <limits>
#include <zlib.h>

namespace fms
{
namespace
{
quint16 read16(const QByteArray& data, qint64 offset)
{
  quint16 value = 0;
  std::memcpy(&value, data.constData() + offset, 2);
  return value;
}

quint32 read32(const QByteArray& data, qint64 offset)
{
  quint32 value = 0;
  std::memcpy(&value, data.constData() + offset, 4);
  return value;
}

quint64 read64(const QByteArray& data, qint64 offset)
{
  quint64 value = 0;
  std::memcpy(&value, data.constData() + offset, 8);
  return value;
}

QByteArray fail(QString* error, const QString& reason)
{
  *error = reason;
  return {};
}

double number(const QByteArray& text)
{
  bool ok = false;
  const double value = text.toDouble(&ok);
  return ok ? value : std::numeric_limits<double>::quiet_NaN();  // "nan", "None"
}

/// Seconds since the epoch of "2026-10-11T16:54:12.338340Z"
double utcSeconds(const QByteArray& text)
{
  const QDateTime time = QDateTime::fromString(QString::fromLatin1(text.left(19)), Qt::ISODate);
  if (!time.isValid())
  {
    return std::numeric_limits<double>::quiet_NaN();
  }
  QDateTime utc = time;
  utc.setTimeSpec(Qt::UTC);
  double fraction = 0;
  if (text.size() > 20 && text[19] == '.')
  {
    fraction = number("0" + text.mid(19).replace("Z", ""));
  }
  return double(utc.toSecsSinceEpoch()) + fraction;
}
}  // namespace

QByteArray unzipFirstFile(const QByteArray& zip, QString* error)
{
  // the end of central directory record, before an optional comment
  qint64 end = -1;
  for (qint64 i = zip.size() - 22; i >= 0 && i >= zip.size() - 22 - 0xFFFF; i--)
  {
    if (read32(zip, i) == 0x06054b50)
    {
      end = i;
      break;
    }
  }
  if (end < 0)
  {
    return fail(error, "not a zip file");
  }
  quint64 directory = read32(zip, end + 16);
  if (directory == 0xFFFFFFFF && end >= 20 && read32(zip, end - 20) == 0x07064b50)
  {
    const quint64 end64 = read64(zip, end - 20 + 8);
    if (end64 + 56 > quint64(zip.size()) || read32(zip, qint64(end64)) != 0x06064b50)
    {
      return fail(error, "bad zip64 directory");
    }
    directory = read64(zip, qint64(end64) + 48);
  }
  if (directory + 46 > quint64(zip.size()) || read32(zip, qint64(directory)) != 0x02014b50)
  {
    return fail(error, "bad zip directory");
  }

  const qint64 entry = qint64(directory);
  const quint16 method = read16(zip, entry + 10);
  quint64 compressed = read32(zip, entry + 20);
  quint64 size = read32(zip, entry + 24);
  const quint16 name_length = read16(zip, entry + 28);
  const quint16 extra_length = read16(zip, entry + 30);
  quint64 local = read32(zip, entry + 42);
  // zip64: the sizes and offset that did not fit, in this order
  for (qint64 extra = entry + 46 + name_length;
       extra + 4 <= entry + 46 + name_length + extra_length;)
  {
    const quint16 id = read16(zip, extra);
    const quint16 length = read16(zip, extra + 2);
    if (id == 0x0001)
    {
      qint64 field = extra + 4;
      for (quint64* value : { &size, &compressed, &local })
      {
        if (*value == 0xFFFFFFFF && field + 8 <= extra + 4 + length)
        {
          *value = read64(zip, field);
          field += 8;
        }
      }
    }
    extra += 4 + length;
  }

  if (local + 30 > quint64(zip.size()) || read32(zip, qint64(local)) != 0x04034b50)
  {
    return fail(error, "bad zip entry");
  }
  const quint64 data =
      local + 30 + read16(zip, qint64(local) + 26) + read16(zip, qint64(local) + 28);
  if (data + compressed > quint64(zip.size()) || size > 1024 * 1024 * 1024)
  {
    return fail(error, "truncated zip file");
  }
  if (method == 0)
  {
    return zip.mid(qint64(data), qint64(size));
  }
  if (method != 8)
  {
    return fail(error, QString("unsupported zip compression %1").arg(method));
  }

  QByteArray out(qint64(size), Qt::Uninitialized);
  z_stream stream{};
  if (inflateInit2(&stream, -MAX_WBITS) != Z_OK)  // raw deflate, no zlib header
  {
    return fail(error, "zlib error");
  }
  stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(zip.constData() + data));
  stream.avail_in = uInt(compressed);
  stream.next_out = reinterpret_cast<Bytef*>(out.data());
  stream.avail_out = uInt(size);
  const int result = inflate(&stream, Z_FINISH);
  inflateEnd(&stream);
  if (result != Z_STREAM_END || stream.total_out != size)
  {
    return fail(error, "corrupt zip data");
  }
  return out;
}

std::map<QString, Samples> parseWinchLog(const QByteArray& csv, double utc_at_boot_s)
{
  // Time,State,MotorState,Position,Velocity,Torque,Force,Voltage,Temperature,Vendor
  // where Vendor is "{'mode': '10', 'fault': '0'}", commas and all.
  QList<QByteArray> lines = csv.split('\n');
  std::map<QString, Samples> series;
  if (lines.size() < 4)
  {
    return series;
  }
  const QList<QByteArray> header = lines[0].trimmed().split(',');
  const int vendor = header.indexOf("Vendor");
  // FMS skips the second line (the init state, all NaN) and the last one,
  // often cut off when the log was written
  lines = lines.mid(2, lines.size() - 3);

  struct Row
  {
    double t;
    QList<QByteArray> columns;
  };
  std::vector<Row> rows;
  for (const QByteArray& line : lines)
  {
    Row row{ 0, line.trimmed().split(',') };
    if (row.columns.size() < header.size())
    {
      continue;
    }
    row.t = utcSeconds(row.columns[0]) - utc_at_boot_s;
    if (row.t > 0)
    {
      rows.push_back(std::move(row));
    }
  }

  const QRegularExpression vendor_field("'(mode|fault)':\\s*'?(-?\\d+)");
  for (int i = 1; i < header.size(); i++)
  {
    const QString column = QString::fromLatin1(header[i]);
    if (column == "State" || column == "MotorState")
    {
      // one-hot, as FMS does: a series per state, 1 while in it
      QSet<QByteArray> states;
      for (const Row& row : rows)
      {
        states.insert(row.columns[i]);
      }
      for (const QByteArray& state : states)
      {
        Samples& samples = series[column + "_" + QString::fromLatin1(state)];
        for (const Row& row : rows)
        {
          samples.emplace_back(row.t, row.columns[i] == state ? 1.0 : 0.0);
        }
      }
    }
    else if (i == vendor)
    {
      for (const Row& row : rows)
      {
        double values[2] = { -1, -1 };  // mode, fault
        auto match = vendor_field.globalMatch(QString::fromLatin1(row.columns.mid(i).join(',')));
        while (match.hasNext())
        {
          const auto m = match.next();
          values[m.captured(1) == "mode" ? 0 : 1] = m.captured(2).toDouble();
        }
        series["Vendor_Mode"].emplace_back(row.t, values[0]);
        series["Vendor_Fault"].emplace_back(row.t, values[1]);
      }
    }
    else
    {
      Samples& samples = series[column];
      for (const Row& row : rows)
      {
        const double value = number(row.columns[i]);
        if (std::isfinite(value))
        {
          samples.emplace_back(row.t, value);
        }
      }
    }
  }
  return series;
}
}  // namespace fms
