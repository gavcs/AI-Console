#pragma once
#include <QProcess>
#include <QProcessEnvironment>
#include <QWidget>
#include "Commands.h"

class QLabel;
class QPlainTextEdit;
class QPushButton;

// A console that runs one command and streams its stdout/stderr live.
// Used for the model server log, tool services (ssh tunnels etc.), the
// script generator and script runs.
class ProcessPage : public QWidget {
    Q_OBJECT
public:
    explicit ProcessPage(QWidget *parent = nullptr);
    ~ProcessPage() override;

    // If true, leaving the page (Back) kills the process. False for things
    // that should keep running while you do something else (server, tunnel).
    void setStopOnBack(bool stop) { m_stopOnBack = stop; }

    // Optional extra button shown once the process has exited successfully
    // (e.g. "Run script" after the generator finished). Empty = no button.
    void setNextAction(const QString &label);

    // Reset the view and launch `cmd` with `env` (cwd from cmd.cwd).
    void begin(const QString &header, const Command &cmd, const QProcessEnvironment &env);

    // Like begin(), but don't launch: show `note` instead and leave the command
    // armed for the "Run again" button. For something that is already up but
    // was not started from this page, so there is no output to show.
    void showIdle(const QString &header, const Command &cmd, const QProcessEnvironment &env, const QString &note);

    bool isRunning() const { return m_process.state() != QProcess::NotRunning; }
    // Everything shown on the page so far.
    QString output() const;
    // Stop the process (if running) and everything it started, and wait for
    // it; finished() is emitted before this returns.
    void stop();
    // For a page that runs python/service_watch.sh, when the app is closing:
    // tell the watcher to stop the service it started and end. Returns at
    // once; follow it with waitForFinished() and then stop().
    void requestServiceShutdown();
    bool waitForFinished(int ms) { return !isRunning() || m_process.waitForFinished(ms); }
    // True once begin()/showIdle() has put something on this page.
    bool hasContent() const { return m_cmd.isValid(); }

signals:
    void backRequested();
    void nextActionRequested();
    void started();                                // the command was launched (begin() or "Run again")
    void finished(int exitCode, bool normalExit);

private:
    void run();
    void appendOutput(const QString &text);
    void setRunning(bool running);

    Command             m_cmd;
    QProcessEnvironment m_env;
    bool                m_stopOnBack = true;
    int                 m_lastExit   = -1;

    QProcess        m_process;
    QLabel         *m_header     = nullptr;
    QPlainTextEdit *m_output     = nullptr;
    QPushButton    *m_runButton  = nullptr;
    QPushButton    *m_stopButton = nullptr;
    QPushButton    *m_nextButton = nullptr;
};
