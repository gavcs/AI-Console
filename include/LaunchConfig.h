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
    QString           claudeModel;      // ClaudeCode: model or alias ("opus"); empty = Claude Code's default
    PromptMode        mode = PromptMode::Chat;
    QList<ToolChoice> tools;

    ScriptSource scriptSource = ScriptSource::ExistingFile;
    QString      scriptPath;      // ExistingFile: the script to run. Generate: where to write it.
    QString      scriptPrompt;    // Generate: what the script should do.

    QString shortModel() const {
        if (backend == Backend::ClaudeCode)
            return claudeModel.isEmpty() ? QStringLiteral("Claude Code")
                                         : QStringLiteral("Claude Code (%1)").arg(claudeModel);
        return model.section('/', -1);
    }

    QString summary() const {
        QStringList t;
        for (const ToolChoice &c : tools)
            t << QStringLiteral("%1 (%2)").arg(c.name, c.location);
        return t.isEmpty() ? shortModel()
                           : QStringLiteral("%1 · tools: %2").arg(shortModel(), t.join(", "));
    }
};
