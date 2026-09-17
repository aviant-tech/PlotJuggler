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
  connect(_widget, &FmsBrowserWidget::showRequested, this, &ToolboxFMS::showFromMenuAction);
  connect(_widget, &FmsBrowserWidget::groupProgress, this,
          [this](const QString& group, int percent) { emit groupProgress(group.toStdString(), percent); });

  const QString link_flight = qEnvironmentVariable("FMS_FLIGHT_ID");
  if (!link_flight.isEmpty())
  {
    // Launched from an FMS deep link. The link named one flight, so there is
    // nothing to choose in the browser panel: leave the plot view where it is
    // and let the widget download the whole flight in the background. Series
    // appear as batches land. The panel is only raised if the widget hits
    // something the user has to deal with (see showRequested). Queued so the
    // host has finished wiring importData before the first batch arrives.
    QTimer::singleShot(0, _widget, [this, link_flight]() {
      _widget->openFlightFromLink(link_flight);
    });
  }
}

void ToolboxFMS::showFromMenuAction() const
{
  // The toolbox interface has no way for a plugin to ask to be shown, so
  // trigger the menu action the host created for us: it is named after this
  // plugin and is wired to both onShowWidget() and the widget stack. If the
  // host ever stops working this way nothing happens, and the panel can still
  // be opened by hand from the Tools menu.
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

void ToolboxFMS::onSeriesRequested(const std::string& series_name)
{
  _widget->fetchSeries(QString::fromStdString(series_name));
}
