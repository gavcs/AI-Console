#pragma once
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QUrl>
#include "LaunchConfig.h"
#include "Settings.h"

// ---------------------------------------------------------------------------
// How things are run: the model server's address and command, the environment
// child processes get, and the commands behind a tool's Start button.
// What is specific to one machine is not decided here: the model server and
// the programs to use are the user's settings (Settings.h), and the MCP
// servers are the user's mcp_servers.json (McpServers.h).
// ---------------------------------------------------------------------------

struct Command {
    QString     program;
    QStringList args;
    QString     cwd;      // empty = inherit
    QString     display;  // what the log shows instead of the full command line (optional)

    bool    isValid()  const { return !program.isEmpty(); }
    QString toString() const { return display.isEmpty() ? program + ' ' + args.join(' ') : display; }
};

// One place a tool's backing service can run (e.g. "on this Mac" or "on the
// build server"). Each location has its OWN address on this Mac, so two
// locations can be up at the same time and the app always knows which one it
// is looking at. Built from the user's config by mcp::toTool (McpServers.h).
struct ToolLocation {
    QString     id;          // "local" / "remote": saved in settings, shown in summaries
    QString     label;       // dropdown text
    QString     healthUrl;   // answers (any HTTP status) while this location is reachable;
                             // empty = nothing to start, the MCP server is the tool
    Command     start;       // starts the service / makes it reachable (invalid = can't start it from here)
    QJsonObject mcpServer;   // entry for agent.py's "mcp_servers" (see python/agent.py)
};

// An MCP tool server the app can offer, and the locations it can run in.
struct McpTool {
    QString             name;          // UI label and MCP server name (prefixes tool names)
    QString             description;
    QList<ToolLocation> locations;     // at least one; the first is the default

    const ToolLocation *location(const QString &id) const {
        for (const ToolLocation &l : locations)
            if (l.id == id)
                return &l;
        return locations.isEmpty() ? nullptr : &locations.first();
    }
};

