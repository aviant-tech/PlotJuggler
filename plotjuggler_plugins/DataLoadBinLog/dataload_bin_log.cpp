#include "dataload_bin_log.h"
#include "bin_log_decoder.h"

#include <QApplication>
#include <QDialog>
#include <QFile>
#include <QFileInfo>
#include <QHeaderView>
#include <QTableWidget>
#include <QVBoxLayout>

#include <cmath>
#include <map>
#include <set>
#include <stdexcept>
#include <string_view>

using namespace PJ;

const std::vector<const char*>& DataLoadBinLog::compatibleFileExtensions() const
{
  static std::vector<const char*> extensions = { "log" };
  return extensions;
}

namespace
{
// Every topic becomes a group and every field a series named "<topic>/<field>".
class PlotDataSink : public bin_log::Sink
{
public:
  explicit PlotDataSink(PlotDataMapRef& plot_data) : plot_data_(plot_data)
  {
  }

  void number(const std::string& topic, const std::string& field, double stamp,
              double value) override
  {
    plot_data_.getOrCreateNumeric(topic + "/" + field, plot_data_.getOrCreateGroup(topic))
        .pushBack({ stamp, value });
  }

  void text(const std::string& topic, const std::string& field, double stamp,
            const std::string& value) override
  {
    plot_data_.getOrCreateStringSeries(topic + "/" + field, plot_data_.getOrCreateGroup(topic))
        .pushBack({ stamp, value });
  }

private:
  PlotDataMapRef& plot_data_;
};

// String values cannot be plotted, so a string series also gets a numeric
// "<series>/bitmap" twin whose value is 1 << i for the i-th distinct value (in
// sorted order). The bit assignment is in the series' tooltip in the curve list.
void addBitmapSeries(const std::string& name, const StringSeries& strings,
                     PlotDataMapRef& plot_data)
{
  std::set<std::string> values;
  for (size_t i = 0; i < strings.size(); i++)
  {
    values.emplace(strings.getString(strings.at(i).y));
  }
  std::map<std::string_view, double> bit_of;
  QString legend;
  for (const std::string& value : values)
  {
    const double bit = std::ldexp(1.0, int(bit_of.size()));
    bit_of.emplace(value, bit);
    legend += QString("%1 = %2\n").arg(bit, 0, 'f', 0).arg(QString::fromStdString(value));
  }
  auto group = plot_data.getOrCreateGroup(name.substr(0, name.find('/')));
  PlotData& bitmap = plot_data.getOrCreateNumeric(name + "/bitmap", group);
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
  QFile file(info->filename);
  if (!file.open(QIODevice::ReadOnly))
  {
    throw std::runtime_error("Binary log: cannot open " + info->filename.toStdString());
  }
  const QByteArray data = file.readAll();

  PlotDataSink sink(plot_data);
  bin_log::decode(reinterpret_cast<const uint8_t*>(data.constData()), size_t(data.size()),
                        sink);

  // Free-form log messages are shown in their own window; every other string
  // series (parameter names, mavlink event names, ...) gets a plottable twin.
  for (const auto& [name, strings] : plot_data.strings)
  {
    if (name == "text/text")
    {
      if (strings.size() > 0)
      {
        showLoggedMessages(strings, QFileInfo(info->filename).fileName());
      }
    }
    else
    {
      addBitmapSeries(name, strings, plot_data);
    }
  }
  return true;
}
