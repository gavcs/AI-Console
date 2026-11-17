#pragma once
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include "Commands.h"

// ---------------------------------------------------------------------------
// The user's MCP servers.
//
// Nothing is built in: every server, including ones this project ships a
// preset for, lives in mcp_servers.json in the app's data folder and is added,
// edited and removed in the app (ServerDialog). The file uses the same
// "mcpServers" layout as other MCP clients, so a snippet from a server's README
// can be pasted in as is:
//
//   { "mcpServers": {
//       "files":  { "command": "python3", "args": ["${APP_PYTHON}/mcp_files.py"] },
//       "search": { "type": "http", "url": "https://example.com/mcp",
//                   "headers": { "Authorization": "Bearer ..." } } } }
//
// Two optional additions of this app, per server:
//
//   "service":   the server needs a background service running first (on this
//                Mac, or on another machine reached over ssh). Gives the row a
//                status light and a Start button.
//   "locations": the same tool can run in more than one place; the launch
//                screen then offers a dropdown. Each entry is a server entry
//                of its own (command/url, service) plus "id" and "label".
//
// Values may use ${HOME}, ${APP_PYTHON} (this project's python/ folder),
// ${SERVICE_PORT} and ${SERVICE_URL} (where the location's service is reached
// on this Mac).
// ---------------------------------------------------------------------------

struct ServiceConfig {
    bool    enabled = false;
    QString host;           // ssh destination the service runs on; empty = this Mac
    int     port = 0;       // port it listens on, on that machine (loopback)
    int     localPort = 0;  // remote only: port on this Mac forwarded to it (0 = same as port)
    QString start;          // shell command that starts it there (empty = never start it, only watch)
    QString healthPath;     // HTTP path used to check it is up (empty = "/")
    QString infoPath;       // HTTP path describing it; printed in the log (optional)
    QString logFile;        // its own log file there; new lines are copied to the log (optional)
    QString hint;           // shown if it has not come up after a minute (optional)

    int macPort() const { return (!host.isEmpty() && localPort > 0) ? localPort : port; }
};

struct LocationConfig {
    QString     id;                       // "local" / "remote" / ...: saved in settings
    QString     label;                    // dropdown text
    QString     transport = QStringLiteral("stdio");   // "stdio" | "http" | "sse"
    QString     command;                  // stdio
    QStringList args;
    QString     cwd;
    QMap<QString, QString> env;
    QString     url;                      // http / sse
    QMap<QString, QString> headers;
    ServiceConfig service;
};

struct ServerConfig {
    QString               name;
    QString               description;
    QList<LocationConfig> locations;      // at least one
};

namespace mcp {

inline QString configPath() {
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    return dir + QStringLiteral("/mcp_servers.json");
}

// Ready-made entries shipped with the project (presets/*.json), offered in the
// Add dialog. They are starting points to edit, not servers the app has.
inline QString presetsDir() { return QDir::cleanPath(commands::pythonDir() + QStringLiteral("/../presets")); }

inline bool validName(const QString &name) {
    static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9_-]{1,32}$"));
    return re.match(name).hasMatch();
}

// ---- JSON -> config --------------------------------------------------------
inline QMap<QString, QString> stringMap(const QJsonValue &v) {
    QMap<QString, QString> m;
    const QJsonObject o = v.toObject();
    for (auto it = o.begin(); it != o.end(); ++it)
        m.insert(it.key(), it.value().isString() ? it.value().toString() : it.value().toVariant().toString());
    return m;
}

inline QJsonObject stringMapJson(const QMap<QString, QString> &m) {
    QJsonObject o;
    for (auto it = m.begin(); it != m.end(); ++it)
        o.insert(it.key(), it.value());
    return o;
}

inline ServiceConfig serviceFromJson(const QJsonObject &o) {
    ServiceConfig s;
    s.enabled    = !o.isEmpty();
    s.host       = o["host"].toString();
    s.port       = o["port"].toInt();
    s.localPort  = o["localPort"].toInt();
    s.start      = o["start"].toString();
    s.healthPath = o["healthPath"].toString();
    s.infoPath   = o["infoPath"].toString();
    s.logFile    = o["logFile"].toString();
    s.hint       = o["hint"].toString();
    return s;
}

