#include "MainWindow.h"

#include <QCloseEvent>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QStackedWidget>
#include <QStatusBar>
#include <QVBoxLayout>

#include "ChatPage.h"
#include "Commands.h"
#include "LaunchPage.h"
#include "McpServers.h"
#include "ProcessPage.h"
#include "SettingsDialog.h"

namespace {
// "14:05" for today, "Nov 12" for earlier this year, "Nov 12 2025" before that.
QString when(const QDateTime &t) {
    const QDate today = QDate::currentDate();
    if (t.date() == today)
        return t.toString("HH:mm");
    return t.toString(t.date().year() == today.year() ? "MMM d" : "MMM d yyyy");
}
} // namespace

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    setWindowTitle("AI Console");
    resize(1240, 780);
    restoreGeometry(QSettings().value("window/geometry").toByteArray());   // where it was last time

    m_launch    = new LaunchPage;
    m_server    = new ProcessPage;
    m_chat      = new ChatPage;
    m_scriptGen = new ProcessPage;
    m_scriptRun = new ProcessPage;

    m_server->setStopOnBack(false);      // the server outlives the log view
    m_scriptGen->setStopOnBack(true);
    m_scriptGen->setNextAction("Run script");
    m_scriptRun->setStopOnBack(true);

    m_pages = new QStackedWidget;
    for (QWidget *w : std::initializer_list<QWidget *>{m_launch, m_server, m_chat, m_scriptGen, m_scriptRun})
        m_pages->addWidget(w);

    // ---- sidebar: saved sessions ------------------------------------------
    auto *appTitle = new QLabel("AI Console");
    appTitle->setObjectName("appTitle");
    auto *newSession = new QPushButton("＋  New session");
    newSession->setToolTip("Back to the start page, to start a new chat or script run");
    connect(newSession, &QPushButton::clicked, this, &MainWindow::showLaunchPage);
    auto *saved = new QLabel("SAVED SESSIONS");
    saved->setObjectName("speaker");
    saved->setContentsMargins(6, 10, 0, 0);
    m_filter = new QLineEdit;
    m_filter->setObjectName("sessionFilter");
    m_filter->setPlaceholderText("Filter");
    m_filter->setClearButtonEnabled(true);
    m_filter->setToolTip("Show only sessions whose title or model contains this text");
    connect(m_filter, &QLineEdit::textChanged, this, &MainWindow::refreshSessions);
    m_sessions = new QListWidget;
    m_sessions->setObjectName("sessions");
    m_sessions->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_sessions->setTextElideMode(Qt::ElideRight);
    m_sessions->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_sessions, &QListWidget::itemClicked, this,
            [this](QListWidgetItem *item) { openSession(item->data(Qt::UserRole).toString()); });
    connect(m_sessions, &QListWidget::customContextMenuRequested, this, &MainWindow::sessionMenu);

    auto *sidebar = new QWidget;
    sidebar->setObjectName("sidebar");
    sidebar->setAttribute(Qt::WA_StyledBackground);
    sidebar->setFixedWidth(250);
    auto *sideLayout = new QVBoxLayout(sidebar);
    sideLayout->setContentsMargins(10, 12, 10, 8);
    sideLayout->addWidget(appTitle);
    sideLayout->addWidget(newSession);
    sideLayout->addWidget(saved);
    sideLayout->addWidget(m_filter);
    sideLayout->addWidget(m_sessions, 1);
    auto *settingsButton = new QPushButton("Settings…");
    settingsButton->setFlat(true);
    settingsButton->setToolTip("Model server, Python, Claude Code and folders");
    connect(settingsButton, &QPushButton::clicked, this, &MainWindow::openSettings);
    auto *version = new QLabel("v" AI_CONSOLE_VERSION);
    version->setObjectName("subtle");
    auto *sideBottom = new QHBoxLayout;
    sideBottom->addWidget(settingsButton);
    sideBottom->addStretch(1);
    sideBottom->addWidget(version);
    sideLayout->addLayout(sideBottom);

    auto *central = new QWidget;
    auto *row = new QHBoxLayout(central);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);
    row->addWidget(sidebar);
    row->addWidget(m_pages, 1);
    setCentralWidget(central);

    connect(m_launch, &LaunchPage::serverPageRequested, this, &MainWindow::showServerPage);
    connect(m_launch, &LaunchPage::toolPageRequested,   this, &MainWindow::showToolPage);
    connect(m_launch, &LaunchPage::startRequested,      this, &MainWindow::startSession);
    connect(m_launch, &LaunchPage::continueRequested,   this, &MainWindow::continueChat);
    connect(m_launch, &LaunchPage::settingsRequested,   this, &MainWindow::openSettings);
    buildMenus();
    // A server or location that was edited away takes its log page (and the
    // tunnel / watcher running in it) with it.
    connect(m_launch, &LaunchPage::toolsChanged, this, [this] {
        for (auto it = m_toolPages.begin(); it != m_toolPages.end();) {
            const McpTool *tool = commands::findTool(it.key().section('/', 0, 0));
            const QString locId = it.key().section('/', 1);
            bool exists = false;
            if (tool)
                for (const ToolLocation &l : tool->locations)
                    exists = exists || l.id == locId;
            if (exists) {
                ++it;
            } else {
                delete it.value();      // ~ProcessPage stops its process
                it = m_toolPages.erase(it);
            }
        }
    });
    for (ProcessPage *p : {m_server, m_scriptGen, m_scriptRun})
        connect(p, &ProcessPage::backRequested, this, &MainWindow::showLaunchPage);
    connect(m_chat, &ChatPage::backRequested, this, &MainWindow::showLaunchPage);
    connect(m_chat, &ChatPage::settingsChangeRequested, this, &MainWindow::changeChatSettings);
    connect(m_chat, &ChatPage::settingsChanged, this, [this](const LaunchConfig &config) {
        statusBar()->showMessage(config.summary());
        refreshSessions();
    });
    connect(m_server, &ProcessPage::started, this, [this] { m_serverStarted = true; });
    connect(qApp, &QCoreApplication::aboutToQuit, this, &MainWindow::shutdown);
    connect(m_scriptGen, &ProcessPage::nextActionRequested, this, [this] { runScript(m_pending); });

    // ---- keeping the saved sessions up to date ----------------------------
    // The messages themselves are written by the agent; the app keeps the
    // title and the "last used" time.
    connect(m_chat, &ChatPage::userMessageSent, this, [this](const QString &text) {
        if (!m_chatRecord.isValid())
            return;
        if (m_chatRecord.title.isEmpty()) {
            const QString line = text.simplified();
            m_chatRecord.title = line.isEmpty() ? QStringLiteral("Attachments") : line.left(60);
            m_launch->setOpenConversation(true, m_chatRecord.title);
        }
        m_chatRecord.updated = QDateTime::currentDateTime();
        history::save(m_chatRecord);
        refreshSessions();
    });
    connect(m_chat, &ChatPage::turnFinished, this, [this] {
        if (!m_chatRecord.isValid())
            return;
        m_chatRecord.updated = QDateTime::currentDateTime();
        history::save(m_chatRecord);
        refreshSessions();
    });
    connect(m_scriptRun, &ProcessPage::started, this, [this] { saveScriptRun(false, -1); });
    connect(m_scriptRun, &ProcessPage::finished, this,
            [this](int code, bool normal) { saveScriptRun(true, normal ? code : -1); });

    showLaunchPage();
}

