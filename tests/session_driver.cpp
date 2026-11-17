// session_driver — a headless walk through the app, for tests.
//
// Drives the real MainWindow by clicking its widgets: start a chat with the
// local model, leave it, continue it with Claude Code, pick another Claude
// model and then the local model again from the chat page itself, change the
// effort level, stop a reply half-way, run a script, then "restart the app" (a
// second MainWindow) and reopen both from the session list. It also checks the
// settings (provider presets, how the server command is built) and that
// closing the window stops what the app started.
//
// Nothing real is used and nothing of the user's is touched:
//   AI_CONSOLE_SERVER_PORT   port of tests/fake_model.py (instead of the model server)
//   AI_CONSOLE_CLAUDE        tests/fake_claude.py (instead of the claude CLI)
//   AI_CONSOLE_HISTORY_DIR   where the sessions are saved (a temp folder)
//   AI_CONSOLE_PYTHON_DIR    the project's python/ folder
// and settings go to a separate "AI Console Test" organisation.
//
// Usage:  session_driver --script hello.py [--shots DIR]
// Exit code 0 if every step did what it should; each failure is printed.
// What the backends were sent is asserted on by run_tests.sh afterwards.

#include "ChatPage.h"
#include "Commands.h"
#include "EffortScale.h"
#include "History.h"
#include "LaunchPage.h"
#include "MainWindow.h"
#include "McpServers.h"
#include "ProcessPage.h"
#include "SettingsDialog.h"
#include "Style.h"

#include <QApplication>
#include <QComboBox>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QSlider>
#include <QStackedWidget>
#include <QTcpServer>
#include <QTextBrowser>
#include <QTimer>
#include <QToolButton>
#include <arpa/inet.h>
#include <cstdio>
#include <functional>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

static int failures = 0;
static QString shots;

static void check(bool ok, const char *what) {
    fprintf(stderr, "  %s: %s\n", ok ? "ok" : "FAILED", what);
    if (!ok)
        ++failures;
}

static bool waitFor(const std::function<bool()> &done, int ms = 20000) {
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (done())
            return true;
    }
    return false;
}

static void shot(QWidget &w, const char *name) {
    waitFor([] { return false; }, 250);          // let layout and rendering settle
    if (!shots.isEmpty())
        w.grab().save(shots + '/' + name + ".png");
}

template <class T> static T *byText(QWidget *root, const QString &prefix) {
    for (T *w : root->findChildren<T *>())
        if (w->text().startsWith(prefix))
            return w;
    return nullptr;
}

// The start page's main button. Found by isDefault(), never by text: other
// buttons on that page are also called "Start".
static QPushButton *startButton(LaunchPage *launch) {
    for (QPushButton *b : launch->findChildren<QPushButton *>())
        if (b->isDefault())
            return b;
    return nullptr;
}