inline LocationConfig locationFromJson(const QJsonObject &o) {
    LocationConfig l;
    l.id    = o["id"].toString();
    l.label = o["label"].toString();
    // Other clients write "type" or "transport", and spell streamable HTTP several ways.
    QString t = (o.contains("type") ? o["type"] : o["transport"]).toString().toLower();
    if (t.isEmpty())
        t = o.contains("url") ? QStringLiteral("http") : QStringLiteral("stdio");
    if (t == "streamable-http" || t == "streamablehttp" || t == "streamable_http")
        t = QStringLiteral("http");
    l.transport = t;
    l.command   = o["command"].toString();
    for (const QJsonValue &a : o["args"].toArray())
        l.args << (a.isString() ? a.toString() : a.toVariant().toString());
    l.cwd     = o["cwd"].toString();
    l.env     = stringMap(o["env"]);
    l.url     = o["url"].toString();
    l.headers = stringMap(o["headers"]);
    l.service = serviceFromJson(o["service"].toObject());
    return l;
}

// Fill in id/label where the file left them out, and keep ids unique.
inline void normalize(ServerConfig &s) {
    QStringList used;
    for (LocationConfig &l : s.locations) {
        const bool remote = l.service.enabled && !l.service.host.isEmpty();
        if (l.id.isEmpty())
            l.id = remote ? QStringLiteral("remote") : QStringLiteral("local");
        const QString base = l.id;
        for (int n = 2; used.contains(l.id); ++n)
            l.id = base + QString::number(n);
        used << l.id;
        if (l.label.isEmpty())
            l.label = remote ? QStringLiteral("Remote · ") + l.service.host : QStringLiteral("Local");
    }
}

inline ServerConfig serverFromJson(const QString &name, const QJsonObject &o) {
    ServerConfig s;
    s.name        = name;
    s.description = o["description"].toString();
    if (o["locations"].isArray()) {
        for (const QJsonValue &v : o["locations"].toArray())
            s.locations << locationFromJson(v.toObject());
    }
    if (s.locations.isEmpty())
        s.locations << locationFromJson(o);
    normalize(s);
    return s;
}

// Why a location can't work as written; empty = fine.
inline QString problem(const LocationConfig &l) {
    if (l.transport == "stdio") {
        if (l.command.trimmed().isEmpty()) return QStringLiteral("needs a command");
    } else if (l.transport == "http" || l.transport == "sse") {
        if (l.url.trimmed().isEmpty()) return QStringLiteral("needs a URL");
    } else {
        return QStringLiteral("unknown type \"%1\" (use stdio, http or sse)").arg(l.transport);
    }
    if (l.service.enabled && (l.service.port < 1 || l.service.port > 65535))
        return QStringLiteral("its service needs a port between 1 and 65535");
    return {};
}

// Parse a servers file or a pasted snippet. Accepts {"mcpServers": {...}} and
// a bare {"name": {...}} map. Returns an empty list and sets *error on failure.
inline QList<ServerConfig> parseServers(const QByteArray &json, QString *error) {
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &pe);
    if (!doc.isObject()) {
        *error = pe.error != QJsonParseError::NoError
                     ? QStringLiteral("Not valid JSON: %1 (at character %2)").arg(pe.errorString()).arg(pe.offset)
                     : QStringLiteral("Expected a JSON object like {\"mcpServers\": {\"name\": {...}}}.");
        return {};
    }
    QJsonObject root = doc.object();
    if (root["mcpServers"].isObject())
        root = root["mcpServers"].toObject();
    else if (root.contains("command") || root.contains("url")) {
        *error = QStringLiteral("This is a single server without a name. Wrap it: {\"name\": { ... }}.");
        return {};
    }
    QList<ServerConfig> list;
    for (auto it = root.begin(); it != root.end(); ++it) {
        if (!it.value().isObject()) {
            *error = QStringLiteral("\"%1\" is not a server entry (expected an object).").arg(it.key());
            return {};
        }
        if (!validName(it.key())) {
            *error = QStringLiteral("\"%1\" can't be used as a server name: use letters, digits, - and _ (max 32).").arg(it.key());
            return {};
        }
        const ServerConfig s = serverFromJson(it.key(), it.value().toObject());
        for (const LocationConfig &l : s.locations) {
            const QString why = problem(l);
            if (!why.isEmpty()) {
                *error = QStringLiteral("\"%1\"%2 %3.").arg(s.name, s.locations.size() > 1 ? " (" + l.id + ")" : QString(), why);
                return {};
            }
        }
        list << s;
    }
    return list;
}