// ---- menus, settings, about ---------------------------------------------------

void MainWindow::buildMenus() {
    QMenu *file = menuBar()->addMenu("File");
    QAction *newSession = file->addAction("New Session", this, &MainWindow::showLaunchPage);
    newSession->setShortcut(QKeySequence::New);
    QAction *back = file->addAction("Back to Start Page", this, &MainWindow::showLaunchPage);
    back->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_BracketLeft));
    QAction *find = file->addAction("Filter Sessions", this, [this] {
        m_filter->setFocus();
        m_filter->selectAll();
    });
    find->setShortcut(QKeySequence::Find);
    // On macOS these two move to the application menu, where they are expected.
    QAction *prefs = file->addAction("Settings…", this, &MainWindow::openSettings);
    prefs->setShortcut(QKeySequence::Preferences);
    prefs->setMenuRole(QAction::PreferencesRole);
    QAction *about = file->addAction("About AI Console", this, &MainWindow::showAbout);
    about->setMenuRole(QAction::AboutRole);
}

void MainWindow::openSettings() {
    SettingsDialog dialog(settings::current(), this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    QString error;
    if (!settings::save(dialog.settings(), &error)) {
        QMessageBox::warning(this, "Settings", QStringLiteral("Could not write %1:\n%2").arg(settings::path(), error));
        return;
    }
    m_launch->settingsChanged();
    statusBar()->showMessage("Settings saved — they apply to sessions started from now on", 6000);
}

void MainWindow::showAbout() {
    QMessageBox::about(this, "About AI Console",
                       QStringLiteral("<h3>AI Console %1</h3>"
                                      "<p>A desktop console for running a model with MCP tools: a local model through "
                                      "any OpenAI-compatible server, or Claude Code. Chats and script runs are saved "
                                      "and can be continued with another model or other tools.</p>"
                                      "<p>Settings, MCP servers and saved sessions are kept in<br><code>%2</code></p>"
                                      "<p>MIT licence. Built with Qt %3.</p>")
                           .arg(AI_CONSOLE_VERSION, QFileInfo(settings::path()).absolutePath().toHtmlEscaped(), qVersion()));
}

void MainWindow::closeEvent(QCloseEvent *event) {
    QSettings().setValue("window/geometry", saveGeometry());
    QMainWindow::closeEvent(event);
    if (event->isAccepted()) {
        hide();          // the window goes at once; stopping things may take a few seconds
        shutdown();
    }
}

MainWindow::~MainWindow() {
    shutdown();
}

// ---- closing --------------------------------------------------------------------

// Nothing the app started is left running behind it:
//   - the chat agent, and with it the MCP servers it runs
//   - a script run or a script being written
//   - the services behind MCP servers that were started from here (their
//     watcher is told to stop them; one that was already running is left
//     alone), and the ssh tunnels to them
//   - the model server, if "Start server" started it
// What was started outside the app (a server in a terminal, a service that was
// already up) is not the app's to stop.
void MainWindow::shutdown() {
    if (m_shutDown)
        return;
    m_shutDown = true;

    m_chat->stopAgent();
    m_scriptGen->stop();
    m_scriptRun->stop();

    // Ask every service watcher at once, then give them a common deadline.
    for (ProcessPage *page : std::as_const(m_toolPages))
        page->requestServiceShutdown();
    QElapsedTimer waited;
    waited.start();
    constexpr int kServiceStopMs = 10000;
    for (ProcessPage *page : std::as_const(m_toolPages)) {
        page->waitForFinished(qMax(0, kServiceStopMs - int(waited.elapsed())));
        page->stop();
    }

    m_server->stop();
    // A server that runs outside the command that started it (LM Studio's
    // "lms server start" returns at once) has a command of its own to stop it.
    const QStringList stopCommand = QProcess::splitCommand(settings::current().stopCommand);
    if (m_serverStarted && !stopCommand.isEmpty()) {
        const QProcessEnvironment env = commands::environment();
        QProcess stopper;
        stopper.setProcessEnvironment(env);
        stopper.start(commands::resolveProgram(stopCommand.first(), env), stopCommand.mid(1));
        if (!stopper.waitForFinished(10000))
            stopper.kill();
    }
}

void MainWindow::showServerPage(const LaunchConfig &config, bool alreadyUp) {
    // Only launch if nothing is serving — neither our own process nor one
    // started from a terminal (which we can see but don't own).
    const QString header = QStringLiteral("<b>Server</b> — %1").arg(config.model.section('/', -1));
    if (!m_server->isRunning()) {
        if (!alreadyUp) {
            m_serverModel = config.model;
            m_server->begin(header, commands::serverCommand(config), commands::environment());
        } else if (!m_server->hasContent()) {
            m_server->showIdle(header, commands::serverCommand(config), commands::environment(),
                               QStringLiteral("[A server already answers at %1, but it was not started from this app, "
                                              "so there is no log to show here.]").arg(commands::serverBaseUrl()));
        }
    }
    m_pages->setCurrentWidget(m_server);
    statusBar()->showMessage(QStringLiteral("Server — %1").arg(config.model));
}

void MainWindow::showToolPage(const QString &toolName, const QString &location, bool alreadyUp) {
    const McpTool *tool = commands::findTool(toolName);
    const ToolLocation *loc = tool ? tool->location(location) : nullptr;
    if (!loc)
        return;
    // One page (one process, one log) per tool *location*: the remote log and
    // the local log are different things and both may be running.
    ProcessPage *&page = m_toolPages[toolName + '/' + loc->id];
    if (!page) {
        page = new ProcessPage;
        page->setStopOnBack(false);      // tunnels/services keep running while you chat
        m_pages->addWidget(page);
        connect(page, &ProcessPage::backRequested, this, &MainWindow::showLaunchPage);
    }
    const QString header = QStringLiteral("<b>%1</b> — %2").arg(tool->name, loc->label);
    if (!page->isRunning()) {
        if (!alreadyUp) {
            page->begin(header, loc->start, commands::environment());
        } else if (!page->hasContent()) {
            // Reachable, but not through anything this page started (e.g. a
            // tunnel opened in a terminal), so there is no output to show.
            page->showIdle(header, loc->start, commands::environment(),
                           QStringLiteral("[%1 already answers at %2, but it was not started from this app, so there is "
                                          "no log to show here.]\n[\"Run again\" runs the start/watch command anyway.]")
                               .arg(loc->label, loc->healthUrl));
        }
    }
    m_pages->setCurrentWidget(page);
    statusBar()->showMessage(QStringLiteral("%1 — %2").arg(tool->name, loc->label));
}

// ---- starting, continuing and reopening sessions ---------------------------------

void MainWindow::startSession(const LaunchConfig &config) {
    if (config.mode == LaunchConfig::PromptMode::Chat) {
        if (!confirmLeaveChat())
            return;
        m_chatRecord = history::create(true, config);
        startChat();
    } else if (config.scriptSource == LaunchConfig::ScriptSource::Generate) {
        m_pending = config;
        const QString configPath = commands::writeSessionConfig(config);
        QDir().mkpath(QFileInfo(config.scriptPath).absolutePath());
        m_scriptGen->begin(QStringLiteral("<b>Writing script with Claude</b> — %1").arg(QFileInfo(config.scriptPath).fileName()),
                           commands::generateScriptCommand(config, configPath),
                           commands::sessionEnvironment(configPath, config));
        m_pages->setCurrentWidget(m_scriptGen);
        refreshSessions();
    } else {
        runScript(config);
    }
    statusBar()->showMessage(config.summary());
}

// "Continue" on the start page: back to the open chat. If the model or the MCP
// servers were changed there, the agent is restarted with them; the
// conversation itself is in the saved messages, so nothing is lost.
void MainWindow::continueChat(const LaunchConfig &config) {
    if (!m_chatRecord.isValid())
        return;
    if (history::sameSettings(config, m_chatRecord.config) && m_chat->isAgentRunning()) {
        showChat();
        return;
    }
    if (!confirmLeaveChat())
        return;
    // Only the model or the effort changed: the running agent can take that.
    const bool live = history::sameBackendAndTools(config, m_chatRecord.config) && m_chat->isAgentRunning() &&
                      !m_chat->isBusy();
    const bool serverRestarted = restartServerFor(config);
    m_chatRecord.config  = config;
    m_chatRecord.updated = QDateTime::currentDateTime();
    history::save(m_chatRecord);
    if (live) {
        m_chat->applySettings(config, serverRestarted);
        showChat();
    } else {
        startChat(serverRestarted);
    }
}

// The model picker or the effort scale on the chat page: carry on with the
// same conversation, answered by another model or at another effort level.
void MainWindow::changeChatSettings(const LaunchConfig &config) {
    if (!m_chatRecord.isValid())
        return;
    const bool live = history::sameBackendAndTools(config, m_chatRecord.config) && m_chat->isAgentRunning();
    const bool serverRestarted = restartServerFor(config);
    m_chatRecord.config  = config;
    m_chatRecord.updated = QDateTime::currentDateTime();
    history::save(m_chatRecord);
    m_launch->setConfig(config);              // the start page shows what the chat uses
    if (live)
        m_chat->applySettings(config, serverRestarted);
    else
        startChat(serverRestarted);           // another backend: its agent takes over the saved conversation
    statusBar()->showMessage(config.summary());
    refreshSessions();
}

// A model server started from here with "{model}" in its command serves that
// one model. When the chat moves to another local model, start it again with
// the new one (the chat's next message waits for it; see agent.py).
bool MainWindow::restartServerFor(const LaunchConfig &config) {
    if (config.backend != LaunchConfig::Backend::LocalModel || config.model.isEmpty() ||
        !m_server->isRunning() || config.model == m_serverModel ||
        !settings::current().startCommand.contains(QLatin1String("{model}")))
        return false;
    m_serverModel = config.model;
    m_server->begin(QStringLiteral("<b>Server</b> — %1").arg(config.model.section('/', -1)),
                    commands::serverCommand(config), commands::environment());
    return true;
}

void MainWindow::openSession(const QString &id) {
    const SessionRecord record = history::load(id);
    if (!record.isValid()) {
        refreshSessions();
        return;
    }
    if (record.chat) {
        if (record.id == m_chatRecord.id && m_chat->isAgentRunning()) {
            showChat();                           // it never stopped: just go back to it
            return;
        }
        if (!confirmLeaveChat()) {
            refreshSessions();                    // put the selection back
            return;
        }
        m_chatRecord = record;
        m_launch->setConfig(record.config);       // so the start page shows what this chat uses
        startChat();
    } else {
        if (record.id != m_scriptRecord.id) {
            m_scriptRun->stop();                  // a run still going is saved under its own record first
            m_scriptRecord = record;
            QFile f(history::outputPath(record));
            const QString output = f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll())
                                                               : QStringLiteral("[no output was saved for this run]");
            const QString configPath = commands::writeSessionConfig(record.config);
            m_scriptRun->showIdle(QStringLiteral("<b>Script</b> — %1 · %2").arg(record.displayTitle(), record.config.summary()),
                                  commands::scriptRunCommand(record.config.scriptPath),
                                  commands::sessionEnvironment(configPath, record.config),
                                  output.trimmed());
        }
        showScriptRun();
    }
}

