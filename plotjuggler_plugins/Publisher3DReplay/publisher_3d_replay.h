#ifndef PUBLISHER_3D_REPLAY_H
#define PUBLISHER_3D_REPLAY_H

#include <QObject>
#include <QtPlugin>
#include <QAction>
#include <QTcpServer>
#include <QTimer>
#include <QWebSocket>
#include <QWebSocketServer>
#include "PlotJuggler/statepublisher_base.h"

/// Replays the vehicle pose in 3D (CesiumJS) in the system web browser.
/// A localhost server serves the page and pushes the trajectory to it once,
/// then sends every move of the PlotJuggler time tracker over a websocket.
class Publisher3DReplay : public PJ::StatePublisher
{
  Q_OBJECT
  Q_PLUGIN_METADATA(IID "facontidavide.PlotJuggler3.StatePublisher")
  Q_INTERFACES(PJ::StatePublisher)

public:
  Publisher3DReplay();

  ~Publisher3DReplay() override;

  const char* name() const override
  {
    return "3D Replay";
  }

  bool enabled() const override
  {
    return _enabled;
  }

  void updateState(double current_time) override;

  void play(double current_time) override;

  const std::vector<QAction*>& availableActions() override
  {
    return _actions;
  }

  /// Port the server listens on, 0 when not enabled.
  quint16 port() const
  {
    return _http_server.serverPort();
  }

public slots:
  void setEnabled(bool enabled) override;

private slots:
  void onNewHttpConnection();

  void onNewWebSocket();

private:
  bool _enabled = false;
  double _current_time = std::numeric_limits<double>::quiet_NaN();

  QTcpServer _http_server;
  QWebSocketServer _ws_server;
  std::vector<QWebSocket*> _clients;

  // the series the trajectory is built from; a change in the data map (new
  // file, series downloaded on demand, streaming) re-sends the trajectory
  std::vector<std::string> _watched_keys;
  QByteArray _data_signature;
  QTimer _poll_timer;

  std::vector<QAction*> _actions;

  QUrl pageUrl() const;

  void handleHttpRequest(QTcpSocket* socket);

  /// The trajectory message sent to a page (JSON), with a "missing" message
  /// instead of the samples when the data map lacks position or attitude.
  QByteArray trajectoryMessage();

  QByteArray dataSignature() const;

  /// Find the series the trajectory needs and ask for the empty ones.
  void watchSeries();

  void pollData();

  void sendTime();

  void sendTrajectoryToAll();

  void openBrowser();

  void editIonToken();

  /// The .glb the page shows: the one chosen with "Set 3D model..." or the built-in one.
  QString modelFile() const;

  void chooseModelFile();
};

#endif  // PUBLISHER_3D_REPLAY_H
