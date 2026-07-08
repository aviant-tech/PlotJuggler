#include "toolbox_fms.h"

#include "fms_browser_widget.h"

ToolboxFMS::ToolboxFMS()
{
  _widget = new FmsBrowserWidget();

  connect(_widget, &FmsBrowserWidget::importData, this, &ToolboxFMS::importData);
  connect(_widget, &FmsBrowserWidget::closed, this, &ToolboxFMS::closed);
}

void ToolboxFMS::init(PJ::PlotDataMapRef&, PJ::TransformsMap&)
{
}

std::pair<QWidget*, PJ::ToolboxPlugin::WidgetType> ToolboxFMS::providedWidget() const
{
  return { _widget, PJ::ToolboxPlugin::FIXED };
}

bool ToolboxFMS::onShowWidget()
{
  _widget->onShow();
  return true;
}