void MainWindow::startChat(bool waitForServer) {
    const LaunchConfig &config = m_chatRecord.config;
    const QString messages   = history::messagesPath(m_chatRecord);
    const QString configPath = commands::writeSessionConfig(config, messages, waitForServer);
    const bool claude = config.backend == LaunchConfig::Backend::ClaudeCode;
    m_chat->setLocalModels(m_launch->localModels());
    m_chat->begin(config,
                  claude ? commands::claudeChatCommand(configPath, config) : commands::agentChatCommand(configPath),
                  commands::sessionEnvironment(configPath, config), messages);
    m_launch->setOpenConversation(true, m_chatRecord.title);
    showChat();
}

void MainWindow::showChat() {
    m_chat->setLocalModels(m_launch->localModels());   // the server may have reported more since
    m_pages->setCurrentWidget(m_chat);
    statusBar()->showMessage(m_chatRecord.config.summary());
    refreshSessions();
}

void MainWindow::showScriptRun() {
    m_pages->setCurrentWidget(m_scriptRun);
    statusBar()->showMessage(m_scriptRecord.config.summary());
    refreshSessions();
}

// Starting another chat or changing the open one's settings stops its agent.
// That is harmless between turns, but would cut off a reply being written.
bool MainWindow::confirmLeaveChat() {
    if (!m_chat->isBusy())
        return true;
    return QMessageBox::question(this, "Reply in progress",
                                 QStringLiteral("“%1” is still being answered. Stop that reply?\n\n"
                                                "(To let it finish, choose No: the chat keeps running while you "
                                                "look at other pages.)")
                                     .arg(m_chatRecord.displayTitle())) == QMessageBox::Yes;
}

