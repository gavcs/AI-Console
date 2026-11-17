#pragma once
#include <QHash>
#include <QMainWindow>
#include "History.h"
#include "LaunchConfig.h"

class QLineEdit;
class QListWidget;
class QStackedWidget;
class LaunchPage;
class ChatPage;
class ProcessPage;

// Top-level window: the list of saved sessions on the left, and the pages it
// switches between on the right.
//
//   LaunchPage --Start server--> ProcessPage (server log; keeps running on Back)
//   LaunchPage --tool Start----> ProcessPage per tool location (service watcher / ssh tunnel; keeps running)
//   LaunchPage --Start---------> ChatPage                      (a new chat)
//                             -> ProcessPage (script run)      (script, existing file)
//                             -> ProcessPage (generator) --Run script--> ProcessPage (script run)
//   LaunchPage --Continue------> ChatPage                      (the open chat, with the settings now selected)
//   ChatPage   --model/effort--> ChatPage                      (the open chat, with another model or effort level)
//   session list --------------> ChatPage / ProcessPage        (a saved chat / a saved script run)
//   any page   --Back----------> LaunchPage
//
// Every chat and script run is saved (History.h). One chat is "open" at a
// time: its agent keeps running while other pages are shown, so you can leave
// it, look at a log or start a service, and come back.
//
// Closing the window (or quitting) stops everything the app started: the chat
// agent and its MCP servers, scripts, the services behind MCP servers and
// their tunnels, and the model server. See shutdown().
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    // Open a saved session as if it had been clicked in the list.
    void openSession(const QString &id);

    // Stop everything this app started. Called when the window closes and when
    // the app quits; safe to call more than once.
    void shutdown();

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    void buildMenus();
    void openSettings();
    void showAbout();
    void showServerPage(const LaunchConfig &config, bool alreadyUp);
    void showToolPage(const QString &toolName, const QString &location, bool alreadyUp);
    void startSession(const LaunchConfig &config);
    void continueChat(const LaunchConfig &config);
    void changeChatSettings(const LaunchConfig &config);   // model / effort picked on the chat page
    bool restartServerFor(const LaunchConfig &config);     // true = the model server is being restarted
    void runScript(const LaunchConfig &config);
    void showLaunchPage();

    // (Re)start the agent for m_chatRecord and show it. `waitForServer`: the
    // model server was just restarted, so the first message waits for it.
    void startChat(bool waitForServer = false);
    void showChat();
    void showScriptRun();
    bool confirmLeaveChat();                  // false = a reply is in progress and the user wants to keep it
    void saveScriptRun(bool finished, int exitCode);
    void refreshSessions();                   // redraw the list from disk
    void sessionMenu(const QPoint &pos);
    void deleteSession(const SessionRecord &record);

    QLineEdit      *m_filter    = nullptr;    // narrows the session list
    QListWidget    *m_sessions  = nullptr;
    QStackedWidget *m_pages     = nullptr;
    LaunchPage     *m_launch    = nullptr;
    ProcessPage    *m_server    = nullptr;
    ChatPage       *m_chat      = nullptr;
    ProcessPage    *m_scriptGen = nullptr;
    ProcessPage    *m_scriptRun = nullptr;
    QHash<QString, ProcessPage *> m_toolPages;   // key: "<tool>/<location id>", one log per location

    LaunchConfig  m_pending;        // config of the script being generated
    SessionRecord m_chatRecord;     // the open chat (invalid = none)
    SessionRecord m_scriptRecord;   // the script run shown on m_scriptRun (invalid = none)

    QString m_serverModel;            // the model the server page's command was last started with
    bool    m_serverStarted = false;  // "Start server" ran here at least once (so the stop command applies)
    bool    m_shutDown      = false;
};
