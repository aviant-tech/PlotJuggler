#include "dataload_bin_log.h"

#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QVBoxLayout>

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string_view>

using namespace PJ;

static const char* PARSER_SETTING = "DataLoadBinLog/parser";

const std::vector<const char*>& DataLoadBinLog::compatibleFileExtensions() const
{
  static std::vector<const char*> extensions = { "log" };
  return extensions;
}

bool DataLoadBinLog::canReadFile(const QString& filename) const
{
  QFile file(filename);
  if (!file.open(QIODevice::ReadOnly))
  {
    return true;  // let the application report the unreadable file
  }
  // The first line is the firmware id: "<product> v<major>.<minor>.<patch>".
  static const QRegularExpression firmware_id("^[A-Za-z0-9_]+ v\\d+\\.\\d+\\.\\d+\\n$");
  return firmware_id.match(QString::fromLatin1(file.readLine(128))).hasMatch();
}

namespace
{
// Splits one CSV line on commas; quoted cells may hold commas and "" for a quote.
std::vector<std::string> splitCsvLine(const std::string& line)
{
  std::vector<std::string> cells(1);
  bool quoted = false;
  for (size_t i = 0; i < line.size(); i++)
  {
    const char c = line[i];
    if (c == '"' && quoted && i + 1 < line.size() && line[i + 1] == '"')
    {
      cells.back() += '"';
      i++;
    }
    else if (c == '"')
    {
      quoted = !quoted;
    }
    else if (c == ',' && !quoted)
    {
      cells.emplace_back();
    }
    else if (c != '\r')
    {
      cells.back() += c;
    }
  }
  return cells;
}

// Numbers may carry a label, e.g. "1 (power on)"; hex columns say so in their name.
std::optional<double> parseNumber(const std::string& cell, bool hex)
{
  char* end = nullptr;
  const double value =
      hex ? double(std::strtoull(cell.c_str(), &end, 16)) : std::strtod(cell.c_str(), &end);
  if (cell.empty() || end == cell.c_str() || (*end != '\0' && *end != ' '))
  {
    return std::nullopt;
  }
  return value;
}

// One parser CSV: the first column is the timestamp in seconds, every other column a
// series "<topic>/<column>". A column is numeric unless its first non-empty cell is not.
void loadCsvTopic(const QString& csv_path, const std::string& topic, PlotDataMapRef& plot_data)
{
  std::ifstream file(csv_path.toStdString());
  std::string line;
  if (!std::getline(file, line))
  {
    return;
  }
  const std::vector<std::string> columns = splitCsvLine(line);
  auto group = plot_data.getOrCreateGroup(topic);
  std::vector<std::string> names(columns.size());
  std::vector<bool> hex(columns.size(), false);
  for (size_t i = 1; i < columns.size(); i++)
  {
    // The parser prefixes the state column with the product name ("<product>_state").
    const bool state = topic == "state" && columns[i].size() > 6 &&
                       columns[i].compare(columns[i].size() - 6, 6, "_state") == 0;
    names[i] = topic + "/" + (state ? "state" : columns[i]);
    hex[i] = columns[i].find("(hex)") != std::string::npos;
  }
  std::vector<PlotData*> numeric(columns.size(), nullptr);
  std::vector<StringSeries*> strings(columns.size(), nullptr);

  while (std::getline(file, line))
  {
    const std::vector<std::string> cells = splitCsvLine(line);
    const auto stamp = parseNumber(cells[0], false);
    if (!stamp || cells.size() != columns.size())
    {
      continue;
    }
    for (size_t i = 1; i < columns.size(); i++)
    {
      if (cells[i].empty())
      {
        continue;
      }
      const auto value = parseNumber(cells[i], hex[i]);
      if (!numeric[i] && !strings[i])
      {
        if (value)
        {
          numeric[i] = &plot_data.getOrCreateNumeric(names[i], group);
        }
        else
        {
          strings[i] = &plot_data.getOrCreateStringSeries(names[i], group);
        }
      }
      if (strings[i])
      {
        strings[i]->pushBack({ *stamp, cells[i] });
      }
      else if (value)
      {
        numeric[i]->pushBack({ *stamp, *value });
      }
    }
  }
}

// String values cannot be plotted, so a string series also gets a numeric
// "<series>/bitmap" twin whose value is 1 << i for the i-th distinct value (in
// sorted order). The bit assignment is in the series' tooltip in the curve list.
// Columns with more than 32 distinct values (the mavlink "info" text) stay strings only.
void addBitmapSeries(const std::string& name, const StringSeries& strings,
                     PlotDataMapRef& plot_data)
{
  std::set<std::string> values;
  for (size_t i = 0; i < strings.size(); i++)
  {
    values.emplace(strings.getString(strings.at(i).y));
  }
  if (values.size() > 32)
  {
    return;
  }
  std::map<std::string_view, double> bit_of;
  QString legend;
  for (const std::string& value : values)
  {
    const double bit = std::ldexp(1.0, int(bit_of.size()));
    bit_of.emplace(value, bit);
    legend += QString("%1 = %2\n").arg(bit, 0, 'f', 0).arg(QString::fromStdString(value));
  }
  PlotData& bitmap = plot_data.getOrCreateNumeric(name + "/bitmap", strings.group());
  bitmap.setAttribute(PJ::TOOL_TIP, legend.trimmed());
  for (size_t i = 0; i < strings.size(); i++)
  {
    const auto& point = strings.at(i);
    bitmap.pushBack({ point.x, bit_of.at(strings.getString(point.y)) });
  }
}

// Same shape as the "Message Logs" tab of the ULog loader.
void showLoggedMessages(const StringSeries& messages, const QString& log_name)
{
  auto* table = new QTableWidget(int(messages.size()), 2);
  table->setHorizontalHeaderLabels({ "Timestamp", "Message" });
  table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table->verticalHeader()->hide();
  for (size_t row = 0; row < messages.size(); row++)
  {
    const auto& point = messages.at(row);
    const std::string_view text = messages.getString(point.y);
    table->setItem(int(row), 0, new QTableWidgetItem(QString::number(point.x, 'f', 5)));
    table->setItem(int(row), 1,
                   new QTableWidgetItem(QString::fromUtf8(text.data(), int(text.size()))));
  }
  table->resizeColumnToContents(0);
  table->horizontalHeader()->setStretchLastSection(true);

  QWidget* main_window = nullptr;
  for (QWidget* widget : qApp->topLevelWidgets())
  {
    if (widget->inherits("QMainWindow"))
    {
      main_window = widget;
    }
  }
  auto* dialog = new QDialog(main_window);
  dialog->setWindowTitle(QString("Logged messages - %1").arg(log_name));
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->resize(700, 500);
  auto* layout = new QVBoxLayout(dialog);
  layout->addWidget(table);
  dialog->show();
}
}  // namespace

