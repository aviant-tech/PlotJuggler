#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// Decoder for the binary .log written by DRS firmware v2.9 and later.
// The format was reverse engineered from the vendor's log parser; the layout is
// described in docs/bin_log/FORMAT.md. This file has no Qt dependency so
// it can be tested alone (see tests/dump_csv.cpp).
namespace bin_log
{
// Receives one value per decoded field. `topic` and `field` match the names the
// vendor parser uses for its CSV files and columns.
class Sink
{
public:
  virtual ~Sink() = default;
  virtual void number(const std::string& topic, const std::string& field, double stamp,
                      double value) = 0;
  virtual void text(const std::string& topic, const std::string& field, double stamp,
                    const std::string& value) = 0;
};

// Decodes a whole file held in memory. Throws std::runtime_error when the file
// does not start with a "DRS_Parachute v2.x" header this decoder understands.
void decode(const uint8_t* data, size_t size, Sink& sink);
}  // namespace bin_log
