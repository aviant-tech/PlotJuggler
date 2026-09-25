#pragma once

#include "PlotJuggler/dataloader_base.h"

// Opens the binary .log written by the DRS flight recorder. Every message type
// becomes a topic (named like the CSVs of the vendor's log parser) and the
// text messages are also shown in a "Logged messages" window.
class DataLoadBinLog : public PJ::DataLoader
{
  Q_OBJECT
  Q_PLUGIN_METADATA(IID "facontidavide.PlotJuggler3.DataLoader")
  Q_INTERFACES(PJ::DataLoader)

public:
  const std::vector<const char*>& compatibleFileExtensions() const override;

  bool readDataFromFile(PJ::FileLoadInfo* fileload_info, PJ::PlotDataMapRef& destination) override;

  const char* name() const override
  {
    return "DataLoad Binary Log";
  }
};
