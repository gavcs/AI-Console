#pragma once
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QSaveFile>
#include <QStandardPaths>
#include <QString>
#include <QStringList>

#define AI_CONSOLE_VERSION "0.1.0"

// ---------------------------------------------------------------------------
// What differs from one machine to the next: which model server is used and
// how it is started, and where a few programs are. Edited in the app
// (SettingsDialog) and kept in settings.json in the app's data folder, next to
// mcp_servers.json; nothing here is ever read from the project folder.
//
// A missing file, or a missing key, means the default, so the app works before
// anything has been set.
// ---------------------------------------------------------------------------

// A model server the Settings dialog can fill in for you. These are starting
// points: every field can be edited afterwards.
struct Provider {
    QString id;
    QString name;           // shown in the dropdown
    QString baseUrl;        // its OpenAI-compatible API
    QString startCommand;   // what "Start server" runs; {model}, {port} and {host} are filled in. Empty = never started from here
    QString model;          // a model id in the form this server expects (may be empty)
    QString note;           // one line shown under the dropdown
};

struct AppSettings {
    QString     provider;       // Provider::id the fields below were last filled in from
    QString     baseUrl;
    QString     startCommand;
    QStringList models;         // offered in the Model box on the start page (anything else can be typed)
    QString     apiKey;         // sent as a Bearer token, for a server that wants one (empty = none)
    QString     python;         // interpreter that runs python/*.py and scripts ("python3" = first on the PATH)
    QString     claude;         // the claude CLI (empty = searched for)
    QString     scriptsDir;     // where generated scripts go (empty = Documents/AI Console/scripts)
};

namespace settings {

inline const QList<Provider> &providers() {
    static const QList<Provider> list = {
        {"vllm-mlx", "vllm-mlx (Apple Silicon)", "http://127.0.0.1:8000/v1",
         "vllm-mlx serve {model} --enable-auto-tool-choice --tool-call-parser auto --port {port}",
         "mlx-community/Qwen3.5-9B-MLX-4bit",
         "Serves one model, downloaded from Hugging Face the first time it is started."},
        {"mlx-lm", "mlx-lm (Apple Silicon)", "http://127.0.0.1:8080/v1",
         "mlx_lm.server --model {model} --port {port}",
         "mlx-community/Qwen3.5-9B-MLX-4bit",
         "Serves one model, downloaded from Hugging Face the first time it is started."},
        {"ollama", "Ollama", "http://127.0.0.1:11434/v1",
         "ollama serve",
         "qwen3:8b",
         "Pull the model first (ollama pull <model>). If the Ollama app is already running, so is the server, "
         "and Start is not needed."},
        {"lmstudio", "LM Studio", "http://127.0.0.1:1234/v1",
         "lms server start --port {port}",
         "",
         "Load a model in LM Studio. The Model box on the start page lists what the running server offers."},
        {"llama.cpp", "llama.cpp (llama-server)", "http://127.0.0.1:8080/v1",
         "llama-server -hf {model} --jinja --port {port}",
         "ggml-org/gemma-3-4b-it-GGUF",
         "The model is a Hugging Face GGUF repository. --jinja is what makes tool calls work."},
        {"other", "Other OpenAI-compatible server", "http://127.0.0.1:8000/v1",
         "",
         "",
         "For a server that is started elsewhere (also on another machine): the app only connects to it. "
         "Give an API key below if it wants one."},
    };
    return list;
}

inline const Provider &provider(const QString &id) {
    for (const Provider &p : providers())
        if (p.id == id)
            return p;
    return providers().first();
}

inline AppSettings defaults() {
    const Provider &p = providers().first();
    AppSettings s;
    s.provider     = p.id;
    s.baseUrl      = p.baseUrl;
    s.startCommand = p.startCommand;
    s.models       = QStringList{p.model};
    s.python       = QStringLiteral("python3");
    return s;
}

inline QString path() {
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    return dir + "/settings.json";
}

inline AppSettings load() {
    AppSettings s = defaults();
    QFile f(path());
    if (!f.open(QIODevice::ReadOnly))
        return s;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    const auto text = [&o](const char *key, QString *into) {
        if (o.contains(QLatin1String(key)))
            *into = o[QLatin1String(key)].toString().trimmed();
    };
    text("provider", &s.provider);
    text("baseUrl", &s.baseUrl);
    text("startCommand", &s.startCommand);
    text("apiKey", &s.apiKey);
    text("python", &s.python);
    text("claude", &s.claude);
    text("scriptsDir", &s.scriptsDir);
    if (o.contains("models")) {
        s.models.clear();
        for (const QJsonValue &v : o["models"].toArray())
            if (!v.toString().trimmed().isEmpty())
                s.models << v.toString().trimmed();
    }
    if (s.baseUrl.isEmpty())
        s.baseUrl = defaults().baseUrl;
    if (s.python.isEmpty())
        s.python = defaults().python;
    return s;
}

// The settings in use. Loaded on first use; replaced by save().
inline AppSettings &current() {
    static AppSettings s = load();
    return s;
}

inline bool save(const AppSettings &s, QString *error = nullptr) {
    const QJsonObject o{{"provider", s.provider},
                        {"baseUrl", s.baseUrl},
                        {"startCommand", s.startCommand},
                        {"models", QJsonArray::fromStringList(s.models)},
                        {"apiKey", s.apiKey},
                        {"python", s.python},
                        {"claude", s.claude},
                        {"scriptsDir", s.scriptsDir}};
    QSaveFile f(path());
    if (!f.open(QIODevice::WriteOnly) || f.write(QJsonDocument(o).toJson(QJsonDocument::Indented)) < 0 || !f.commit()) {
        if (error)
            *error = f.errorString();
        return false;
    }
    current() = s;
    return true;
}

} // namespace settings