// ---- config -> JSON --------------------------------------------------------
inline QJsonObject locationToJson(const LocationConfig &l) {
    QJsonObject o;
    if (l.transport == "stdio") {
        o["command"] = l.command;
        if (!l.args.isEmpty()) o["args"] = QJsonArray::fromStringList(l.args);
        if (!l.cwd.isEmpty())  o["cwd"]  = l.cwd;
        if (!l.env.isEmpty())  o["env"]  = stringMapJson(l.env);
    } else {
        o["type"] = l.transport;
        o["url"]  = l.url;
        if (!l.headers.isEmpty()) o["headers"] = stringMapJson(l.headers);
    }
    if (l.service.enabled) {
        const ServiceConfig &s = l.service;
        QJsonObject so{{"port", s.port}};
        if (!s.host.isEmpty())       so["host"]       = s.host;
        if (s.localPort > 0)         so["localPort"]  = s.localPort;
        if (!s.start.isEmpty())      so["start"]      = s.start;
        if (!s.healthPath.isEmpty()) so["healthPath"] = s.healthPath;
        if (!s.infoPath.isEmpty())   so["infoPath"]   = s.infoPath;
        if (!s.logFile.isEmpty())    so["logFile"]    = s.logFile;
        if (!s.hint.isEmpty())       so["hint"]       = s.hint;
        o["service"] = so;
    }
    return o;
}

// A server with one plain location is written exactly as other MCP clients
// write it; "locations" only appears when there is more than one.
inline QJsonObject serverToJson(const ServerConfig &s) {
    QJsonObject o;
    if (s.locations.size() == 1) {
        o = locationToJson(s.locations.first());
        if (s.locations.first().label != QLatin1String("Local") || s.locations.first().service.enabled)
            o["label"] = s.locations.first().label;
    } else {
        QJsonArray locs;
        for (const LocationConfig &l : s.locations) {
            QJsonObject lo = locationToJson(l);
            lo["id"]    = l.id;
            lo["label"] = l.label;
            locs.append(lo);
        }
        o["locations"] = locs;
    }
    if (!s.description.isEmpty())
        o["description"] = s.description;
    return o;
}

inline QByteArray toJson(const QList<ServerConfig> &servers) {
    QJsonObject map;
    for (const ServerConfig &s : servers)
        map.insert(s.name, serverToJson(s));
    return QJsonDocument(QJsonObject{{"mcpServers", map}}).toJson(QJsonDocument::Indented);
}

// ---- file ------------------------------------------------------------------
inline QList<ServerConfig> load(QString *error = nullptr) {
    QFile f(configPath());
    if (!f.exists())
        return {};                      // first run: no servers yet
    QString err;
    QList<ServerConfig> list;
    if (!f.open(QIODevice::ReadOnly))
        err = f.errorString();
    else
        list = parseServers(f.readAll(), &err);
    if (error)
        *error = err;
    return list;
}

inline bool save(const QList<ServerConfig> &servers, QString *error) {
    QSaveFile f(configPath());          // write-then-rename: a failed save keeps the old file
    if (!f.open(QIODevice::WriteOnly) || f.write(toJson(servers)) < 0 || !f.commit()) {
        *error = f.errorString();
        return false;
    }
    return true;
}

inline QList<ServerConfig> presets() {
    QList<ServerConfig> list;
    const QDir dir(presetsDir());
    for (const QString &name : dir.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name)) {
        QFile f(dir.filePath(name));
        QString err;
        if (f.open(QIODevice::ReadOnly))
            list << parseServers(f.readAll(), &err);
    }
    return list;
}

// ---- config -> what the app runs -------------------------------------------
inline QString expand(QString s, const LocationConfig &l) {
    if (s.startsWith(QLatin1String("~/")))
        s = QDir::homePath() + s.mid(1);
    s.replace(QLatin1String("${HOME}"), QDir::homePath());
    s.replace(QLatin1String("${APP_PYTHON}"), commands::pythonDir());
    if (l.service.enabled) {
        s.replace(QLatin1String("${SERVICE_PORT}"), QString::number(l.service.macPort()));
        s.replace(QLatin1String("${SERVICE_URL}"), QStringLiteral("http://127.0.0.1:%1").arg(l.service.macPort()));
    }
    return s;
}