bool DataLoadBinLog::readDataFromFile(FileLoadInfo* info, PlotDataMapRef& plot_data)
{
  // The parser executable is remembered in QSettings once it has run successfully.
  QSettings settings;
  QString parser = settings.value(PARSER_SETTING).toString();
  if (parser.isEmpty() || !QFileInfo(parser).isExecutable())
  {
    parser = QFileDialog::getOpenFileName(nullptr, tr("Locate the vendor log parser "
                                                      "(SW_040200_log-parser)"));
    if (parser.isEmpty())
    {
      return false;
    }
  }

  // The parser writes its CSVs next to the input, so work on a copy in a scratch directory.
  QTemporaryDir scratch;
  const QFileInfo log_info(info->filename);
  const QString log_copy = scratch.filePath(log_info.fileName());
  if (!scratch.isValid() || !QFile::copy(info->filename, log_copy))
  {
    throw std::runtime_error("Binary log: cannot copy the file to a temporary directory");
  }

  QProcess process;
  process.setWorkingDirectory(scratch.path());
  process.setProcessChannelMode(QProcess::MergedChannels);
  process.start(parser, { log_copy });
  if (!process.waitForFinished(-1) || process.exitStatus() != QProcess::NormalExit ||
      process.exitCode() != 0)
  {
    const std::string output = process.readAll().toStdString();
    throw std::runtime_error("Binary log: the log parser failed:\n" + output + "\n" +
                             process.errorString().toStdString());
  }
  settings.setValue(PARSER_SETTING, parser);

  const QString base = log_info.completeBaseName() + "_";
  for (const QString& csv : QDir(scratch.path()).entryList({ base + "*.csv" }))
  {
    const QString topic = QFileInfo(csv).completeBaseName().mid(base.size());
    loadCsvTopic(scratch.filePath(csv), topic.toStdString(), plot_data);
  }

  // Free-form log messages are shown in their own window; every other string
  // series (parameter names, mavlink event names, ...) gets a plottable twin.
  for (const auto& [name, strings] : plot_data.strings)
  {
    if (name == "text/text")
    {
      if (strings.size() > 0)
      {
        showLoggedMessages(strings, log_info.fileName());
      }
    }
    else
    {
      addBitmapSeries(name, strings, plot_data);
    }
  }
  return true;
}
