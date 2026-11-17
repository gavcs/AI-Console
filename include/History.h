#pragma once
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QStandardPaths>
#include <QString>
#include <QUuid>
#include <algorithm>
#include "LaunchConfig.h"

// ---------------------------------------------------------------------------
// Saved sessions: every chat and every script run gets a folder of its own
// under <app data>/history/<id>/ :
//
//   meta.json      what it is: kind, title, dates and the settings it was
//                  last run with (model, MCP servers). Written by the app.
//   messages.json  chat only: the conversation, written by the backend that
//                  is answering (python/history.py describes the format).
//   claude.json    chat only: which Claude Code session holds it, if any.
//   output.log     script run only: what the script printed.
//
// Keeping the settings apart from the messages is what lets a conversation be
// continued with another model or other tools: the app rewrites meta.json and
// starts a backend on the same messages.json.
// ---------------------------------------------------------------------------

struct SessionRecord {
    QString      id;
    bool         chat = true;        // false: a script run
    QString      title;
    QDateTime    created;
    QDateTime    updated;
    LaunchConfig config;             // settings it was last run with
    bool         finished = false;   // script run: it ended (exitCode says how)
    int          exitCode = -1;      // script run: -1 = killed / did not start

    bool    isValid() const { return !id.isEmpty(); }
    QString displayTitle() const {
        return !title.isEmpty() ? title : chat ? QStringLiteral("New chat") : QStringLiteral("Script run");
    }
};

namespace history {

inline QString rootDir() {
    const QByteArray override = qgetenv("AI_CONSOLE_HISTORY_DIR");   // for tests
    const QString dir = !override.isEmpty()
                            ? QString::fromLocal8Bit(override)
                            : QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/history";
    QDir().mkpath(dir);
    return dir;
}

inline QString dir(const SessionRecord &r)          { return rootDir() + '/' + r.id; }
inline QString messagesPath(const SessionRecord &r) { return dir(r) + "/messages.json"; }
inline QString outputPath(const SessionRecord &r)   { return dir(r) + "/output.log"; }

inline QJsonObject configToJson(const LaunchConfig &c) {
    QJsonArray tools;
    for (const ToolChoice &t : c.tools)
        tools.append(QJsonObject{{"name", t.name}, {"location", t.location}});
    return {{"backend", c.backend == LaunchConfig::Backend::ClaudeCode ? "claude" : "local"},
            {"model", c.model},
            {"claudeModel", c.claudeModel},
            {"tools", tools},
            {"scriptPath", c.scriptPath}};
}

inline LaunchConfig configFromJson(const QJsonObject &o, bool chat) {
    LaunchConfig c;
    c.backend     = o["backend"].toString() == "claude" ? LaunchConfig::Backend::ClaudeCode
                                                        : LaunchConfig::Backend::LocalModel;
    c.model       = o["model"].toString();
    c.claudeModel = o["claudeModel"].toString();
    c.scriptPath  = o["scriptPath"].toString();
    c.mode        = chat ? LaunchConfig::PromptMode::Chat : LaunchConfig::PromptMode::Script;
    for (const QJsonValue &v : o["tools"].toArray())
        c.tools.append({v.toObject()["name"].toString(), v.toObject()["location"].toString()});
    return c;
}

// Same model and tools? (What decides whether a running chat can be kept.)
inline bool sameSettings(const LaunchConfig &a, const LaunchConfig &b) {
    QJsonObject x = configToJson(a), y = configToJson(b);
    x.remove("scriptPath");
    y.remove("scriptPath");
    return x == y;
}

inline bool save(const SessionRecord &r) {
    QDir().mkpath(dir(r));
    const QJsonObject o{{"id", r.id},
                        {"kind", r.chat ? "chat" : "script"},
                        {"title", r.title},
                        {"created", r.created.toString(Qt::ISODateWithMs)},
                        {"updated", r.updated.toString(Qt::ISODateWithMs)},
                        {"config", configToJson(r.config)},
                        {"finished", r.finished},
                        {"exitCode", r.exitCode}};
    QFile f(dir(r) + "/meta.json");
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
    return true;
}

// A new, saved record.
inline SessionRecord create(bool chat, const LaunchConfig &config, const QString &title = {}) {
    SessionRecord r;
    r.id      = QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss-") +
                QUuid::createUuid().toString(QUuid::WithoutBraces).left(6);
    r.chat    = chat;
    r.title   = title;
    r.created = r.updated = QDateTime::currentDateTime();
    r.config  = config;
    save(r);
    return r;
}

// Invalid record if there is no such session.
inline SessionRecord load(const QString &id) {
    QFile f(rootDir() + '/' + id + "/meta.json");
    if (id.isEmpty() || !f.open(QIODevice::ReadOnly))
        return {};
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    SessionRecord r;
    r.id       = id;
    r.chat     = o["kind"].toString() != "script";
    r.title    = o["title"].toString();
    r.created  = QDateTime::fromString(o["created"].toString(), Qt::ISODateWithMs);
    r.updated  = QDateTime::fromString(o["updated"].toString(), Qt::ISODateWithMs);
    r.config   = configFromJson(o["config"].toObject(), r.chat);
    r.finished = o["finished"].toBool();
    r.exitCode = o["exitCode"].toInt(-1);
    return r;
}

// Every saved session, most recently used first.
inline QList<SessionRecord> list() {
    QList<SessionRecord> out;
    for (const QString &id : QDir(rootDir()).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const SessionRecord r = load(id);
        if (r.isValid())
            out.append(r);
    }
    std::sort(out.begin(), out.end(),
              [](const SessionRecord &a, const SessionRecord &b) { return a.updated > b.updated; });
    return out;
}

inline void remove(const SessionRecord &r) {
    if (r.isValid())
        QDir(dir(r)).removeRecursively();
}

} // namespace history