inline McpTool toTool(const ServerConfig &s) {
    McpTool tool;
    tool.name        = s.name;
    tool.description = s.description;
    for (const LocationConfig &l : s.locations) {
        ToolLocation loc;
        loc.id    = l.id;
        loc.label = l.label;

        QJsonObject entry{{"name", s.name}, {"transport", l.transport}};
        if (l.transport == "stdio") {
            QJsonArray args;
            for (const QString &a : l.args)
                args.append(expand(a, l));
            QMap<QString, QString> env;
            for (auto it = l.env.begin(); it != l.env.end(); ++it)
                env.insert(it.key(), expand(it.value(), l));
            entry["command"] = expand(l.command, l);
            entry["args"]    = args;
            entry["env"]     = stringMapJson(env);
            if (!l.cwd.isEmpty())
                entry["cwd"] = expand(l.cwd, l);
        } else {
            QMap<QString, QString> headers;
            for (auto it = l.headers.begin(); it != l.headers.end(); ++it)
                headers.insert(it.key(), expand(it.value(), l));
            entry["url"]     = expand(l.url, l);
            entry["headers"] = stringMapJson(headers);
        }
        loc.mcpServer = entry;

        if (l.service.enabled) {
            const ServiceConfig &sv = l.service;
            QString path = sv.healthPath.isEmpty() ? QStringLiteral("/") : sv.healthPath;
            if (!path.startsWith('/'))
                path.prepend('/');
            loc.healthUrl = QStringLiteral("http://127.0.0.1:%1%2").arg(sv.macPort()).arg(path);
            commands::ServiceSpec spec;
            spec.name     = s.name;
            spec.port     = sv.port;
            spec.startCmd = sv.start;
            spec.infoPath = sv.infoPath;
            spec.logFile  = sv.logFile;
            spec.hint     = sv.hint;
            if (sv.host.isEmpty()) {
                loc.start = commands::localServiceCommand(spec);
            } else {
                spec.readyNote = QStringLiteral("reachable from this Mac at 127.0.0.1:%1 for as long as this stays running")
                                     .arg(sv.macPort());
                loc.start = commands::remoteServiceCommand(sv.host, sv.macPort(), spec);
            }
        }
        tool.locations << loc;
    }
    return tool;
}

} // namespace mcp

namespace commands {

// ---- the current server list ----------------------------------------------
struct ServerState {
    QList<ServerConfig> servers;    // as in mcp_servers.json
    QList<McpTool>      tools;      // the same, turned into what the app runs
    QString             loadError;  // why the file could not be read, if it couldn't
    bool                loaded = false;
};

inline ServerState &serverState() {
    static ServerState st;
    if (!st.loaded) {
        st.loaded  = true;
        st.servers = mcp::load(&st.loadError);
        for (const ServerConfig &s : st.servers)
            st.tools << mcp::toTool(s);
    }
    return st;
}

inline const QList<ServerConfig> &servers() { return serverState().servers; }
inline const QList<McpTool> &allTools()     { return serverState().tools; }

// Replace the server list and write it to disk. Pointers from findTool() are
// invalid afterwards.
inline bool setServers(const QList<ServerConfig> &list, QString *error) {
    if (!mcp::save(list, error))
        return false;
    ServerState &st = serverState();
    st.servers = list;
    st.tools.clear();
    st.loadError.clear();
    for (const ServerConfig &s : list)
        st.tools << mcp::toTool(s);
    return true;
}

inline const McpTool *findTool(const QString &name) {
    for (const McpTool &t : allTools())
        if (t.name == name)
            return &t;
    return nullptr;
}

// ---- session config for agent.py -----------------------------------------
// Writes the JSON agent.py reads and returns its path. `historyPath` (chats
// only) is where the backend keeps the conversation: see History.h.
inline QString writeSessionConfig(const LaunchConfig &c, const QString &historyPath = QString()) {
    QJsonArray servers;
    for (const ToolChoice &choice : c.tools)
        if (const McpTool *t = findTool(choice.name))
            if (const ToolLocation *l = t->location(choice.location))
                servers.append(l->mcpServer);
    QJsonObject cfg{{"base_url", serverBaseUrl()},
                    {"model", c.model},
                    {"label", c.shortModel()},
                    {"mcp_servers", servers}};
    if (!historyPath.isEmpty())
        cfg["history"] = historyPath;
    if (!settings::current().apiKey.isEmpty())
        cfg["api_key"] = settings::current().apiKey;
    const QString path = sessionConfigPath(c.mode == LaunchConfig::PromptMode::Chat);
    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(QJsonDocument(cfg).toJson(QJsonDocument::Indented));
    return path;
}

} // namespace commands
