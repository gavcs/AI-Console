#pragma once
#include <QList>
#include <QString>
#include <QStringList>

// One MCP tool the user ticked on the launch screen, and which of its
// locations (ToolLocation::id in Commands.h, e.g. "local" / "remote") to use.
// A local model always runs on this Mac.
struct ToolChoice {
    QString name;
    QString location;
};

// ---------------------------------------------------------------------------
// The Claude models offered by name. `id` is what the claude CLI is given, so
// the number shown is the model that answers (an alias such as "opus" would
// move to the next release on its own). Anything else can still be typed.
// ---------------------------------------------------------------------------
struct ClaudeModel {
    const char *id;
    const char *name;
};

namespace claude {

inline const QList<ClaudeModel> &models() {
    static const QList<ClaudeModel> list = {
        {"claude-fable-5-2", "Fable 5.2"},
        {"claude-opus-5-5", "Opus 5.5"},
        {"claude-sonnet-5-5", "Sonnet 5.5"},
        {"claude-haiku-5-5", "Haiku 5.5"},
    };
    return list;
}

// "Opus 5.5" for "claude-opus-5-5"; empty for a model that is not in the list.
inline QString name(const QString &id) {
    for (const ClaudeModel &m : models())
        if (id == QLatin1String(m.id))
            return QString::fromLatin1(m.name);
    return {};
}

// The id behind a name or a bare alias ("Opus 5.5", "opus"); otherwise `text` as it is.
inline QString idFor(const QString &text) {
    const QString t = text.trimmed();
    for (const ClaudeModel &m : models()) {
        const QString full = QString::fromLatin1(m.name);
        if (t.compare(full, Qt::CaseInsensitive) == 0 || t.compare(full.section(' ', 0, 0), Qt::CaseInsensitive) == 0)
            return QString::fromLatin1(m.id);
    }
    return t;
}

} // namespace claude

// What the user picked on the launch screen. Handed to whichever session
// page (chat or script) gets shown next.
struct LaunchConfig {
    enum class Backend      { LocalModel, ClaudeCode };
    enum class PromptMode   { Chat, Script };
    enum class ScriptSource { ExistingFile, Generate };

    // Who answers in a chat: the local model server, or Claude Code (the
    // `claude` CLI with the user's own login). Scripts always use the local model.
    Backend           backend = Backend::LocalModel;
    QString           model;            // LocalModel: the model id the server serves
    QString           claudeModel;      // ClaudeCode: model id or alias; empty = Claude Code's default
    QString           effort;           // how hard the model thinks (see `effort` below); empty = its default
    PromptMode        mode = PromptMode::Chat;
    QList<ToolChoice> tools;

    ScriptSource scriptSource = ScriptSource::ExistingFile;
    QString      scriptPath;      // ExistingFile: the script to run. Generate: where to write it.
    QString      scriptPrompt;    // Generate: what the script should do.

    QString shortModel() const {
        if (backend == Backend::ClaudeCode) {
            if (claudeModel.isEmpty())
                return QStringLiteral("Claude Code");
            const QString known = claude::name(claudeModel);
            return known.isEmpty() ? QStringLiteral("Claude Code (%1)").arg(claudeModel)
                                   : QStringLiteral("Claude %1").arg(known);
        }
        return model.section('/', -1);
    }

    QString summary() const;
};

// ---------------------------------------------------------------------------
// Effort: how much the model thinks before it answers. One scale, of which
// each backend takes the part it understands:
//
//   Claude Code   low · medium · high · xhigh · max   (the CLI's --effort)
//   local model   off · low · medium · high           (thinking switched off, or
//                 asked for at that level; see "Effort" in python/agent.py)
//
// The empty level means "say nothing": the model's or the server's own default.
// ---------------------------------------------------------------------------
namespace effort {

inline QStringList levels(LaunchConfig::Backend backend) {
    return backend == LaunchConfig::Backend::ClaudeCode
               ? QStringList{QString(), "low", "medium", "high", "xhigh", "max"}
               : QStringList{QString(), "off", "low", "medium", "high"};
}

inline QString label(const QString &level) {
    if (level.isEmpty())   return QStringLiteral("Default");
    if (level == "off")    return QStringLiteral("Off");
    if (level == "low")    return QStringLiteral("Low");
    if (level == "medium") return QStringLiteral("Medium");
    if (level == "high")   return QStringLiteral("High");
    if (level == "xhigh")  return QStringLiteral("Extra high");
    if (level == "max")    return QStringLiteral("Max");
    return level;
}

// The nearest level `backend` has: Claude always thinks, so "off" becomes
// "low"; a local model has nothing above "high".
inline QString clamp(const QString &level, LaunchConfig::Backend backend) {
    if (levels(backend).contains(level))
        return level;
    if (level == "off")
        return QStringLiteral("low");
    if (level == "xhigh" || level == "max")
        return QStringLiteral("high");
    return {};
}

} // namespace effort

inline QString LaunchConfig::summary() const {
    QString s = shortModel();
    if (!effort.isEmpty())
        s += QStringLiteral(" · effort: %1").arg(effort::label(effort).toLower());
    QStringList t;
    for (const ToolChoice &c : tools)
        t << QStringLiteral("%1 (%2)").arg(c.name, c.location);
    return t.isEmpty() ? s : QStringLiteral("%1 · tools: %2").arg(s, t.join(", "));
}
