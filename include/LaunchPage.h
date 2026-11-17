#pragma once
#include <QHash>
#include <QTimer>
#include <QWidget>
#include "LaunchConfig.h"

struct ServerConfig;

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QNetworkAccessManager;
class QNetworkReply;
class QPlainTextEdit;
class QPushButton;
class QRadioButton;
class QVBoxLayout;
class QWidget;
class EffortScale;

// First screen: model, MCP tools (and which location of each to use), and how
// to prompt. Polls the model server and, for each tool, the health URL of the
// location currently selected in its dropdown.
class LaunchPage : public QWidget {
    Q_OBJECT
public:
    explicit LaunchPage(QWidget *parent = nullptr);

    LaunchConfig config() const;

    // Put a saved session's model and MCP server choices into the controls.
    void setConfig(const LaunchConfig &config);
    // The chat that is currently open (empty title = none). While there is one,
    // a second button offers to continue it with the settings shown here.
    void setOpenConversation(bool open, const QString &title = QString());
    // The app's settings were edited: show the new model list and server address.
    void settingsChanged();
    // The local models on offer: the ones in the settings, what the running
    // server reports, and whatever was typed.
    QStringList localModels() const;

signals:
    void startRequested(const LaunchConfig &config);
    // Continue the open chat with these settings (model / MCP servers may have changed).
    void continueRequested(const LaunchConfig &config);
    // `alreadyUp` is what the last probe saw, so the caller doesn't launch a
    // second copy of something started from a terminal.
    void serverPageRequested(const LaunchConfig &config, bool alreadyUp);
    void toolPageRequested(const QString &toolName, const QString &location, bool alreadyUp);
    // The server list was edited (added / changed / removed).
    void toolsChanged();
    // The link in the setup note was clicked.
    void settingsRequested();

protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    struct ToolRow {
        QCheckBox   *enabled  = nullptr;
        QComboBox   *location = nullptr;   // itemData: ToolLocation::id
        QLabel      *status   = nullptr;
        QPushButton *button   = nullptr;
        bool         up       = false;
    };

    void rebuildToolRows();
    void editServer(const QString &name);   // empty name = add a new one
    void importServers();
    void applyServers(const QList<ServerConfig> &servers);
    void probeAll();
    void setServerStatus(bool up, const QStringList &models);
    void setToolStatus(const QString &name, bool up);
    void probeTool(const QString &name);
    void toolLocationChanged(const QString &name);
    QString toolLocationId(const QString &name) const;
    void updateScriptWidgets();
    void updateBackendWidgets();
    void checkSetup();        // look for what would stop a session from working
    void updateSetupNote();
    bool validate(const LaunchConfig &c, QString *why) const;
    bool confirmStart(const LaunchConfig &c);   // validate, warn about services that are down, remember the choices

    // who answers: the local model server or Claude Code
    QRadioButton *m_localBackend  = nullptr;
    QRadioButton *m_claudeBackend = nullptr;
    QComboBox    *m_claudeModel   = nullptr;   // itemData: the model id; other text = typed as is
    QLabel       *m_claudeStatus  = nullptr;
    EffortScale  *m_effort        = nullptr;   // how hard the model thinks (levels follow the backend)
    QString claudeModelId() const;
    void    showClaudeModel(const QString &id);

    // model + server
    QComboBox   *m_model        = nullptr;
    QLabel      *m_serverStatus = nullptr;
    QPushButton *m_serverButton = nullptr;
    bool         m_serverUp     = false;

    // tools: rows are rebuilt from mcp_servers.json whenever it is edited
    QHash<QString, ToolRow> m_tools;
    QVBoxLayout            *m_toolBoxLayout = nullptr;
    QWidget                *m_toolRows      = nullptr;

    // prompt mode
    QRadioButton   *m_chatMode     = nullptr;
    QRadioButton   *m_scriptMode   = nullptr;
    QWidget        *m_scriptBox    = nullptr;
    QRadioButton   *m_scriptFile   = nullptr;
    QRadioButton   *m_scriptGen    = nullptr;
    QLineEdit      *m_scriptPath   = nullptr;
    QPushButton    *m_browse       = nullptr;
    QPlainTextEdit *m_scriptPrompt = nullptr;

    // what is missing on this machine, if anything (hidden when nothing is)
    QLabel  *m_setupNote = nullptr;
    QString  m_pythonProblem;

    QPushButton *m_start    = nullptr;
    QPushButton *m_continue = nullptr;   // shown while a chat is open

    // probing
    QNetworkAccessManager            *m_net = nullptr;
    QHash<QString, QNetworkReply *>   m_probes;   // key: "server" or "<tool>/<location id>"
    QTimer                            m_probeTimer;
};
