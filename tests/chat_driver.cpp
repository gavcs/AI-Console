// chat_driver — a headless driver for ChatPage, for tests.
//
// It builds the real ChatPage, starts the real python/agent.py against a
// scripted fake model server (tests/fake_model.py), attaches files, types a
// prompt and "presses Enter", then saves a screenshot. The fake server records
// the request the app produced, so a test can assert on it. No real model and
// no clicking — it drives the actual widgets under QT_QPA_PLATFORM=offscreen.
//
// Usage:
//   QT_QPA_PLATFORM=offscreen AI_CONSOLE_PYTHON_DIR=<proj>/python \
//     chat_driver --cfg cfg.json [--prompt TEXT] [--attach FILE ...] [--shot out.png]
//                 [--backend claude] [--replay messages.json]
//
// --backend claude starts python/claude_agent.py instead; AI_CONSOLE_CLAUDE
// chooses the `claude` it runs (tests/fake_claude.py, or unset for the real one).
// --replay shows a saved conversation first, as reopening it from the session
// list does (the agent itself continues from the "history" file in the cfg).
//
// AI_CONSOLE_PYTHON_DIR must point at the project's python/ dir, because the
// driver binary does not live in <project>/bin where pythonDir() would look.
//
// Exit code: 0 if the agent reached "ready" and the turn completed, 1 otherwise
// (the screenshot is still written). Assertions live in run_tests.sh.

#include "ChatPage.h"
#include "LaunchConfig.h"
#include "Commands.h"

#include "Style.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QPlainTextEdit>
#include <cstdio>

static void pump(int ms) {
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
}

// Pump the event loop until `input` is enabled (the page is idle and ready) or
// the timeout elapses. Returns true if it became enabled.
static bool waitEnabled(QWidget *input, int ms) {
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (input->isEnabled())
            return true;
    }
    return false;
}

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    style::apply(app);

    bool claude = false;   // --backend claude: drive python/claude_agent.py instead of agent.py
    QString replay;
    QString session, prompt = QStringLiteral("Please review the attached."), shot;
    QStringList attach;
    for (int i = 1; i < argc; ++i) {
        const QString a = QString::fromLocal8Bit(argv[i]);
        const QString next = (i + 1 < argc) ? QString::fromLocal8Bit(argv[i + 1]) : QString();
        if (a == "--cfg")      session = next, ++i;
        else if (a == "--prompt")  prompt = next, ++i;
        else if (a == "--shot")    shot = next, ++i;
        else if (a == "--attach")  attach << next, ++i;
        else if (a == "--backend") claude = (next == "claude"), ++i;
        else if (a == "--replay")  replay = next, ++i;
    }
    if (session.isEmpty()) {
        fprintf(stderr, "chat_driver: --cfg is required\n");
        return 1;
    }

    LaunchConfig lc;
    lc.model = QStringLiteral("test-model");
    ChatPage page;
    page.resize(720, 640);
    page.show();
    if (claude)
        lc.backend = LaunchConfig::Backend::ClaudeCode;
    page.begin(lc, claude ? commands::claudeChatCommand(session, lc) : commands::agentChatCommand(session),
               QProcessEnvironment::systemEnvironment(), replay);

    auto *input = page.findChild<QPlainTextEdit *>("chatInput");
    if (!input) {
        fprintf(stderr, "chat_driver: no chat input found in ChatPage\n");
        return 1;
    }

    const bool ready = waitEnabled(input, claude ? 60000 : 8000);   // agent connects and emits "ready"
    if (!ready)
        fprintf(stderr, "chat_driver: agent never became ready\n");

    if (!attach.isEmpty())
        page.addFiles(attach);
    pump(200);

    input->setPlainText(prompt);
    QKeyEvent press(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QCoreApplication::sendEvent(input, &press);

    const bool done = waitEnabled(input, claude ? 120000 : 10000);   // turn completes, input re-enabled
    pump(100);

    if (!shot.isEmpty())
        page.grab().save(shot);

    fprintf(stderr, "chat_driver: ready=%s done=%s cleared=%s\n",
            ready ? "yes" : "no", done ? "yes" : "no",
            input->toPlainText().isEmpty() ? "yes" : "no");
    return (ready && done) ? 0 : 1;
}
