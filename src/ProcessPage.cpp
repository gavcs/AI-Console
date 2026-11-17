#include "ProcessPage.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTextCursor>
#include <QVBoxLayout>

#include "ProcessUtil.h"

ProcessPage::ProcessPage(QWidget *parent) : QWidget(parent) {
    auto *back = new QPushButton("← Back");
    back->setFlat(true);
    connect(back, &QPushButton::clicked, this, [this] {
        if (m_stopOnBack)
            stop();
        emit backRequested();
    });

    m_header     = new QLabel;
    m_header->setObjectName("pageHeader");
    m_nextButton = new QPushButton;
    m_runButton  = new QPushButton("Run again");
    m_stopButton = new QPushButton("Stop");
    m_nextButton->hide();
    connect(m_nextButton, &QPushButton::clicked, this, &ProcessPage::nextActionRequested);
    connect(m_runButton,  &QPushButton::clicked, this, &ProcessPage::run);
    connect(m_stopButton, &QPushButton::clicked, this, &ProcessPage::stop);

    auto *headerRow = new QHBoxLayout;
    headerRow->addWidget(back);
    headerRow->addWidget(m_header, 1);
    headerRow->addWidget(m_nextButton);
    headerRow->addWidget(m_runButton);
    headerRow->addWidget(m_stopButton);

    m_output = new QPlainTextEdit;
    m_output->setReadOnly(true);
    m_output->setMaximumBlockCount(10000);   // keep memory bounded on chatty processes
    m_output->setObjectName("log");

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(headerRow);
    layout->addWidget(m_output, 1);

    // Interleave stdout and stderr in the order they arrive.
    m_process.setProcessChannelMode(QProcess::MergedChannels);
    proc::ownGroup(m_process);   // so that stop() reaches what the command starts

    connect(&m_process, &QProcess::readyRead, this, [this] {
        appendOutput(QString::fromLocal8Bit(m_process.readAll()));
    });
    connect(&m_process, &QProcess::finished, this,
            [this](int code, QProcess::ExitStatus status) {
                const bool normal = status == QProcess::NormalExit;
                m_lastExit = normal ? code : -1;
                appendOutput(normal ? QStringLiteral("\n[process exited with code %1]\n").arg(code)
                                    : QStringLiteral("\n[process killed]\n"));
                setRunning(false);
                emit finished(code, normal);
            });
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        appendOutput(QStringLiteral("\n[error] %1\n").arg(m_process.errorString()));
        if (!isRunning()) {   // e.g. FailedToStart: no finished() will follow
            m_lastExit = -1;
            setRunning(false);
        }
    });

    setRunning(false);
}

ProcessPage::~ProcessPage() {
    stop();   // QProcess warns (and kills) if destroyed while running; do it cleanly
}

void ProcessPage::setNextAction(const QString &label) {
    m_nextButton->setText(label);
    m_nextButton->setVisible(!label.isEmpty());
}

void ProcessPage::begin(const QString &header, const Command &cmd, const QProcessEnvironment &env) {
    stop();
    m_cmd = cmd;
    m_env = env;
    m_header->setText(header);
    m_output->clear();
    run();
}

void ProcessPage::showIdle(const QString &header, const Command &cmd, const QProcessEnvironment &env,
                           const QString &note) {
    stop();
    m_cmd = cmd;
    m_env = env;
    m_header->setText(header);
    m_output->clear();
    appendOutput(note + '\n');
    setRunning(false);
}

void ProcessPage::run() {
    if (isRunning())
        return;
    if (!m_cmd.isValid()) {
        appendOutput(QStringLiteral("[error] no command configured for this action (see Commands.h)\n"));
        return;
    }
    appendOutput(QStringLiteral("$ %1\n").arg(m_cmd.toString()));
    m_lastExit = -1;
    setRunning(true);
    m_process.setProcessEnvironment(m_env);
    m_process.setWorkingDirectory(m_cmd.cwd);
    m_process.start(commands::resolveProgram(m_cmd.program, m_env), m_cmd.args);
    emit started();
}

QString ProcessPage::output() const {
    return m_output->toPlainText();
}

void ProcessPage::stop() {
    proc::stop(m_process);                 // SIGTERM first so servers can shut down cleanly
}

void ProcessPage::requestServiceShutdown() {
    if (!isRunning())
        return;
    // The watcher reads its stdin (see "shutdown" in python/service_watch.sh);
    // for a remote service the line travels over the ssh connection.
    m_process.write("shutdown\n");
    m_process.closeWriteChannel();
}

void ProcessPage::appendOutput(const QString &text) {
    // insertPlainText (not appendPlainText) so partial lines stream without
    // getting a newline injected between chunks.
    m_output->moveCursor(QTextCursor::End);
    m_output->insertPlainText(text);
    m_output->ensureCursorVisible();
}

void ProcessPage::setRunning(bool running) {
    m_runButton->setEnabled(!running);
    m_stopButton->setEnabled(running);
    m_nextButton->setEnabled(!running && m_lastExit == 0);
}
