#include <QApplication>
#include <QSocketNotifier>
#include "MainWindow.h"
#include "Settings.h"
#include "Style.h"

#ifdef Q_OS_UNIX
#include <csignal>
#include <initializer_list>
#include <unistd.h>

// Ctrl-C in the terminal the app was started from, `kill`, or the terminal
// closing: quit the ordinary way, so that what the app started is stopped
// (MainWindow::shutdown) instead of being left behind. A signal handler may
// only do very little, so it writes a byte to a pipe the event loop watches.
static int quitPipe[2] = {-1, -1};

static void onQuitSignal(int) {
    const char byte = 1;
    if (::write(quitPipe[1], &byte, 1) < 0) {
        // nothing to be done about it here
    }
}

static void quitOnSignals(QApplication &app) {
    if (::pipe(quitPipe) != 0)
        return;
    auto *watch = new QSocketNotifier(quitPipe[0], QSocketNotifier::Read, &app);
    QObject::connect(watch, &QSocketNotifier::activated, &app, [] {
        char byte;
        if (::read(quitPipe[0], &byte, 1) > 0)
            QCoreApplication::quit();
    });
    for (int sig : {SIGINT, SIGTERM, SIGHUP})
        std::signal(sig, onQuitSignal);
}
#else
static void quitOnSignals(QApplication &) {}
#endif

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("AI Console");   // QSettings location
    QCoreApplication::setApplicationName("AI Console");
    QCoreApplication::setApplicationVersion(AI_CONSOLE_VERSION);

    style::apply(app);
    quitOnSignals(app);

    MainWindow window;
    window.show();

    return app.exec();
}