void MainWindow::runScript(const LaunchConfig &config) {
    m_scriptRun->stop();                          // a run still going is saved under its own record first
    m_scriptRecord = history::create(false, config, QFileInfo(config.scriptPath).fileName());
    const QString configPath = commands::writeSessionConfig(config);
    m_scriptRun->begin(QStringLiteral("<b>Script</b> — %1 · %2").arg(QFileInfo(config.scriptPath).fileName(), config.summary()),
                       commands::scriptRunCommand(config.scriptPath),
                       commands::sessionEnvironment(configPath, config));
    showScriptRun();
}

// Keep the saved copy of the script run on screen in step with the page.
void MainWindow::saveScriptRun(bool finished, int exitCode) {
    if (!m_scriptRecord.isValid())
        return;
    m_scriptRecord.finished = finished;
    m_scriptRecord.exitCode = exitCode;
    m_scriptRecord.updated  = QDateTime::currentDateTime();
    history::save(m_scriptRecord);
    QFile f(history::outputPath(m_scriptRecord));
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(m_scriptRun->output().toUtf8());
    refreshSessions();
}

void MainWindow::showLaunchPage() {
    m_pages->setCurrentWidget(m_launch);
    statusBar()->showMessage(m_server->isRunning() ? "Ready — server running (started here)" : "Ready");
    refreshSessions();
}

