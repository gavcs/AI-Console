#include "LaunchPage.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QVBoxLayout>
#include <algorithm>

#include "Commands.h"
#include "McpServers.h"
#include "ServerDialog.h"

namespace {
constexpr int     kProbeIntervalMs = 2000;
const char        kGreenDot[] = "<span style='color:#2e9e44'>●</span>";
const char        kRedDot[]   = "<span style='color:#b33'>●</span>";
} // namespace

LaunchPage::LaunchPage(QWidget *parent)
    : QWidget(parent), m_net(new QNetworkAccessManager(this)) {
    QSettings settings;

    auto *title = new QLabel("Start a session");
    title->setObjectName("pageTitle");

    // ---- model + server ---------------------------------------------------
    // What is missing on this machine (see checkSetup); hidden when nothing is.
    m_setupNote = new QLabel;
    m_setupNote->setObjectName("notice");
    m_setupNote->setWordWrap(true);
    m_setupNote->setTextFormat(Qt::RichText);
    m_setupNote->hide();
    connect(m_setupNote, &QLabel::linkActivated, this, &LaunchPage::settingsRequested);

    m_model = new QComboBox;
    m_model->addItems(settings::current().models);
    m_model->setEditable(true);   // any model id can be typed in
    m_model->setCurrentText(settings.value("model", settings::current().models.value(0)).toString());

    m_serverStatus = new QLabel;
    m_serverButton = new QPushButton;
    connect(m_serverButton, &QPushButton::clicked, this,
            [this] { emit serverPageRequested(config(), m_serverUp); });
    auto *serverRow = new QHBoxLayout;
    serverRow->addWidget(m_serverStatus, 1);
    serverRow->addWidget(m_serverButton);

    // ---- who answers ------------------------------------------------------
    m_localBackend  = new QRadioButton("Local model");
    m_claudeBackend = new QRadioButton("Claude Code");
    m_claudeBackend->setToolTip("Chat with Claude through the claude CLI, using your Claude Code login. "
                                "It gets the MCP servers ticked below and nothing else.");
    auto *backendGroup = new QButtonGroup(this);   // separate from the "Prompt via" radios
    backendGroup->addButton(m_localBackend);
    backendGroup->addButton(m_claudeBackend);
    (settings.value("backend").toString() == "claude" ? m_claudeBackend : m_localBackend)->setChecked(true);
    auto *backendRow = new QHBoxLayout;
    backendRow->addWidget(m_localBackend);
    backendRow->addWidget(m_claudeBackend);
    backendRow->addStretch(1);

    m_claudeModel = new QComboBox;
    m_claudeModel->setEditable(true);              // an alias or a full model name
    m_claudeModel->addItems({QString(), "fable", "opus", "sonnet", "haiku"});
    m_claudeModel->lineEdit()->setPlaceholderText("Claude Code's default");
    m_claudeModel->setCurrentText(settings.value("claudeModel").toString());
    m_claudeModel->setMinimumWidth(220);           // room for the placeholder text
    m_claudeStatus = new QLabel;
    auto *claudeRow = new QHBoxLayout;
    claudeRow->addWidget(m_claudeModel);
    claudeRow->addWidget(m_claudeStatus, 1);

    auto *form = new QFormLayout;
    form->addRow("Run with:", backendRow);
    form->addRow("Model:",  m_model);
    form->addRow("Server:", serverRow);
    form->addRow("Claude model:", claudeRow);
    connect(m_claudeBackend, &QRadioButton::toggled, this, &LaunchPage::updateBackendWidgets);
    updateBackendWidgets();

    // ---- tools ------------------------------------------------------------
    // Rows come from the user's mcp_servers.json; nothing is built in.
    auto *toolBox = new QGroupBox("MCP servers");
    m_toolBoxLayout = new QVBoxLayout(toolBox);
    auto *addServer    = new QPushButton("Add MCP server…");
    auto *importServer = new QPushButton("Import JSON…");
    connect(addServer,    &QPushButton::clicked, this, [this] { editServer(QString()); });
    connect(importServer, &QPushButton::clicked, this, &LaunchPage::importServers);
    auto *toolButtons = new QHBoxLayout;
    toolButtons->addWidget(addServer);
    toolButtons->addWidget(importServer);
    toolButtons->addStretch();
    m_toolBoxLayout->addLayout(toolButtons);
    rebuildToolRows();

    // ---- prompt mode ------------------------------------------------------
    m_chatMode   = new QRadioButton("Chat — type prompts interactively");
    m_scriptMode = new QRadioButton("Python script");
    m_chatMode->setChecked(true);

    m_scriptFile   = new QRadioButton("Existing file:");
    m_scriptGen    = new QRadioButton("Write a new one with Claude:");
    m_scriptFile->setChecked(true);
    m_scriptPath   = new QLineEdit(settings.value("scriptPath").toString());
    m_scriptPath->setPlaceholderText("/path/to/script.py");
    m_browse       = new QPushButton("Browse…");
    connect(m_browse, &QPushButton::clicked, this, [this] {
        const QString start = m_scriptPath->text().isEmpty() ? commands::scriptsDir() : m_scriptPath->text();
        const QString f = QFileDialog::getOpenFileName(this, "Choose a Python script", start, "Python (*.py)");
        if (!f.isEmpty())
            m_scriptPath->setText(f);
    });
    m_scriptPrompt = new QPlainTextEdit(settings.value("scriptPrompt").toString());
    m_scriptPrompt->setPlaceholderText("What should the script do? e.g. “List every function in the loaded "
                                       "binary that calls strcpy and write a one-line summary of each.”");
    m_scriptPrompt->setFixedHeight(70);

    auto *fileRow = new QHBoxLayout;
    fileRow->addWidget(m_scriptFile);
    fileRow->addWidget(m_scriptPath, 1);
    fileRow->addWidget(m_browse);

    m_scriptBox = new QWidget;
    auto *scriptLayout = new QVBoxLayout(m_scriptBox);
    scriptLayout->setContentsMargins(24, 0, 0, 0);
    scriptLayout->addLayout(fileRow);
    scriptLayout->addWidget(m_scriptGen);
    scriptLayout->addWidget(m_scriptPrompt);

    auto *modeBox    = new QGroupBox("Prompt via");
    auto *modeLayout = new QVBoxLayout(modeBox);
    modeLayout->addWidget(m_chatMode);
    modeLayout->addWidget(m_scriptMode);
    modeLayout->addWidget(m_scriptBox);

    // Explicit groups: left to themselves, radio buttons that share a parent
    // can be un-ticked by clicking the ticked one, leaving none selected.
    auto *modeGroup = new QButtonGroup(this);
    modeGroup->addButton(m_chatMode);
    modeGroup->addButton(m_scriptMode);
    auto *sourceGroup = new QButtonGroup(this);
    sourceGroup->addButton(m_scriptFile);
    sourceGroup->addButton(m_scriptGen);
    for (QRadioButton *r : {m_chatMode, m_scriptMode, m_scriptFile, m_scriptGen})
        connect(r, &QRadioButton::toggled, this, &LaunchPage::updateScriptWidgets);
    updateScriptWidgets();

    // ---- start ------------------------------------------------------------
    m_start = new QPushButton("Start");
    m_start->setDefault(true);
    m_start->setObjectName("primary");
    connect(m_start, &QPushButton::clicked, this, [this] {
        const LaunchConfig c = config();
        if (confirmStart(c))
            emit startRequested(c);
    });
    // Only while a chat is open: carry on with it, using whatever is selected above.
    m_continue = new QPushButton("Continue chat");
    m_continue->setToolTip("Go back to the open conversation. If you changed the model or the MCP servers above, "
                           "it continues with those.");
    m_continue->hide();
    connect(m_continue, &QPushButton::clicked, this, [this] {
        LaunchConfig c = config();
        c.mode = LaunchConfig::PromptMode::Chat;
        if (confirmStart(c))
            emit continueRequested(c);
    });
    auto *startRow = new QHBoxLayout;
    startRow->addStretch(1);
    startRow->addWidget(m_continue);
    startRow->addWidget(m_start);

    // ---- layout: a fixed-width card ---------------------------------------
    auto *card       = new QWidget;
    card->setMaximumWidth(860);
    auto *cardLayout = new QVBoxLayout(card);
    cardLayout->addWidget(title);
    cardLayout->addWidget(m_setupNote);
    auto *modelBox = new QGroupBox("Model");
    modelBox->setLayout(form);
    cardLayout->addWidget(modelBox);
    cardLayout->addWidget(toolBox);
    cardLayout->addWidget(modeBox);
    cardLayout->addLayout(startRow);

    auto *row = new QHBoxLayout;
    row->addStretch(1);
    row->addWidget(card, 100);   // take the width, up to the card's maximum
    row->addStretch(1);

    auto *layout = new QVBoxLayout(this);
    layout->addStretch();
    layout->addLayout(row);
    layout->addStretch();

    m_probeTimer.setInterval(kProbeIntervalMs);
    connect(&m_probeTimer, &QTimer::timeout, this, &LaunchPage::probeAll);
    setServerStatus(false, {});
    checkSetup();
}

