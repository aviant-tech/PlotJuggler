// Runs the plugin's decoder on a .log and writes OUTDIR/<base>_<topic>.csv files in the
// same shape as the vendor parser, so compare_with_vendor.py can check the C++ port.
// Built as bin_log_dump_csv with -DBUILD_TESTING=ON, or without Qt from this directory:
//   g++ -std=c++17 -O2 -I . tests/dump_csv.cpp bin_log_decoder.cpp -o dump_csv
//   ./dump_csv FILE.log OUTDIR
#include "bin_log_decoder.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <vector>

namespace
{
std::string quoted(const std::string& cell)
{
  if (cell.find_first_of(",\"") == std::string::npos)
  {
    return cell;
  }
  std::string out = "\"";
  for (char c : cell)
  {
    out += c == '"' ? "\"\"" : std::string(1, c);
  }
  return out + "\"";
}

// Collects rows per topic; one row per (topic, timestamp) in arrival order.
class CsvSink : public bin_log::Sink
{
public:
  void number(const std::string& topic, const std::string& field, double stamp,
              double value) override
  {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.9g", value);
    cell(topic, field, stamp) = buf;
  }
  void text(const std::string& topic, const std::string& field, double stamp,
            const std::string& value) override
  {
    cell(topic, field, stamp) = value;
  }
  void write(const std::string& base, const std::string& out_dir) const
  {
    for (const auto& [topic, t] : topics_)
    {
      std::ofstream out(out_dir + "/" + base + "_" + topic + ".csv");
      out << "timestamp";
      for (const auto& column : t.columns)
      {
        out << "," << column;
      }
      out << "\n";
      for (const auto& row : t.rows)
      {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.5f", row.stamp);
        out << buf;
        for (const auto& column : t.columns)
        {
          const auto cell = row.cells.find(column);
          out << "," << (cell == row.cells.end() ? "" : quoted(cell->second));
        }
        out << "\n";
      }
    }
  }

private:
  struct Row
  {
    double stamp;
    std::map<std::string, std::string> cells;
  };
  struct Topic
  {
    std::vector<std::string> columns;
    std::vector<Row> rows;
  };
  std::string& cell(const std::string& topic, const std::string& field, double stamp)
  {
    Topic& t = topics_[topic];
    if (std::find(t.columns.begin(), t.columns.end(), field) == t.columns.end())
    {
      t.columns.push_back(field);
    }
    // A value joins the current row unless the stamp changed or the field is
    // already set (e.g. two PWM1 records with the same stamp).
    const bool same_row = !t.rows.empty() && t.rows.back().stamp == stamp &&
                          !t.rows.back().cells.count(field);
    if (!same_row)
    {
      t.rows.push_back({ stamp, {} });
    }
    return t.rows.back().cells[field];
  }
  std::map<std::string, Topic> topics_;
};
}  // namespace

int main(int argc, char** argv)
{
  if (argc != 3)
  {
    std::fprintf(stderr, "usage: %s FILE.log OUTDIR\n", argv[0]);
    return 2;
  }
  std::ifstream in(argv[1], std::ios::binary);
  const std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
  std::string base = argv[1];
  base = base.substr(base.find_last_of('/') + 1);
  base = base.substr(0, base.find_last_of('.'));
  CsvSink sink;
  bin_log::decode(data.data(), data.size(), sink);
  sink.write(base, argv[2]);
  return 0;
}