// ---- the session list ---------------------------------------------------------

void MainWindow::refreshSessions() {
    // What is on screen decides which row is highlighted.
    QString current;
    if (m_pages->currentWidget() == m_chat)
        current = m_chatRecord.id;
    else if (m_pages->currentWidget() == m_scriptRun)
        current = m_scriptRecord.id;

    const QString filter = m_filter->text().trimmed();
    m_sessions->clear();
    for (const SessionRecord &r : history::list()) {
        if (!filter.isEmpty() && !r.displayTitle().contains(filter, Qt::CaseInsensitive) &&
            !r.config.summary().contains(filter, Qt::CaseInsensitive))
            continue;
        QString detail;
        if (r.chat) {
            detail = r.config.shortModel();
            if (r.id == m_chatRecord.id && m_chat->isAgentRunning())
                detail += m_chat->isBusy() ? " · answering…" : " · open";
        } else {
            detail = r.id == m_scriptRecord.id && m_scriptRun->isRunning() ? QStringLiteral("running…")
                     : !r.finished                                        ? QStringLiteral("did not finish")
                     : r.exitCode == 0                                    ? QStringLiteral("finished")
                     : r.exitCode < 0                                     ? QStringLiteral("stopped")
                                                                          : QStringLiteral("failed (%1)").arg(r.exitCode);
        }
        // Second line: the time first, so it survives when the line is cut short.
        auto *item = new QListWidgetItem(QStringLiteral("%1\n%2 · %3%4")
                                             .arg(r.displayTitle(), when(r.updated), r.chat ? "" : "Script ", detail));
        item->setData(Qt::UserRole, r.id);
        item->setToolTip(QStringLiteral("%1\n%2\nStarted %3")
                             .arg(r.displayTitle(), r.config.summary(), r.created.toString("yyyy-MM-dd HH:mm")));
        m_sessions->addItem(item);
        if (r.id == current)
            m_sessions->setCurrentItem(item);
    }
}