void LaunchPage::settingsChanged() {
    const QString chosen = m_model->currentText();
    m_model->clear();
    m_model->addItems(settings::current().models);
    // Keep what was selected if it still makes sense, else take the first of the new list.
    m_model->setCurrentText(settings::current().models.isEmpty() || settings::current().models.contains(chosen)
                                ? chosen : settings::current().models.first());
    if (QNetworkReply *inFlight = m_probes.take("server"))   // an answer from the old address
        inFlight->abort();
    setServerStatus(false, {});
    updateBackendWidgets();
    checkSetup();
    probeAll();
}

// Things a new user is likely to be missing, said in plain words on the start
// page instead of surfacing later as an error in a log.
void LaunchPage::checkSetup() {
    const QString python = commands::pythonProgram();
    const QProcessEnvironment env = commands::environment();
    auto *probe = new QProcess(this);
    probe->setProcessEnvironment(env);
    const auto done = [this, probe](const QString &problem) {
        m_pythonProblem = problem;
        probe->deleteLater();
        updateSetupNote();
    };
    connect(probe, &QProcess::finished, this, [done, python](int code, QProcess::ExitStatus status) {
        const bool ok = status == QProcess::NormalExit && code == 0;
        done(ok ? QString()
                : QStringLiteral("<b>%1</b> can’t import the <b>mcp</b> package, which chats with a local model and "
                                 "scripts need. Install it with <code>%1 -m pip install -r requirements.txt</code>, "
                                 "or choose another Python in <a href='settings'>Settings</a>.")
                      .arg(python.toHtmlEscaped()));
    });
    connect(probe, &QProcess::errorOccurred, this, [done, python](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            done(QStringLiteral("Python was not found (<b>%1</b>). Install Python 3, or choose the interpreter in "
                                "<a href='settings'>Settings</a>.").arg(python.toHtmlEscaped()));
    });
    probe->start(commands::resolveProgram(python, env), {"-c", "import mcp, httpx"});
}

