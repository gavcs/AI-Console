#pragma once
#include <QProcess>

#ifdef Q_OS_UNIX
#include <csignal>
#include <unistd.h>
#endif

// ---------------------------------------------------------------------------
// Stopping what the app started, completely.
//
// A program the app runs often starts programs of its own: a model server its
// workers, the agent its MCP servers, a shell script its helpers. Signalling
// only the program itself leaves those behind. So every child is started as
// the leader of a session (and process group) of its own, and stopping it
// signals the whole group.
//
// A child that deliberately moves itself out of the group is not reached; that
// is how python/service_watch.sh keeps a service alive when only its watcher
// is stopped.
// ---------------------------------------------------------------------------
namespace proc {

// Call before start(): the program becomes the leader of its own process group.
inline void ownGroup(QProcess &p) {
#ifdef Q_OS_UNIX
    p.setChildProcessModifier([] { ::setsid(); });
#else
    Q_UNUSED(p);
#endif
}

#ifdef Q_OS_UNIX
inline void signalGroup(qint64 pid, int sig) {
    if (pid > 0 && ::kill(-pid_t(pid), sig) != 0)
        ::kill(pid_t(pid), sig);                // no such group: it was started without ownGroup()
}
#endif

// What is left of the group once its leader has exited.
inline void sweepGroup(qint64 pid) {
#ifdef Q_OS_UNIX
    if (pid > 0)
        ::kill(-pid_t(pid), SIGKILL);
#else
    Q_UNUSED(pid);
#endif
}

// Ask the program and everything it started to end (SIGTERM), wait `termMs`,
// then force what is left. Returns once the program has exited.
inline void stop(QProcess &p, int termMs = 3000, int killMs = 1000) {
    if (p.state() == QProcess::NotRunning)
        return;
    const qint64 pid = p.processId();
#ifdef Q_OS_UNIX
    signalGroup(pid, SIGTERM);
    if (!p.waitForFinished(termMs)) {
        signalGroup(pid, SIGKILL);
        p.waitForFinished(killMs);
    }
#else
    p.terminate();
    if (!p.waitForFinished(termMs)) {
        p.kill();
        p.waitForFinished(killMs);
    }
#endif
    sweepGroup(pid);
}

} // namespace proc