void MainWindow::sessionMenu(const QPoint &pos) {
    QListWidgetItem *item = m_sessions->itemAt(pos);
    if (!item)
        return;
    SessionRecord record = history::load(item->data(Qt::UserRole).toString());
    if (!record.isValid())
        return;
    QMenu menu(this);
    QAction *open   = menu.addAction("Open");
    QAction *rename = menu.addAction("Rename…");
    menu.addSeparator();
    QAction *remove = menu.addAction("Delete");
    QAction *chosen = menu.exec(m_sessions->viewport()->mapToGlobal(pos));
    if (chosen == open) {
        openSession(record.id);
    } else if (chosen == rename) {
        bool ok = false;
        const QString title = QInputDialog::getText(this, "Rename session", "Title:", QLineEdit::Normal,
                                                    record.displayTitle(), &ok).trimmed();
        if (!ok || title.isEmpty())
            return;
        record.title = title;
        history::save(record);
        if (record.id == m_chatRecord.id) {
            m_chatRecord.title = title;
            m_launch->setOpenConversation(true, title);
        }
        if (record.id == m_scriptRecord.id)
            m_scriptRecord.title = title;
        refreshSessions();
    } else if (chosen == remove) {
        if (QMessageBox::question(this, "Delete session",
                                  QStringLiteral("Delete “%1”? This cannot be undone.").arg(record.displayTitle())) ==
            QMessageBox::Yes)
            deleteSession(record);
    }
}

void MainWindow::deleteSession(const SessionRecord &record) {
    // Forget it before stopping what runs in it: stopping would otherwise save it again.
    if (record.id == m_chatRecord.id) {
        m_chatRecord = {};
        m_chat->stopAgent();
        m_launch->setOpenConversation(false);
        if (m_pages->currentWidget() == m_chat)
            m_pages->setCurrentWidget(m_launch);
    }
    if (record.id == m_scriptRecord.id) {
        m_scriptRecord = {};
        m_scriptRun->stop();
        if (m_pages->currentWidget() == m_scriptRun)
            m_pages->setCurrentWidget(m_launch);
    }
    history::remove(record);
    refreshSessions();
}