void LaunchPage::updateSetupNote() {
    QStringList problems;
    if (!m_pythonProblem.isEmpty())
        problems << m_pythonProblem;
    // The server's start command, unless a server is answering anyway.
    const Command server = commands::serverCommand(config());
    if (server.isValid() && !m_serverUp) {
        const QString resolved = commands::resolveProgram(server.program, commands::environment());
        if (!QFileInfo(resolved).isExecutable())
            problems << QStringLiteral("The model server command <b>%1</b> is not installed (or not on the PATH), so "
                                       "“Start server” can’t work. Install it, or choose another provider in "
                                       "<a href='settings'>Settings</a>. Claude Code chats don’t need it.")
                            .arg(server.program.toHtmlEscaped());
    }
    m_setupNote->setText(problems.join("<br><br>"));
    m_setupNote->setVisible(!problems.isEmpty());
}

// (Re)create one row per configured server. Called at start and after every
// edit of the server list.
void LaunchPage::rebuildToolRows() {
    if (m_toolRows) {
        // Not `delete`: this can run from a click on a button inside the old rows.
        m_toolBoxLayout->removeWidget(m_toolRows);
        m_toolRows->hide();
        m_toolRows->deleteLater();
    }
    m_tools.clear();
    m_toolRows = new QWidget;
    m_toolBoxLayout->insertWidget(0, m_toolRows);
    auto *grid = new QFormLayout(m_toolRows);
    grid->setContentsMargins(0, 0, 0, 0);
    QSettings settings;
    if (!commands::serverState().loadError.isEmpty())
        grid->addRow(new QLabel(QStringLiteral("<b>Could not read %1:</b> %2")
                                    .arg(mcp::configPath().toHtmlEscaped(), commands::serverState().loadError.toHtmlEscaped())));
    for (const McpTool &tool : commands::allTools()) {
        ToolRow row;
        row.enabled = new QCheckBox(tool.name);
        row.enabled->setToolTip(tool.description);
        row.enabled->ensurePolished();                                   // size it with the app's style applied
        row.enabled->setMinimumWidth(row.enabled->sizeHint().width() + 2);   // never clip the server's name
        row.enabled->setChecked(settings.value("tool/" + tool.name + "/enabled", false).toBool());

        // Which location to use: the dropdown lists every place this tool's
        // service can run. Status, the Start/Log button and the session all
        // follow whatever is selected here.
        row.location = new QComboBox;
        for (const ToolLocation &loc : tool.locations)
            row.location->addItem(loc.label, loc.id);
        row.location->setEnabled(tool.locations.size() > 1);
        QString wanted = settings.value("tool/" + tool.name + "/location").toString();
        if (wanted.isEmpty() && settings.contains("tool/" + tool.name + "/remote"))   // setting from older builds
            wanted = settings.value("tool/" + tool.name + "/remote").toBool() ? "remote" : "local";
        row.location->setCurrentIndex(qMax(0, row.location->findData(wanted)));

        row.status = new QLabel;
        row.button = new QPushButton;
        const QString name = tool.name;
        connect(row.button, &QPushButton::clicked, this, [this, name] {
            emit toolPageRequested(name, toolLocationId(name), m_tools[name].up);
        });
        connect(row.location, &QComboBox::currentIndexChanged, this, [this, name] { toolLocationChanged(name); });
        connect(row.enabled, &QCheckBox::toggled, this, [name](bool on) {
            QSettings().setValue("tool/" + name + "/enabled", on);
        });
        auto *edit = new QPushButton("Edit…");
        connect(edit, &QPushButton::clicked, this, [this, name] { editServer(name); });

        auto *h = new QHBoxLayout;
        h->addWidget(row.location);
        h->addWidget(row.status, 1);
        h->addWidget(row.button);
        h->addWidget(edit);
        grid->addRow(row.enabled, h);
        m_tools.insert(tool.name, row);
    }
    if (m_tools.isEmpty())
        grid->addRow(new QLabel("<i>No MCP servers yet. Add one below; the Add dialog also offers presets.</i>"));
    for (const QString &name : m_tools.keys())
        setToolStatus(name, false);
}

