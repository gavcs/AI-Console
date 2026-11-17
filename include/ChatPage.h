#pragma once
#include <QByteArray>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStringList>
#include <QTimer>
#include <QWidget>
#include "Commands.h"
#include "LaunchConfig.h"

class QHBoxLayout;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QScrollArea;
class QVBoxLayout;
class ChatInput;
class ChatMessageView;
class ChatThinkingBlock;

// Session screen for PromptMode::Chat. Drives `python/agent.py chat`, which
// owns the model <-> MCP tool loop, over a JSON-lines protocol on stdin/stdout
// (see the docstring at the top of agent.py). stderr goes to the log pane.
//
// The transcript is a column of blocks: who is speaking, a message (the
// model's answers are rendered as Markdown), a collapsible "Thinking" block
// for the model's reasoning, and one line per tool call / result.
//
// The input takes several lines (Shift+Enter for a new line, Enter sends).
// While a reply is being written the Send button is a Stop button; Esc does
// the same.
class ChatPage : public QWidget {
    Q_OBJECT
public:
    explicit ChatPage(QWidget *parent = nullptr);
    ~ChatPage() override;

    // Reset the view and (re)start the agent process for this session.
    // `messagesPath` is the conversation's saved messages (History.h); what is
    // already in it is shown again, and the agent continues from it.
    void begin(const LaunchConfig &config, const Command &agentCommand, const QProcessEnvironment &env,
               const QString &messagesPath = QString());

    // Stop the agent process (the transcript stays on screen).
    void stopAgent();
    bool isAgentRunning() const { return m_agent.state() != QProcess::NotRunning; }
    // A reply is being written right now.
    bool isBusy() const { return m_turnActive && isAgentRunning(); }

    // Queue files to send with the next message (used by attachFiles() after
    // the file dialog; also the headless-testable entry point).
    void addFiles(const QStringList &paths);

signals:
    void backRequested();
    void userMessageSent(const QString &text);   // the user sent a message (text may be empty: attachments only)
    void turnFinished();                         // the reply to it is complete

private:
    void submit();
    void stopTurn();                          // ask the agent to stop the reply being written
    void updateSendButton();
    void attachFiles();                       // open the file dialog, then addFiles()
    void refreshAttachStrip();                // redraw the pending-attachment chips
    void addUserAttachments(const QStringList &paths);  // show attachments in the transcript
    void onStdout();
    void onStderr();
    void handleEvent(const QJsonObject &ev);
    void setBusy(bool busy);
    void replay(const QString &messagesPath);          // show a saved conversation
    void addUserText(const QString &text);
    void log(const QString &text);            // log pane

    // transcript
    void clearTranscript();
    void addBlock(QWidget *block, int index = -1);
    void addSpeaker(const QString &who);
    void addNote(const QString &text);        // plain, dimmed line (tool calls, errors, status)
    void appendAnswer(const QString &text);
    void appendThinking(const QString &text);
    void closeAnswer();
    void closeThinking();
    void render();                            // push buffered text into the open blocks

    LaunchConfig m_config;
    QProcess     m_agent;
    QByteArray   m_stdoutBuf;
    bool         m_ready = false;
    QString      m_readyStatus;               // "ready · N tools", shown again after each turn
    bool         m_turnActive = false;        // between sending a message and its "done"
    bool         m_replayed   = false;        // the transcript started from a saved conversation

    QLabel         *m_header = nullptr;
    QLabel         *m_status = nullptr;
    QScrollArea    *m_scroll = nullptr;
    QVBoxLayout    *m_blocks = nullptr;
    QPlainTextEdit *m_log    = nullptr;
    ChatInput      *m_input  = nullptr;
    QPushButton    *m_send   = nullptr;       // "Send", or "Stop" while a reply is being written
    QPushButton    *m_attach = nullptr;       // "Attach…" button
    QWidget        *m_attachStrip  = nullptr; // row of chips for files queued for the next message
    QHBoxLayout    *m_attachLayout = nullptr;
    QStringList     m_pendingPaths;           // files queued for the next message (for display)
    QJsonArray      m_pendingAttachments;     // ...as protocol entries (kept in step with m_pendingPaths)
    bool            m_stickToBottom = true;   // keep following new text unless the user scrolled up

    // The blocks currently being streamed into (null = none open).
    ChatMessageView   *m_answer   = nullptr;
    QString            m_answerText;
    QElapsedTimer      m_answerClock;
    ChatThinkingBlock *m_thinking = nullptr;
    QTimer             m_renderTimer;         // batches re-rendering while text streams in
};