// Type `text` into the chat and press Enter.
static bool type(ChatPage *chat, const QString &text) {
    auto *input = chat->findChild<QPlainTextEdit *>("chatInput");
    if (!waitFor([&] { return input->isEnabled(); }))
        return false;
    input->setPlainText(text);
    QKeyEvent press(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QCoreApplication::sendEvent(input, &press);
    return true;
}

// ...and wait for the reply to finish.
static bool say(ChatPage *chat, const QString &text) {
    auto *input = chat->findChild<QPlainTextEdit *>("chatInput");
    return type(chat, text) && waitFor([&] { return input->isEnabled() && !chat->isBusy(); });
}

static int speakers(ChatPage *chat) {
    return int(chat->findChildren<QLabel *>("speaker").size());
}

static int notes(ChatPage *chat, const QString &text) {
    int n = 0;
    for (QTextBrowser *b : chat->findChildren<QTextBrowser *>("note"))
        n += b->toPlainText() == text;
    return n;
}

// Choose an entry of the chat page's model picker, as a click on it would.
static bool pickModel(ChatPage *chat, const QString &backend, const QString &model) {
    auto *box = chat->findChild<QComboBox *>("chatModel");
    const int index = box ? box->findData(QStringList{backend, model}) : -1;
    if (index < 0 || !box->isEnabled())
        return false;
    box->setCurrentIndex(index);
    emit box->activated(index);
    return true;
}

static QString header(ChatPage *chat) {
    return chat->findChild<QLabel *>("pageHeader")->text();
}

static int freePort() {
    QTcpServer s;
    s.listen(QHostAddress::LocalHost, 0);
    return s.serverPort();
}

// Is something listening on `port`? Asked with a plain socket: it must not
// take the port itself, even for a moment, while a service is about to bind it.
static bool listening(int port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(uint16_t(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const bool up = ::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof addr) == 0;
    ::close(fd);
    return up;
}

// ---- closing the app: nothing it started is left behind -----------------------

static void checkShutdown() {
    fprintf(stderr, "closing the window\n");
    const QList<ServerConfig> before = commands::servers();
    const AppSettings saved = settings::current();
    const int servicePort = freePort(), keptPort = freePort(), workerPort = freePort();

    // A service the app is asked to start, and one that is already running.
    QString error;
    const QString json = QStringLiteral(
        R"({"started": {"command": "true", "service": {"port": %1, "start": "exec python3 -m http.server %1 --bind 127.0.0.1"}},
            "kept":    {"command": "true", "service": {"port": %2, "start": "false"}}})").arg(servicePort).arg(keptPort);
    const QList<ServerConfig> servers = mcp::parseServers(json.toUtf8(), &error);
    check(servers.size() == 2 && commands::setServers(servers, &error), "two servers with a service configured");
    if (!error.isEmpty())
        fprintf(stderr, "    %s\n", qPrintable(error));
    QProcess already;
    already.start("python3", {"-m", "http.server", QString::number(keptPort), "--bind", "127.0.0.1"});
    // A "model server" that is a shell with a worker of its own: stopping the shell must stop the worker.
    settings::current().startCommand =
        QStringLiteral("sh -c \"python3 -m http.server %1 --bind 127.0.0.1 & wait\"").arg(workerPort);
    check(waitFor([&] { return listening(keptPort); }, 8000), "a service is already running before the app starts");
    {
        MainWindow window;
        window.show();
        auto *launch = window.findChild<LaunchPage *>();
        emit launch->toolPageRequested("started", "local", false);
        emit launch->toolPageRequested("kept", "local", true);
        for (ProcessPage *page : window.findChildren<ProcessPage *>())      // the page of "kept" was only shown idle
            if (page->output().contains("already answers"))
                byText<QPushButton>(page, "Run again")->click();
        emit launch->serverPageRequested(launch->config(), false);
        check(waitFor([&] { return listening(servicePort); }, 15000), "the service is started from the app");
        check(waitFor([&] { return listening(workerPort); }, 15000), "the model server is started, and starts its worker");
        if (!listening(servicePort) || !listening(workerPort))
            for (ProcessPage *page : window.findChildren<ProcessPage *>())
                fprintf(stderr, "---- page output ----\n%s\n", qPrintable(page->output()));
        waitFor([] { return false; }, 1500);          // let the watcher of "kept" see that it is running
        QElapsedTimer t;
        t.start();
        window.close();
        // (a process that was just killed may take a moment to let go of its port)
        check(waitFor([&] { return !listening(servicePort); }, 2000),
              "closing the window stops a service that was started from the app");
        check(waitFor([&] { return !listening(workerPort); }, 2000),
              "...and the model server together with what it started");
        check(listening(keptPort), "...but not a service that was already running");
        check(t.elapsed() < 12000, "closing does not take long");
    }
    already.kill();
    already.waitForFinished(2000);
    settings::current() = saved;
    commands::setServers(before, &error);
}

// ---- settings: no window needed ---------------------------------------------

static void checkSettings() {
    fprintf(stderr, "settings\n");
    const AppSettings saved = settings::current();
    const AppSettings d = settings::defaults();
    check(d.provider == "vllm-mlx" && d.baseUrl == "http://127.0.0.1:8000/v1" && d.python == "python3" &&
              d.startCommand.startsWith("vllm-mlx serve {model}") && d.models == QStringList{"mlx-community/Qwen3.5-9B-MLX-4bit"},
          "with nothing saved, the settings are what the app always used");

    LaunchConfig c;
    c.model = "org/some model";                  // a space must not split the argument
    settings::current().startCommand = settings::provider("llama.cpp").startCommand;
    const Command llama = commands::serverCommand(c);
    check(llama.program == "llama-server" &&
              llama.args == QStringList({"-hf", "org/some model", "--jinja", "--port", QString::number(commands::serverPort())}),
          "start command: {model} and {port} filled in, one argument each");
    settings::current().startCommand.clear();
    check(!commands::serverCommand(c).isValid(), "no start command: nothing to start");
    settings::current().python = "/opt/venv/bin/python";
    check(commands::agentChatCommand("x.json").program == "/opt/venv/bin/python" &&
              commands::scriptRunCommand("/tmp/s.py").program == "/opt/venv/bin/python",
          "the chosen Python runs the agent and scripts");
    settings::current() = saved;

    // The dialog: choosing a provider fills in its address and command.
    SettingsDialog dialog(d);
    auto *provider = dialog.findChild<QComboBox *>();
    const int ollama = provider->findData("ollama");
    provider->setCurrentIndex(ollama);
    emit provider->activated(ollama);
    static_cast<QDialog &>(dialog).accept();
    const AppSettings picked = dialog.settings();
    check(picked.provider == "ollama" && picked.baseUrl == "http://127.0.0.1:11434/v1" &&
              picked.startCommand == "ollama serve" && picked.models == QStringList{"qwen3:8b"} && picked.python == "python3",
          "Settings dialog: choosing Ollama fills in its address, command and a model");
    check(provider->count() == settings::providers().size() && provider->count() >= 5, "several providers offered");
}

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("AI Console Test");
    QCoreApplication::setApplicationName("AI Console");
    style::apply(app);

    QString script;
    for (int i = 1; i + 1 < argc; ++i) {
        if (QString::fromLocal8Bit(argv[i]) == "--script") script = QString::fromLocal8Bit(argv[i + 1]);
        if (QString::fromLocal8Bit(argv[i]) == "--shots")  shots  = QString::fromLocal8Bit(argv[i + 1]);
    }
    // A question box nobody can click would hang the run: give up instead.
    QTimer::singleShot(170000, [] { fprintf(stderr, "session_driver: timed out\n"); _exit(3); });

    // Choices an earlier run left behind would change what this one starts with.
    for (const char *key : {"effort", "claudeModel"})
        QSettings().remove(key);

    checkSettings();
    checkShutdown();

    QString chatId;
    {
        MainWindow window;
        window.show();
        auto *launch = window.findChild<LaunchPage *>();
        auto *chat   = window.findChild<ChatPage *>();
        auto *pages  = window.findChild<QStackedWidget *>();
        auto *list   = window.findChild<QListWidget *>("sessions");
        shot(window, "1-start");

        fprintf(stderr, "new chat, local model\n");
        byText<QRadioButton>(launch, "Local model")->click();
        byText<QRadioButton>(launch, "Chat")->click();
        check(!byText<QPushButton>(launch, "Continue")->isVisibleTo(launch), "no Continue button before a chat is open");
        startButton(launch)->click();
        check(pages->currentWidget() == chat, "chat page shown");
        check(say(chat, "first question"), "first turn answered");
        check(list->count() == 1 && list->item(0)->text().startsWith("first question"), "chat listed under its first message");
        chatId = list->count() ? list->item(0)->data(Qt::UserRole).toString() : QString();
        shot(window, "2-chat-local");

        fprintf(stderr, "leave, switch to Claude Code, continue\n");
        byText<QPushButton>(chat, "← Back")->click();
        check(pages->currentWidget() == launch, "back on the start page");
        QPushButton *cont = byText<QPushButton>(launch, "Continue");
        check(cont->isVisibleTo(launch) && chat->isAgentRunning(), "Continue offered, agent still running");
        shot(window, "3-start-with-open-chat");
        cont->click();
        check(pages->currentWidget() == chat && speakers(chat) == 2, "Continue with nothing changed returns to the same chat");
        byText<QPushButton>(chat, "← Back")->click();
        byText<QRadioButton>(launch, "Claude Code")->click();
        cont->click();
        check(pages->currentWidget() == chat, "chat page shown again");
        check(speakers(chat) == 2, "earlier turn shown again after the restart");
        check(say(chat, "second question"), "second turn answered by Claude Code");
        check(byText<QToolButton>(chat, "Thought for") != nullptr, "Claude's reasoning shown as a Thinking block");
        check(list->count() == 1, "still one saved chat");
        shot(window, "4-chat-claude");

        fprintf(stderr, "another Claude model, from the chat page\n");
        auto *input = chat->findChild<QPlainTextEdit *>("chatInput");
        check(pickModel(chat, "claude", "claude-opus-5-5"), "Claude models are listed by name and number");
        check(waitFor([&] { return input->isEnabled(); }) && header(chat).contains("Claude Opus 5.5") &&
                  notes(chat, "Changed — now answering: Claude Opus 5.5") == 1 && speakers(chat) == 4,
              "the model changes in the running chat, with the conversation as it was");
        check(say(chat, "opus question"), "next turn answered by the new Claude model");
        check(launch->config().claudeModel == "claude-opus-5-5", "the start page shows the model the chat uses now");

        fprintf(stderr, "back to the local model, from the chat page\n");
        check(pickModel(chat, "local", "test-model"), "the local model is offered in the chat's model picker");
        check(waitFor([&] { return input->isEnabled(); }) && speakers(chat) == 6 && !header(chat).contains("Claude"),
              "the local model's agent takes over the conversation");
        check(say(chat, "third question"), "third turn answered by the local model");
        check(speakers(chat) == 8, "four questions and four answers on screen");

        fprintf(stderr, "effort\n");
        auto *scale = chat->findChild<EffortScale *>("chatEffort");
        check(scale && scale->level().isEmpty() && effort::levels(LaunchConfig::Backend::LocalModel).value(2) == "low",
              "the effort scale starts at Default");
        scale->findChild<QSlider *>()->setValue(2);
        check(waitFor([&] { return input->isEnabled() && notes(chat, "Changed — effort: low") == 1; }) &&
                  header(chat).contains("effort: low") && scale->level() == "low",
              "moving the scale changes the effort in the running chat");
        shot(window, "4b-model-and-effort");

        fprintf(stderr, "stop a reply half-way\n");
        check(type(chat, "a slow question"), "slow question sent");
        QPushButton *send = byText<QPushButton>(chat, "Stop");
        check(send != nullptr && chat->isBusy(), "Send turns into Stop while the reply is written");
        waitFor([] { return false; }, 700);       // let some of the reply arrive
        if (send)
            send->click();
        check(waitFor([&] { return !chat->isBusy(); }, 5000), "the reply stops");
        check(notes(chat, "[stopped]") == 1 && byText<QPushButton>(chat, "Send") != nullptr, "marked as stopped; Send is back");
        shot(window, "5-stopped");

        fprintf(stderr, "script run\n");
        byText<QPushButton>(chat, "← Back")->click();
        byText<QRadioButton>(launch, "Python script")->click();
        byText<QRadioButton>(launch, "Existing file")->click();
        for (QLineEdit *e : launch->findChildren<QLineEdit *>())
            if (e->placeholderText().endsWith("script.py"))
                e->setText(script);
        startButton(launch)->click();
        auto *run = qobject_cast<ProcessPage *>(pages->currentWidget());
        check(run != nullptr, "script page shown");
        check(run && waitFor([&] { return !run->isRunning(); }), "script finished");
        check(run && run->output().contains("hello from the script"), "script output on the page");
        check(list->count() == 2 && list->item(0)->text().contains("finished"), "script run listed as finished");
        check(chat->isAgentRunning(), "the chat is still open behind the script run");
        shot(window, "6-script");

        auto *filter = window.findChild<QLineEdit *>("sessionFilter");
        filter->setText("HELLO");
        check(list->count() == 1 && list->item(0)->text().startsWith("hello.py"), "filter narrows the session list");
        filter->clear();
        check(list->count() == 2, "clearing the filter shows everything again");
    }

    fprintf(stderr, "restart, reopen the chat\n");
    {
        MainWindow window;
        window.show();
        auto *chat  = window.findChild<ChatPage *>();
        auto *pages = window.findChild<QStackedWidget *>();
        auto *list  = window.findChild<QListWidget *>("sessions");
        check(list->count() == 2, "both sessions listed after a restart");
        window.openSession(chatId);
        check(pages->currentWidget() == chat && speakers(chat) == 10, "saved conversation shown again");
        check(chat->findChild<EffortScale *>("chatEffort")->level() == "low", "with the effort level it was left at");
        check(notes(chat, "[stopped]") == 1 && byText<QToolButton>(chat, "Thought") != nullptr,
              "with the stopped reply and the reasoning");
        check(say(chat, "fourth question"), "conversation continues after a restart");
        shot(window, "7-reopened");

        // the saved script run: its output is shown without running it again
        for (int i = 0; i < list->count(); ++i)
            if (list->item(i)->text().startsWith("hello.py"))
                window.openSession(list->item(i)->data(Qt::UserRole).toString());
        auto *run = qobject_cast<ProcessPage *>(pages->currentWidget());
        check(run && !run->isRunning() && run->output().contains("hello from the script"), "saved script output shown");
        shot(window, "8-saved-script");
    }

    printf("%s\n", qPrintable(chatId));
    fprintf(stderr, "session_driver: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