void LaunchPage::editServer(const QString &name) {
    QList<ServerConfig> all = commands::servers();
    int index = -1;
    QStringList others;
    for (int i = 0; i < all.size(); ++i) {
        if (all[i].name == name && !name.isEmpty())
            index = i;
        else
            others << all[i].name;
    }
    ServerDialog dlg(index >= 0 ? all[index] : ServerConfig{}, others, index < 0, this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    if (dlg.deleteRequested())
        all.removeAt(index);
    else if (index >= 0)
        all[index] = dlg.server();
    else
        all.append(dlg.server());
    applyServers(all);
}

void LaunchPage::importServers() {
    const QList<ServerConfig> imported = ServerDialog::importJson(this);
    if (imported.isEmpty())
        return;
    QList<ServerConfig> all = commands::servers();
    for (const ServerConfig &s : imported) {
        int existing = -1;
        for (int i = 0; i < all.size(); ++i)
            if (all[i].name == s.name)
                existing = i;
        if (existing < 0) {
            all.append(s);
        } else if (QMessageBox::question(this, "Replace server",
                                         QStringLiteral("There is already a server called \"%1\". Replace it?").arg(s.name)) ==
                   QMessageBox::Yes) {
            all[existing] = s;
        }
    }
    applyServers(all);
}

// Save the edited list (mcp_servers.json) and show it.
void LaunchPage::applyServers(const QList<ServerConfig> &servers) {
    // Keep the on-screen order the same as the file's (JSON objects are sorted by key).
    QList<ServerConfig> sorted = servers;
    std::sort(sorted.begin(), sorted.end(), [](const ServerConfig &a, const ServerConfig &b) { return a.name < b.name; });
    QString error;
    if (!commands::setServers(sorted, &error)) {
        QMessageBox::warning(this, "Can't save", QStringLiteral("Could not write %1:\n%2").arg(mcp::configPath(), error));
        return;
    }
    rebuildToolRows();
    emit toolsChanged();
    probeAll();
}

LaunchConfig LaunchPage::config() const {
    LaunchConfig c;
    c.model = m_model->currentText().trimmed();
    c.backend     = m_claudeBackend->isChecked() ? LaunchConfig::Backend::ClaudeCode : LaunchConfig::Backend::LocalModel;
    c.claudeModel = m_claudeModel->currentText().trimmed();
    c.mode  = m_scriptMode->isChecked() ? LaunchConfig::PromptMode::Script : LaunchConfig::PromptMode::Chat;
    for (auto it = m_tools.cbegin(); it != m_tools.cend(); ++it)
        if (it->enabled->isChecked())
            c.tools.append({it.key(), it->location->currentData().toString()});
    if (m_scriptGen->isChecked()) {
        c.scriptSource = LaunchConfig::ScriptSource::Generate;
        c.scriptPrompt = m_scriptPrompt->toPlainText().trimmed();
        c.scriptPath   = commands::scriptsDir() + "/gen_" +
                         QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss") + ".py";
    } else {
        c.scriptSource = LaunchConfig::ScriptSource::ExistingFile;
        c.scriptPath   = m_scriptPath->text().trimmed();
    }
    return c;
}

// Checks, questions and bookkeeping shared by Start and Continue.
bool LaunchPage::confirmStart(const LaunchConfig &c) {
    QString why;
    if (!validate(c, &why)) {
        QMessageBox::warning(this, "Can't start", why);
        return false;
    }
    // A ticked tool whose service isn't reachable would give a session
    // with (almost) none of its tools: say so before starting.
    QStringList down;
    for (const ToolChoice &choice : c.tools) {
        const McpTool *tool = commands::findTool(choice.name);
        const ToolLocation *loc = tool ? tool->location(choice.location) : nullptr;
        if (loc && !loc->healthUrl.isEmpty() && !m_tools[choice.name].up)
            down << QStringLiteral("%1 (%2)").arg(choice.name, loc->label);
    }
    if (!down.isEmpty() &&
        QMessageBox::question(this, "Service not running",
                              "Not reachable right now:\n  " + down.join("\n  ") +
                              "\n\nIts tools will be missing from this session. Start anyway?") != QMessageBox::Yes)
        return false;
    QSettings s;
    s.setValue("model", m_model->currentText().trimmed());
    s.setValue("backend", m_claudeBackend->isChecked() ? "claude" : "local");
    s.setValue("claudeModel", m_claudeModel->currentText().trimmed());
    s.setValue("scriptPath", m_scriptPath->text());
    s.setValue("scriptPrompt", m_scriptPrompt->toPlainText());
    return true;
}

void LaunchPage::setConfig(const LaunchConfig &c) {
    (c.backend == LaunchConfig::Backend::ClaudeCode ? m_claudeBackend : m_localBackend)->setChecked(true);
    if (!c.model.isEmpty())
        m_model->setCurrentText(c.model);
    m_claudeModel->setCurrentText(c.claudeModel);
    for (auto it = m_tools.begin(); it != m_tools.end(); ++it) {
        const auto chosen = std::find_if(c.tools.cbegin(), c.tools.cend(),
                                         [&](const ToolChoice &t) { return t.name == it.key(); });
        it->enabled->setChecked(chosen != c.tools.cend());
        if (chosen != c.tools.cend() && it->location->findData(chosen->location) >= 0)
            it->location->setCurrentIndex(it->location->findData(chosen->location));
    }
}

void LaunchPage::setOpenConversation(bool open, const QString &title) {
    const QString shown = title.size() > 32 ? title.left(31) + "…" : title;
    m_continue->setText(shown.isEmpty() ? QStringLiteral("Continue chat") : QStringLiteral("Continue “%1”").arg(shown));
    m_continue->setVisible(open);
    m_start->setText(open ? "Start new" : "Start");
}

bool LaunchPage::validate(const LaunchConfig &c, QString *why) const {
    const bool claude = c.backend == LaunchConfig::Backend::ClaudeCode;
    if (claude && c.mode == LaunchConfig::PromptMode::Script) {
        *why = "Scripts run against the local model. Choose \"Local model\" to run a script, "
               "or \"Chat\" to talk to Claude Code.";
        return false;
    }
    if (claude && commands::findClaude().isEmpty()) {
        *why = "The claude command was not found. Install Claude Code, or choose it in Settings.";
        return false;
    }
    if (!claude && c.model.isEmpty()) { *why = "Pick or type a model id."; return false; }
    if (c.mode == LaunchConfig::PromptMode::Script) {
        if (c.scriptSource == LaunchConfig::ScriptSource::ExistingFile && !QFile::exists(c.scriptPath)) {
            *why = c.scriptPath.isEmpty() ? "Choose the script to run." : "Script not found:\n" + c.scriptPath;
            return false;
        }
        if (c.scriptSource == LaunchConfig::ScriptSource::Generate && c.scriptPrompt.isEmpty()) {
            *why = "Describe what the script should do.";
            return false;
        }
    }
    return true;
}

void LaunchPage::updateScriptWidgets() {
    const bool script = m_scriptMode->isChecked();
    m_scriptBox->setEnabled(script);
    const bool gen = m_scriptGen->isChecked();
    m_scriptPath->setEnabled(!gen);
    m_browse->setEnabled(!gen);
    m_scriptPrompt->setEnabled(gen);
}

// The rows that belong to the other backend are greyed out, not hidden, so
// the local server can still be seen (and its log opened) either way.
void LaunchPage::updateBackendWidgets() {
    const bool claude = m_claudeBackend->isChecked();
    m_model->setEnabled(!claude);
    m_claudeModel->setEnabled(claude);
    const QString path = commands::findClaude();
    m_claudeStatus->setText(path.isEmpty() ? QStringLiteral("%1 claude command not found").arg(kRedDot)
                                           : QStringLiteral("%1 claude command found").arg(kGreenDot));
    m_claudeStatus->setToolTip(path);   // the path can be long (e.g. inside a VS Code extension)
    m_claudeStatus->setEnabled(claude);
}

// ---- probing ------------------------------------------------------------------

void LaunchPage::showEvent(QShowEvent *event) {
    QWidget::showEvent(event);
    probeAll();
    m_probeTimer.start();
}

void LaunchPage::hideEvent(QHideEvent *event) {
    QWidget::hideEvent(event);
    m_probeTimer.stop();
}

void LaunchPage::probeAll() {
    auto probe = [this](const QString &key, const QString &url, auto onDone) {
        if (m_probes.contains(key))   // previous probe still in flight
            return;
        QNetworkRequest req{QUrl(url)};
        req.setTransferTimeout(1500);
        if (!settings::current().apiKey.isEmpty())
            req.setRawHeader("Authorization", "Bearer " + settings::current().apiKey.toUtf8());
        QNetworkReply *reply = m_net->get(req);
        m_probes.insert(key, reply);
        connect(reply, &QNetworkReply::finished, this, [this, key, reply, onDone] {
            if (m_probes.value(key) == reply)
                m_probes.remove(key);
            reply->deleteLater();
            onDone(reply);
        });
    };

    probe("server", commands::serverBaseUrl() + "/models", [this](QNetworkReply *r) {
        if (r->error() == QNetworkReply::OperationCanceledError)   // the address changed meanwhile
            return;
        if (r->error() != QNetworkReply::NoError) { setServerStatus(false, {}); return; }
        QStringList ids;
        const QJsonArray data = QJsonDocument::fromJson(r->readAll()).object()["data"].toArray();
        for (const QJsonValue &v : data)
            ids << v.toObject()["id"].toString();
        setServerStatus(true, ids);
    });

    for (const McpTool &tool : commands::allTools())
        probeTool(tool.name);
}

// Empty if there is no such row (any more).
QString LaunchPage::toolLocationId(const QString &name) const {
    const auto it = m_tools.constFind(name);
    return it == m_tools.constEnd() ? QString() : it->location->currentData().toString();
}

// Probe the location currently selected for `name`. The reply is keyed by
// tool AND location, and dropped if the dropdown moved on meanwhile, so one
// location's answer can never be shown as another's status.
void LaunchPage::probeTool(const QString &name) {
    const McpTool *tool = commands::findTool(name);
    const ToolLocation *loc = tool ? tool->location(toolLocationId(name)) : nullptr;
    if (!loc || loc->healthUrl.isEmpty())   // nothing to probe: starts with the session
        return;
    const QString locId = loc->id;
    const QString key   = name + '/' + locId;
    if (m_probes.contains(key))             // previous probe still in flight
        return;
    QNetworkRequest req{QUrl(loc->healthUrl)};
    req.setTransferTimeout(1500);
    QNetworkReply *reply = m_net->get(req);
    m_probes.insert(key, reply);
    connect(reply, &QNetworkReply::finished, this, [this, key, name, locId, reply] {
        m_probes.remove(key);
        reply->deleteLater();
        if (toolLocationId(name) != locId)   // dropdown moved on, or the row is gone
            return;
        // Any HTTP status means something answered on the port; only
        // transport-level failures (refused, timeout) count as down.
        const bool answered = reply->error() == QNetworkReply::NoError ||
                              reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).isValid();
        setToolStatus(name, answered);
    });
}

