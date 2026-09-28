#pragma once

#include "PlotJuggler/dataloader_base.h"

// Opens the binary .log written by the flight recorder by running the vendor's
// log parser on it. Every CSV the parser writes becomes a topic; the text CSV
// is also shown in a "Logged messages" window.
class DataLoadBinLog : public PJ::DataLoader
{
  Q_OBJECT
  Q_PLUGIN_METADATA(IID "facontidavide.PlotJuggler3.DataLoader")
  Q_INTERFACES(PJ::DataLoader)

public:
  const std::vector<const char*>& compatibleFileExtensions() const override;

  // Other programs write .log files too; only claim the ones with our header.
  bool canReadFile(const QString& filename) const override;

  bool readDataFromFile(PJ::FileLoadInfo* fileload_info, PJ::PlotDataMapRef& destination) override;

  const char* name() const override
  {
    return "DataLoad Binary Log";
  }
};
