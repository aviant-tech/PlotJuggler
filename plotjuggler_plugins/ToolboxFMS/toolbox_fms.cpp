#include "toolbox_fms.h"

#include <QAction>
#include <QApplication>
#include <QTimer>
#include <QWidget>

#include "fms_browser_widget.h"

ToolboxFMS::ToolboxFMS()
{
  _widget = new FmsBrowserWidget();

  connect(_widget, &FmsBrowserWidget::importData, this, &ToolboxFMS::importData);
  connect(_widget, &FmsBrowserWidget::closed, this, &ToolboxFMS::closed);

  if (qEnvironmentVariableIsSet("FMS_FLIGHT_ID"))
  {
    // Launched from an FMS deep link, so open the browser instead of leaving it
    // in the Tools menu. The toolbox interface has no way for a plugin to ask to
    // be shown, so trigger the menu action the host created for us: it is named
    // after this plugin and is wired to both onShowWidget() and the widget
    // stack. Queued because the action does not exist yet while we are being
    // constructed. If the host ever stops working this way nothing happens, and
    // opening the browser by hand still lands on the right flight.
    QTimer::singleShot(0, this, [this]() { showFromMenuAction(); });
  }
}

void ToolboxFMS::showFromMenuAction() const
{
  for (QWidget* top_level : QApplication::topLevelWidgets())
  {
    for (QAction* action : top_level->findChildren<QAction*>())
    {
      if (action->text() == name())
      {
        action->trigger();
        return;
      }
    }
  }
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