namespace commands {

// ---- model server ---------------------------------------------------------
// Where the OpenAI-compatible API is (Settings > Address).
inline QUrl serverUrl() {
    const int testPort = qEnvironmentVariableIntValue("AI_CONSOLE_SERVER_PORT");   // tests: a fake server
    if (testPort > 0)
        return QUrl(QStringLiteral("http://127.0.0.1:%1/v1").arg(testPort));
    return QUrl(settings::current().baseUrl);
}
inline QString serverHost() { return serverUrl().host(); }
inline int     serverPort() { return serverUrl().port(serverUrl().scheme() == QLatin1String("https") ? 443 : 80); }
inline QString serverBaseUrl() {
    QString url = serverUrl().toString();
    while (url.endsWith('/'))
        url.chop(1);
    return url;
}

// ---- paths ----------------------------------------------------------------
// The project's python/ directory (agent.py, gen_script.py). The binary lives
// in <project>/bin (make) or <project>/build (cmake); look upward from there.
inline QString pythonDir() {
    const QByteArray override = qgetenv("AI_CONSOLE_PYTHON_DIR");   // for tests / unusual layouts
    if (!override.isEmpty())
        return QString::fromLocal8Bit(override);
    const QDir app(QCoreApplication::applicationDirPath());
    for (const char *rel : {"../python", "../../python", "python"}) {
        const QString p = QDir::cleanPath(app.filePath(QString::fromLatin1(rel)));
        if (QFile::exists(p + "/agent.py"))
            return p;
    }
    return app.filePath("../python");   // best guess; agent.py missing will show in the log
}

// Where "write a script with Claude" puts its scripts (Settings > Scripts folder).
inline QString defaultScriptsDir() {
    return QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/AI Console/scripts";
}
inline QString scriptsDir() {
    const QString chosen = settings::current().scriptsDir;
    return chosen.isEmpty() ? defaultScriptsDir() : chosen;
}

// The interpreter for python/*.py and for scripts (Settings > Python).
inline QString pythonProgram() {
    const QString chosen = settings::current().python;
    return chosen.isEmpty() ? QStringLiteral("python3") : chosen;
}

// The chat and a script run each have their own config file, so that starting
// one never changes what the other (possibly still running) was started with.
inline QString sessionConfigPath(bool chat = true) {
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    return dir + (chat ? "/session.json" : "/script_session.json");
}

// ---- environment ----------------------------------------------------------
// What every child process gets. An app started from Finder only has
// /usr/bin:/bin:/usr/sbin:/sbin on its PATH, not what your shell profile adds,
// so the usual install locations are added when they are missing. A folder
// that is already on the PATH keeps its place: started from a terminal (or
// with a virtual environment active), the app finds the same programs your
// shell does. python/ goes on PYTHONPATH so that scripts can
// `from agent import Agent`.
inline QProcessEnvironment environment() {
    auto env = QProcessEnvironment::systemEnvironment();
    const QStringList path = env.value(QStringLiteral("PATH")).split(':', Qt::SkipEmptyParts);
    QStringList missing;
    for (const QString &dir : {QDir::homePath() + QStringLiteral("/.local/bin"),
                               QStringLiteral("/usr/local/bin"),
                               QStringLiteral("/opt/homebrew/bin")})
        if (!path.contains(dir))
            missing << dir;
    env.insert(QStringLiteral("PATH"), (missing + path).join(':'));
    const QString pp = env.value(QStringLiteral("PYTHONPATH"));
    env.insert(QStringLiteral("PYTHONPATH"), pp.isEmpty() ? pythonDir() : pythonDir() + ':' + pp);
    env.insert(QStringLiteral("PYTHONUNBUFFERED"), QStringLiteral("1"));
    return env;
}

// QProcess resolves a bare program name against *this* process's PATH, not the
// one in the child's environment — so resolve it ourselves against `env`.
inline QString resolveProgram(const QString &program, const QProcessEnvironment &env) {
    if (program.contains('/'))
        return program;
    const QString found = QStandardPaths::findExecutable(program, env.value(QStringLiteral("PATH")).split(':'));
    return found.isEmpty() ? program : found;
}

// Environment for anything that goes through agent.py (chat, scripts, generator).
inline QProcessEnvironment sessionEnvironment(const QString &configPath, const LaunchConfig &c) {
    auto env = environment();
    env.insert(QStringLiteral("AI_CONSOLE_CONFIG"), configPath);
    env.insert(QStringLiteral("OPENAI_BASE_URL"),   serverBaseUrl());
    const QString key = settings::current().apiKey;
    env.insert(QStringLiteral("OPENAI_API_KEY"),    key.isEmpty() ? QStringLiteral("not-needed") : key);
    env.insert(QStringLiteral("MODEL"),             c.model);
    if (!settings::current().claude.isEmpty())     // python/claude_agent.py's own search honours this
        env.insert(QStringLiteral("AI_CONSOLE_CLAUDE"), settings::current().claude);
    return env;
}

// ---- the model server -----------------------------------------------------
// What "Start server" runs: the start command from the settings, with {model},
// {port} and {host} filled in. Invalid (nothing to run) if the settings have no
// start command, i.e. the server is started somewhere else.
//
// The command is run as you, never with sudo: nobody can answer a password
// prompt from here. If a server only works with sudo in a terminal, its files
// are probably owned by root (see Troubleshooting in the README).
inline Command serverCommand(const LaunchConfig &c) {
    QStringList parts = QProcess::splitCommand(settings::current().startCommand);
    if (parts.isEmpty())
        return {};
    for (QString &part : parts) {   // after splitting, so a model id with a space stays one argument
        part.replace(QLatin1String("{model}"), c.model);
        part.replace(QLatin1String("{port}"), QString::number(serverPort()));
        part.replace(QLatin1String("{host}"), serverHost());
    }
    const QString program = parts.takeFirst();
    return {program, parts, {}};
}

// ---- services behind MCP tools --------------------------------------------
inline QString shellQuote(const QString &s) {
    QString q = s;
    q.replace(QLatin1Char('\''), QLatin1String("'\\''"));
    return QLatin1Char('\'') + q + QLatin1Char('\'');
}

// What python/service_watch.sh needs to know about a service (see that file).
struct ServiceSpec {
    QString name;        // label in the log
    int     port = 0;    // port it listens on, on the machine it runs on
    QString startCmd;    // shell command that starts it there if it isn't running (empty = never start it)
    QString infoPath;    // HTTP path describing the running service; printed in the log
    QString logFile;     // the service's own log file there; new lines are copied into the log
    QString hint;        // shown if it hasn't come up after a minute
    QString readyNote;   // shown once it is up
};

inline QStringList serviceEnv(const ServiceSpec &s) {
    QStringList env{QStringLiteral("NAME=") + s.name, QStringLiteral("PORT=%1").arg(s.port)};
    if (!s.startCmd.isEmpty())  env << QStringLiteral("START_CMD=") + s.startCmd;
    if (!s.infoPath.isEmpty())  env << QStringLiteral("INFO_PATH=") + s.infoPath;
    if (!s.logFile.isEmpty())   env << QStringLiteral("LOG_FILE=") + s.logFile;
    if (!s.hint.isEmpty())      env << QStringLiteral("HINT=") + s.hint;
    if (!s.readyNote.isEmpty()) env << QStringLiteral("READY_NOTE=") + s.readyNote;
    return env;
}

inline QString serviceWatchScript() { return pythonDir() + QStringLiteral("/service_watch.sh"); }

// Start (if needed) and watch a service on this Mac.
inline Command localServiceCommand(const ServiceSpec &s) {
    return {QStringLiteral("/usr/bin/env"),
            serviceEnv(s) << QStringLiteral("/bin/bash") << serviceWatchScript(),
            {},
            QStringLiteral("service_watch.sh: %1 on this Mac, port %2").arg(s.name).arg(s.port)};
}

// Start (if needed) and watch a service on `host`, over ONE ssh connection that
// also forwards 127.0.0.1:<localPort> here to the service's port there. ssh was
// chosen over a "start things for me" daemon on the remote machine: it needs
// nothing installed or kept running there, it is already authenticated by your
// key, and the same connection gives the start, the tunnel and the log.
// The watch script is sent as the remote command, so nothing is copied over.
inline Command remoteServiceCommand(const QString &host, int localPort, const ServiceSpec &s) {
    QFile f(serviceWatchScript());
    if (!f.open(QIODevice::ReadOnly))
        return {};   // python/service_watch.sh missing: ProcessPage reports "no command configured"
    QStringList remote;
    for (const QString &assignment : serviceEnv(s)) {
        const int eq = assignment.indexOf('=');
        remote << assignment.left(eq + 1) + shellQuote(assignment.mid(eq + 1));
    }
    remote << QStringLiteral("bash") << QStringLiteral("-c") << shellQuote(QString::fromUtf8(f.readAll()));
    const QString forward = QStringLiteral("127.0.0.1:%1:127.0.0.1:%2").arg(localPort).arg(s.port);
    return {QStringLiteral("ssh"),
            {QStringLiteral("-o"), QStringLiteral("BatchMode=yes"),            // never wait for a password prompt nobody can see
             QStringLiteral("-o"), QStringLiteral("ConnectTimeout=10"),
             QStringLiteral("-o"), QStringLiteral("ExitOnForwardFailure=yes"), // local port taken = fail loudly
             QStringLiteral("-o"), QStringLiteral("ServerAliveInterval=15"),
             QStringLiteral("-o"), QStringLiteral("ServerAliveCountMax=3"),
             QStringLiteral("-L"), forward,
             host,
             remote.join(' ')},
            {},
            QStringLiteral("ssh -L %1 %2  (runs service_watch.sh there: %3, port %4)")
                .arg(forward, host, s.name).arg(s.port)};
}

// ---- Claude Code ----------------------------------------------------------
// Where the `claude` CLI is, or empty: $AI_CONSOLE_CLAUDE, then the one chosen
// in the settings, then the same search as python/claude_agent.py (PATH, the
// usual install folders, the copy bundled with the VS Code extension).
inline QString findClaude() {
    const QByteArray override = qgetenv("AI_CONSOLE_CLAUDE");
    if (!override.isEmpty())
        return QString::fromLocal8Bit(override);
    if (!settings::current().claude.isEmpty())
        return QFileInfo(settings::current().claude).isExecutable() ? settings::current().claude : QString();
    QStringList dirs = environment().value(QStringLiteral("PATH")).split(':');
    dirs << QDir::homePath() + QStringLiteral("/.claude/local");
    const QString found = QStandardPaths::findExecutable(QStringLiteral("claude"), dirs);
    if (!found.isEmpty())
        return found;
    const QDir ext(QDir::homePath() + QStringLiteral("/.vscode/extensions"));
    const QStringList bundled = ext.entryList({QStringLiteral("anthropic.claude-code-*")}, QDir::Dirs, QDir::Name);
    for (auto it = bundled.crbegin(); it != bundled.crend(); ++it) {
        const QString p = ext.filePath(*it + QStringLiteral("/resources/native-binary/claude"));
        if (QFileInfo(p).isExecutable())
            return p;
    }
    return {};
}

// ---- agent / scripts ------------------------------------------------------
inline Command agentChatCommand(const QString &configPath) {
    return {pythonProgram(),
            {pythonDir() + "/agent.py", QStringLiteral("chat"), QStringLiteral("--config"), configPath},
            pythonDir()};
}

// The same chat protocol, answered by Claude Code instead of the local model
// (python/claude_agent.py). The MCP servers still come from the session config.
inline Command claudeChatCommand(const QString &configPath, const LaunchConfig &c) {
    QStringList args{pythonDir() + "/claude_agent.py", QStringLiteral("chat"), QStringLiteral("--config"), configPath};
    const QString claude = findClaude();
    if (!claude.isEmpty())      // if empty, claude_agent.py reports that it is missing
        args << QStringLiteral("--claude") << claude;
    if (!c.claudeModel.isEmpty())
        args << QStringLiteral("--model") << c.claudeModel;
    return {pythonProgram(), args, pythonDir()};
}

// Run a user script. cwd is the script's own folder; agent.py is on PYTHONPATH.
inline Command scriptRunCommand(const QString &scriptPath) {
    return {pythonProgram(), {scriptPath}, QFileInfo(scriptPath).absolutePath()};
}

// Ask Claude Code to write a script (uses your Claude Code login, no API key).
inline Command generateScriptCommand(const LaunchConfig &c, const QString &configPath) {
    return {pythonProgram(),
            {pythonDir() + "/gen_script.py",
             QStringLiteral("--out"), c.scriptPath,
             QStringLiteral("--prompt"), c.scriptPrompt,
             QStringLiteral("--config"), configPath},
            pythonDir()};
}

} // namespace commands
