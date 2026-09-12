#pragma once

#include "PlotJuggler/toolbox_base.h"

class FmsBrowserWidget;

class ToolboxFMS : public PJ::ToolboxPlugin
{
  Q_OBJECT
  Q_PLUGIN_METADATA(IID "facontidavide.PlotJuggler3.Toolbox")
  Q_INTERFACES(PJ::ToolboxPlugin)

public:
  ToolboxFMS();

  const char* name() const override
  {
    return "FMS Flight Browser";
  }

  void init(PJ::PlotDataMapRef& src_data, PJ::TransformsMap& transform_map) override;

  std::pair<QWidget*, WidgetType> providedWidget() const override;

public slots:
  bool onShowWidget() override;

private:
  /// Opens the toolbox by triggering the menu action the host registered for it.
  void showFromMenuAction() const;

  FmsBrowserWidget* _widget;
};