void LaunchPage::toolLocationChanged(const QString &name) {
    QSettings().setValue("tool/" + name + "/location", toolLocationId(name));
    setToolStatus(name, false);   // not known to be up until the new location answers
    probeTool(name);
}

void LaunchPage::setServerStatus(bool up, const QStringList &models) {
    QString where = QStringLiteral("%1:%2").arg(commands::serverHost()).arg(commands::serverPort());
    if (up) {
        // Some servers serve one model, others list everything they have.
        const QString serving = models.size() <= 2 ? models.join(", ") : QStringLiteral("%1 models").arg(models.size());
        m_serverStatus->setText(QStringLiteral("%1 running on %2 — %3").arg(kGreenDot, where, serving));
        m_serverStatus->setToolTip(models.join('\n'));
        m_serverButton->setText("Server log");
        if (!m_serverUp) {   // down→up: offer what is actually served
            for (const QString &id : models)
                if (m_model->findText(id) < 0)
                    m_model->addItem(id);
            if (!models.isEmpty() && !models.contains(m_model->currentText().trimmed()))
                m_model->setCurrentText(models.first());
        }
    } else {
        m_serverStatus->setText(QStringLiteral("%1 not running on %2").arg(kRedDot, where));
        m_serverStatus->setToolTip(QString());
        m_serverButton->setText("Start server");
    }
    const bool changed = m_serverUp != up;
    m_serverUp = up;
    // With no start command in the settings there is nothing to start from here.
    m_serverButton->setVisible(up || !settings::current().startCommand.trimmed().isEmpty());
    if (changed)
        updateSetupNote();
}

void LaunchPage::setToolStatus(const QString &name, bool up) {
    if (!m_tools.contains(name))
        return;
    ToolRow &row = m_tools[name];
    const McpTool *tool = commands::findTool(name);
    const ToolLocation *loc = tool ? tool->location(toolLocationId(name)) : nullptr;
    if (!loc || loc->healthUrl.isEmpty()) {   // the MCP server is the tool; nothing to run first
        row.status->setText(QStringLiteral("%1 starts with the session").arg(kGreenDot));
        row.button->hide();
        row.up = true;
        return;
    }
    // Name the location in the status so it is clear which one this is about.
    const QString addr = QUrl(loc->healthUrl).toString(QUrl::RemoveScheme | QUrl::RemovePath).mid(2);
    row.status->setText(up ? QStringLiteral("%1 %2 service up (%3)").arg(kGreenDot, loc->id, addr)
                           : QStringLiteral("%1 %2 service not reachable (%3)").arg(kRedDot, loc->id, addr));
    row.status->setToolTip(loc->healthUrl);
    row.button->setText(up ? "Log" : "Start");
    row.button->setVisible(up || loc->start.isValid());
    row.up = up;
}
