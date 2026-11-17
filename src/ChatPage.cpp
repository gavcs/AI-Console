#include "ChatPage.h"

#include <QAbstractTextDocumentLayout>
#include <QBuffer>
#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QMimeDatabase>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStandardItemModel>
#include <QStringConverter>
#include <QTextBrowser>
#include <QTextDocument>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <cmath>
#include <functional>

#include "EffortScale.h"
#include "ProcessUtil.h"

namespace {
QString compactJson(const QJsonValue &v, int maxChars) {
    QString s = v.isString() ? v.toString()
                             : QString::fromUtf8(QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact));
    if (s.size() > maxChars)
        s = s.left(maxChars) + "…";
    return s;
}

// Is this file's content UTF-8 text (so it can be inlined for the model)?
bool looksTextual(const QByteArray &bytes, const QString &mime) {
    if (mime.startsWith(QLatin1String("text/")))
        return true;
    if (bytes.left(8192).contains('\0'))            // a NUL byte means binary
        return false;
    QStringDecoder dec(QStringConverter::Utf8);
    const QString s = dec.decode(bytes);
    (void)s;
    return !dec.hasError();
}

// Describe a file for the model (see the attachment shapes in agent.py):
// an image becomes a data: URL, a textual file is inlined (capped), and
// anything else is referenced by its path. Returns {} and sets *err on failure.
QJsonObject fileAttachment(const QString &path, QString *err) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        *err = f.errorString();
        return {};
    }
    const QByteArray bytes = f.readAll();
    const QFileInfo fi(path);
    const QString mime = QMimeDatabase().mimeTypeForFileNameAndData(path, bytes).name();
    QJsonObject o{{"name", fi.fileName()}, {"mime", mime}, {"path", fi.absoluteFilePath()}};
    if (mime.startsWith(QLatin1String("image/"))) {
        o["kind"] = "image";
        // Vision models bill by pixels, so shrink anything bigger than the long
        // edge most models downsample to anyway. Re-encode only when we scale;
        // an image already within the cap is sent byte-for-byte.
        constexpr int kMaxImageEdge = 1568;
        QByteArray outBytes = bytes;
        QString    outMime  = mime;
        QImage img;
        if (img.loadFromData(bytes) && qMax(img.width(), img.height()) > kMaxImageEdge) {
            const QImage small = (img.width() >= img.height())
                                     ? img.scaledToWidth(kMaxImageEdge, Qt::SmoothTransformation)
                                     : img.scaledToHeight(kMaxImageEdge, Qt::SmoothTransformation);
            const bool alpha = small.hasAlphaChannel();
            QByteArray enc;
            QBuffer buf(&enc);
            buf.open(QIODevice::WriteOnly);
            if (small.save(&buf, alpha ? "PNG" : "JPEG", alpha ? -1 : 85)) {
                outBytes = enc;
                outMime  = alpha ? QStringLiteral("image/png") : QStringLiteral("image/jpeg");
                o["mime"] = outMime;
                o["downscaled"] = QStringLiteral("%1x%2 → %3x%4")
                                      .arg(img.width()).arg(img.height())
                                      .arg(small.width()).arg(small.height());
            }
        }
        o["data_url"] = QStringLiteral("data:%1;base64,%2").arg(outMime, QString::fromLatin1(outBytes.toBase64()));
    } else if (looksTextual(bytes, mime)) {
        constexpr int kMaxChars = 100000;           // keep a big file from swamping the context
        QString text = QString::fromUtf8(bytes);
        o["kind"] = "text";
        if (text.size() > kMaxChars) {
            text.truncate(kMaxChars);
            o["truncated"] = true;
        }
        o["text"] = text;
    } else {
        o["kind"] = "binary";
        o["size"] = bytes.size();
    }
    return o;
}
} // namespace

// A piece of text in the transcript: as tall as its content (the page scrolls,
// not the block), selectable, and able to show Markdown.
class ChatMessageView : public QTextBrowser {
public:
    ChatMessageView() {
        setFrameShape(QFrame::NoFrame);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);   // wide code blocks / tables
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setOpenExternalLinks(true);
        setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        viewport()->setAutoFillBackground(false);              // sit on the page's background
        document()->setDocumentMargin(2);
        connect(document()->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged, this,
                [this] { fitHeight(); });
    }

    void showMarkdown(const QString &markdown) {
        // NoHTML: text such as <stdio.h> or <tool_call> is shown, not swallowed as a tag.
        document()->setMarkdown(markdown, QTextDocument::MarkdownFeatures(QTextDocument::MarkdownDialectGitHub |
                                                                         QTextDocument::MarkdownNoHTML));
        fitHeight();
    }

    void showPlain(const QString &text) {
        setPlainText(text);
        fitHeight();
    }

    void dim() {   // secondary text: reasoning, tool activity
        QPalette p = palette();
        p.setColor(QPalette::Text, p.color(QPalette::PlaceholderText));
        setPalette(p);
    }

protected:
    void resizeEvent(QResizeEvent *e) override {
        QTextBrowser::resizeEvent(e);
        fitHeight();
    }

    // Vertical scrolling belongs to the page; only sideways scrolling of an
    // over-wide block is handled here.
    void wheelEvent(QWheelEvent *e) override {
        const QPoint d = e->angleDelta();
        if (std::abs(d.x()) > std::abs(d.y()) && horizontalScrollBar()->maximum() > 0)
            QTextBrowser::wheelEvent(e);
        else
            e->ignore();
    }

private:
    void fitHeight() {
        int h = int(std::ceil(document()->size().height())) + 2 * frameWidth();
        if (horizontalScrollBar()->maximum() > 0)
            h += horizontalScrollBar()->sizeHint().height();
        // Compare with the height we asked for, not height(): the layout may
        // not have applied the previous request yet.
        if (h != minimumHeight() || h != maximumHeight())
            setFixedHeight(h);
    }
};

// The model's reasoning: one line ("Thinking." -> ".." -> "..." while it is
// still coming in, then "Thought for N s") that opens to show the text.
class ChatThinkingBlock : public QWidget {
public:
    ChatThinkingBlock() {
        m_toggle = new QToolButton;
        m_toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        m_toggle->setArrowType(Qt::RightArrow);
        m_toggle->setCheckable(true);
        m_toggle->setAutoRaise(true);
        m_body = new ChatMessageView;
        m_body->dim();
        m_body->hide();

        auto *bodyRow = new QHBoxLayout;
        bodyRow->setContentsMargins(18, 0, 0, 0);
        bodyRow->addWidget(m_body);
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        layout->addWidget(m_toggle, 0, Qt::AlignLeft);
        layout->addLayout(bodyRow);

        connect(m_toggle, &QToolButton::toggled, this, [this](bool open) {
            m_toggle->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
            m_body->setVisible(open);
            if (open)
                m_body->showPlain(m_text.trimmed());
        });
        m_dots.setInterval(450);
        connect(&m_dots, &QTimer::timeout, this, [this] { tick(); });
        m_clock.start();
        m_dots.start();
        tick();
    }

    void append(const QString &text) {
        m_text += text;
        m_dirty = true;
    }

    void render() {
        if (m_dirty && m_toggle->isChecked())
            m_body->showPlain(m_text.trimmed());
        m_dirty = false;
    }

    // Reasoning is over. `elapsedMs` overrides the block's own clock (used when
    // the text was first shown as answer and reclassified).
    void finish(qint64 elapsedMs = -1) {
        if (!m_dots.isActive())
            return;
        m_dots.stop();
        const qint64 ms = elapsedMs >= 0 ? elapsedMs : m_clock.elapsed();
        setFinished(QStringLiteral("Thought for %1 s").arg(qMax<qint64>(1, (ms + 500) / 1000)));
    }

    // Reasoning from a saved conversation: how long it took is not known.
    void finishSaved() {
        m_dots.stop();
        setFinished(QStringLiteral("Thought"));
    }

    QString text() const { return m_text; }

private:
    // Some models only say that they reasoned, not what (Claude Code can do
    // this): then there is nothing to open.
    void setFinished(const QString &label) {
        if (m_text.trimmed().isEmpty()) {
            m_toggle->setChecked(false);
            m_toggle->setCheckable(false);
            m_toggle->setArrowType(Qt::NoArrow);
            m_toggle->setText(label + QStringLiteral(" · reasoning not shown"));
            m_toggle->setToolTip(QStringLiteral("The model reasoned before answering, but the text of its reasoning was not provided."));
        } else {
            m_toggle->setText(label);
        }
        render();
    }

    void tick() {
        m_step = m_step % 3 + 1;                                  // 1, 2, 3, 1, ...
        m_toggle->setText(QStringLiteral("Thinking") + QString(m_step, QLatin1Char('.')));
    }

    QToolButton     *m_toggle;
    ChatMessageView *m_body;
    QString          m_text;
    bool             m_dirty = false;
    int              m_step  = 0;
    QTimer           m_dots;
    QElapsedTimer    m_clock;
};

// The message box: grows from one line to eight as text is typed, Enter sends,
// Shift+Enter starts a new line.
class ChatInput : public QPlainTextEdit {
public:
    std::function<void()> onSubmit;

    ChatInput() {
        setObjectName("chatInput");
        setTabChangesFocus(true);
        setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        connect(this, &QPlainTextEdit::textChanged, this, [this] { fit(); });
        fit();
    }

protected:
    void keyPressEvent(QKeyEvent *e) override {
        const bool enter = e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter;
        if (enter && !(e->modifiers() & Qt::ShiftModifier)) {
            if (onSubmit)
                onSubmit();
            return;
        }
        QPlainTextEdit::keyPressEvent(e);
    }

    void resizeEvent(QResizeEvent *e) override {
        QPlainTextEdit::resizeEvent(e);
        fit();                                    // a narrower box wraps into more lines
    }

private:
    void fit() {
        // For a plain-text document the layout's height is its number of lines.
        const int lines = qBound(1, int(document()->size().height()), 8);
        const int h = lines * fontMetrics().lineSpacing() + 2 * int(document()->documentMargin()) + 22;
        if (h != minimumHeight() || h != maximumHeight())
            setFixedHeight(h);
    }
};

ChatPage::ChatPage(QWidget *parent) : QWidget(parent) {
    auto *back = new QPushButton("← Back");
    back->setFlat(true);
    back->setToolTip("Back to the start page. The conversation stays open: change the model or the MCP servers "
                     "there and continue it, or just come back to it.");
    connect(back, &QPushButton::clicked, this, &ChatPage::backRequested);

    m_header = new QLabel;
    m_header->setObjectName("pageHeader");
    m_status = new QLabel;
    m_status->setObjectName("subtle");

    auto *headerRow = new QHBoxLayout;
    headerRow->addWidget(back);
    headerRow->addWidget(m_header, 1);
    headerRow->addWidget(m_status);

    // Transcript: a scrolling column of blocks, with a stretch at the bottom.
    auto *column = new QWidget;
    column->setObjectName("transcriptColumn");
    m_blocks = new QVBoxLayout(column);
    m_blocks->setContentsMargins(16, 10, 16, 14);
    m_blocks->setSpacing(4);
    m_blocks->addStretch(1);
    m_scroll = new QScrollArea;
    m_scroll->setObjectName("transcript");
    m_scroll->setWidgetResizable(true);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scroll->setWidget(column);
    QScrollBar *bar = m_scroll->verticalScrollBar();
    connect(bar, &QScrollBar::valueChanged, this, [this, bar](int v) { m_stickToBottom = v >= bar->maximum() - 4; });
    connect(bar, &QScrollBar::rangeChanged, this, [this, bar](int, int max) {
        if (m_stickToBottom)
            bar->setValue(max);
    });

    m_log = new QPlainTextEdit;
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(2000);
    m_log->setPlaceholderText("agent log (stderr)");
    m_log->setObjectName("log");

    auto *split = new QSplitter(Qt::Vertical);
    split->addWidget(m_scroll);
    split->addWidget(m_log);
    split->setStretchFactor(0, 4);
    split->setStretchFactor(1, 1);
    split->setSizes({500, 120});

    // Files queued for the next message show as a row of chips above the input;
    // the row is hidden while empty.
    m_attachStrip = new QWidget;
    m_attachLayout = new QHBoxLayout(m_attachStrip);
    m_attachLayout->setContentsMargins(0, 0, 0, 0);
    m_attachLayout->setSpacing(4);
    m_attachLayout->addStretch(1);
    m_attachStrip->hide();

    m_attach = new QPushButton("Attach…");
    m_attach->setToolTip("Attach file(s) to send with your next message "
                         "(images go to a vision model; text files are inlined; other files are referenced by path)");
    connect(m_attach, &QPushButton::clicked, this, &ChatPage::attachFiles);

    m_input = new ChatInput;
    m_input->setPlaceholderText("Type a prompt — Enter to send, Shift+Enter for a new line");
    m_input->onSubmit = [this] { submit(); };

    // One button: Send while idle, Stop while a reply is being written.
    m_send = new QPushButton("Send");
    m_send->setObjectName("primary");
    connect(m_send, &QPushButton::clicked, this, [this] {
        if (m_turnActive)
            stopTurn();
        else
            submit();
    });
    // Esc stops a reply too (the input is disabled meanwhile, so it cannot take the key itself).
    auto *esc = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    esc->setContext(Qt::WidgetWithChildrenShortcut);
    connect(esc, &QShortcut::activated, this, &ChatPage::stopTurn);

    auto *inputRow = new QHBoxLayout;
    inputRow->setAlignment(Qt::AlignBottom);
    inputRow->addWidget(m_attach, 0, Qt::AlignBottom);
    inputRow->addWidget(m_input, 1);
    inputRow->addWidget(m_send, 0, Qt::AlignBottom);

    // Who answers and how hard it thinks, for the messages that follow.
    m_modelBox = new QComboBox;
    m_modelBox->setObjectName("chatModel");
    m_modelBox->setToolTip("Who answers from the next message on. The conversation stays as it is: "
                           "the model you switch to gets all of it.");
    // activated, not currentIndexChanged: only a choice made by hand asks for a change.
    connect(m_modelBox, &QComboBox::activated, this, [this](int index) {
        const QStringList choice = m_modelBox->itemData(index).toStringList();
        if (choice.size() != 2)
            return;
        LaunchConfig c = m_config;
        if (choice[0] == "claude") {
            c.backend     = LaunchConfig::Backend::ClaudeCode;
            c.claudeModel = choice[1];
        } else {
            c.backend = LaunchConfig::Backend::LocalModel;
            c.model   = choice[1];
        }
        requestSettings(c);
    });
    m_effort = new EffortScale;
    m_effort->setObjectName("chatEffort");
    connect(m_effort, &EffortScale::levelChanged, this, [this](const QString &level) {
        LaunchConfig c = m_config;
        c.effort = level;
        requestSettings(c);
    });
    auto *optionsRow = new QHBoxLayout;
    optionsRow->setSpacing(18);
    optionsRow->addWidget(m_modelBox);
    optionsRow->addWidget(m_effort);
    optionsRow->addStretch(1);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(headerRow);
    layout->addWidget(split, 1);
    layout->addWidget(m_attachStrip);
    layout->addLayout(inputRow);
    layout->addLayout(optionsRow);

    // Re-rendering Markdown on every token would be wasteful; do it at most
    // every 60 ms while text is streaming in.
    m_renderTimer.setSingleShot(true);
    m_renderTimer.setInterval(60);
    connect(&m_renderTimer, &QTimer::timeout, this, &ChatPage::render);

    m_agent.setProcessChannelMode(QProcess::SeparateChannels);
    proc::ownGroup(m_agent);   // so that stopAgent() reaches the MCP servers the agent runs
    connect(&m_agent, &QProcess::readyReadStandardOutput, this, &ChatPage::onStdout);
    connect(&m_agent, &QProcess::readyReadStandardError,  this, &ChatPage::onStderr);
    connect(&m_agent, &QProcess::finished, this, [this](int code, QProcess::ExitStatus st) {
        m_ready = false;
        m_status->setText(st == QProcess::NormalExit ? QStringLiteral("agent exited (%1)").arg(code)
                                                     : QStringLiteral("agent killed"));
        closeThinking();
        closeAnswer();
        addNote(QStringLiteral("[agent stopped — the conversation is saved; reopen it from the list on the left, "
                               "or use Continue on the start page]"));
        const bool wasAnswering = m_turnActive;
        m_turnActive = false;
        m_configuring = false;
        setBusy(true);
        syncControls();
        if (wasAnswering)
            emit turnFinished();
    });
    connect(&m_agent, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        log("[error] " + m_agent.errorString());
        if (m_agent.state() == QProcess::NotRunning) {
            m_status->setText("agent failed to start");
            setBusy(true);
        }
    });
}

ChatPage::~ChatPage() {
    stopAgent();
}

void ChatPage::begin(const LaunchConfig &config, const Command &agentCommand, const QProcessEnvironment &env,
                     const QString &messagesPath) {
    stopAgent();
    m_config = config;
    m_turnActive = false;
    m_configuring = false;
    syncControls();
    m_header->setText(QStringLiteral("<b>Chat</b> — %1").arg(config.summary().toHtmlEscaped()));
    m_readyStatus.clear();
    m_status->setText("starting agent…");
    clearTranscript();
    replay(messagesPath);
    m_log->clear();
    m_stdoutBuf.clear();
    m_input->clear();
    m_pendingPaths.clear();
    m_pendingAttachments = QJsonArray();
    refreshAttachStrip();
    setBusy(true);

    log("$ " + agentCommand.toString());
    m_agent.setProcessEnvironment(env);
    m_agent.setWorkingDirectory(agentCommand.cwd);
    m_agent.start(commands::resolveProgram(agentCommand.program, env), agentCommand.args);
}

void ChatPage::submit() {
    const QString text = m_input->toPlainText().trimmed();
    if ((text.isEmpty() && m_pendingAttachments.isEmpty()) || !m_ready || m_turnActive || m_configuring)
        return;
    m_input->clear();
    m_stickToBottom = true;
    addSpeaker("You");
    if (!m_pendingPaths.isEmpty())
        addUserAttachments(m_pendingPaths);
    if (!text.isEmpty())
        addUserText(text);
    addSpeaker(m_config.shortModel());   // the reply's blocks follow
    m_turnActive = true;
    setBusy(true);
    emit userMessageSent(text);

    QJsonObject msg{{"type", "user"}, {"content", text}};
    if (!m_pendingAttachments.isEmpty())
        msg["attachments"] = m_pendingAttachments;
    m_agent.write(QJsonDocument(msg).toJson(QJsonDocument::Compact) + '\n');

    m_pendingPaths.clear();
    m_pendingAttachments = QJsonArray();
    refreshAttachStrip();
}

// Ask the agent to stop writing. It answers with "stopped" and "done" (see
// agent.py); until then the page stays busy.
void ChatPage::stopTurn() {
    if (!isBusy())
        return;
    m_agent.write("{\"type\":\"stop\"}\n");
    m_status->setText("stopping…");
    m_send->setEnabled(false);
}

void ChatPage::updateSendButton() {
    m_send->setText(m_turnActive ? "Stop" : "Send");
    m_send->setToolTip(m_turnActive ? "Stop this reply (Esc)" : "Send (Enter)");
    m_send->setEnabled(m_turnActive ? isAgentRunning() : m_ready && !m_configuring);
}

// ---- model and effort ------------------------------------------------------------

void ChatPage::setLocalModels(const QStringList &models) {
    m_localModels = models;
    syncControls();
}

// Fill the model picker and set both controls to what is in use (or, while a
// change is being applied, to what was asked for).
void ChatPage::syncControls() {
    const LaunchConfig &shown = m_configuring ? m_pendingConfig : m_config;
    const bool claudeNow = shown.backend == LaunchConfig::Backend::ClaudeCode;
    const QSignalBlocker block(m_modelBox);
    m_modelBox->clear();

    QStringList local = m_localModels;
    local.removeAll(QString());
    if (!shown.model.isEmpty() && !local.contains(shown.model))
        local.prepend(shown.model);
    for (const QString &id : local) {
        m_modelBox->addItem(id.section('/', -1), QStringList{"local", id});
        m_modelBox->setItemData(m_modelBox->count() - 1, id, Qt::ToolTipRole);
    }
    if (!local.isEmpty())
        m_modelBox->insertSeparator(m_modelBox->count());

    const int firstClaude = m_modelBox->count();
    m_modelBox->addItem("Claude Code · its default model", QStringList{"claude", QString()});
    for (const ClaudeModel &m : claude::models())
        m_modelBox->addItem(QStringLiteral("Claude %1").arg(QLatin1String(m.name)),
                            QStringList{"claude", QString::fromLatin1(m.id)});
    if (claudeNow && !shown.claudeModel.isEmpty() && claude::name(shown.claudeModel).isEmpty())
        m_modelBox->addItem(shown.shortModel(), QStringList{"claude", shown.claudeModel});
    // Without the claude CLI its models can be seen but not chosen.
    if (commands::findClaude().isEmpty() && !claudeNow)
        if (auto *items = qobject_cast<QStandardItemModel *>(m_modelBox->model()))
            for (int i = firstClaude; i < m_modelBox->count(); ++i) {
                items->item(i)->setEnabled(false);
                items->item(i)->setToolTip("The claude command was not found: install Claude Code, or choose it in Settings.");
            }

    const QStringList current{claudeNow ? "claude" : "local", claudeNow ? shown.claudeModel : shown.model};
    m_modelBox->setCurrentIndex(m_modelBox->findData(current));

    m_effort->setBackend(shown.backend);
    m_effort->setLevel(shown.effort);
}

void ChatPage::requestSettings(LaunchConfig config) {
    config.effort = effort::clamp(config.effort, config.backend);   // "off" means nothing to Claude, "max" nothing locally
    const bool same = config.backend == m_config.backend && config.model == m_config.model &&
                      config.claudeModel == m_config.claudeModel && config.effort == m_config.effort;
    if (same || !m_ready || m_turnActive || m_configuring) {
        syncControls();
        return;
    }
    emit settingsChangeRequested(config);
    // Until the change is confirmed ("configured", or a restart through begin()), show what is in effect.
    if (!m_configuring)
        syncControls();
}

void ChatPage::applySettings(const LaunchConfig &config, bool waitForServer) {
    if (!m_ready || m_turnActive || m_configuring || config.backend != m_config.backend) {
        syncControls();
        return;
    }
    const bool claude = config.backend == LaunchConfig::Backend::ClaudeCode;
    m_pendingConfig = config;
    m_pendingWait   = waitForServer;
    m_configuring   = true;
    m_status->setText("switching…");
    setBusy(true);
    QJsonObject msg{{"type", "configure"},
                    {"model", claude ? config.claudeModel : config.model},
                    {"label", config.shortModel()},
                    {"effort", config.effort}};
    if (waitForServer)
        msg["wait_for_server"] = true;
    m_agent.write(QJsonDocument(msg).toJson(QJsonDocument::Compact) + '\n');
}

void ChatPage::onStdout() {
    m_stdoutBuf += m_agent.readAllStandardOutput();
    int nl;
    while ((nl = m_stdoutBuf.indexOf('\n')) >= 0) {
        const QByteArray line = m_stdoutBuf.left(nl).trimmed();
        m_stdoutBuf.remove(0, nl + 1);
        if (line.isEmpty())
            continue;
        const QJsonDocument doc = QJsonDocument::fromJson(line);
        if (doc.isObject())
            handleEvent(doc.object());
        else
            log(QString::fromUtf8(line));   // not protocol: show it
    }
}

void ChatPage::onStderr() {
    const QString text = QString::fromLocal8Bit(m_agent.readAllStandardError());
    for (const QString &line : text.split('\n', Qt::SkipEmptyParts))
        log(line);
}

void ChatPage::handleEvent(const QJsonObject &ev) {
    const QString type = ev["type"].toString();
    if (type == "ready") {
        m_ready = true;
        const int n = ev["tools"].toArray().size();
        m_readyStatus = n ? QStringLiteral("ready · %1 tools").arg(n) : QStringLiteral("ready · no tools");
        m_status->setText(m_readyStatus);
        addNote(m_replayed ? QStringLiteral("Continuing with: %1").arg(m_config.summary())
                           : QStringLiteral("Session started: %1").arg(m_config.summary()));
        setBusy(false);
    } else if (type == "configured") {
        // The answer to applySettings(): the agent now uses what was asked for.
        if (!m_configuring)
            return;
        QStringList changed;
        if (m_pendingConfig.shortModel() != m_config.shortModel())
            changed << QStringLiteral("now answering: %1").arg(m_pendingConfig.shortModel());
        if (m_pendingConfig.effort != m_config.effort)
            changed << QStringLiteral("effort: %1").arg(effort::label(m_pendingConfig.effort).toLower());
        m_config = m_pendingConfig;
        m_configuring = false;
        m_header->setText(QStringLiteral("<b>Chat</b> — %1").arg(m_config.summary().toHtmlEscaped()));
        const int n = ev["tools"].toArray().size();
        m_readyStatus = n ? QStringLiteral("ready · %1 tools").arg(n) : QStringLiteral("ready · no tools");
        m_status->setText(m_readyStatus);
        if (!changed.isEmpty())
            addNote(QStringLiteral("Changed — %1").arg(changed.join(" · ")));
        if (m_pendingWait)
            addNote(QStringLiteral("The model server is restarting with this model. Your next message waits until it "
                                   "is ready (its log is behind “Server log” on the start page)."));
        syncControls();
        setBusy(false);
        emit settingsChanged(m_config);
    } else if (type == "status") {
        if (m_turnActive)
            m_status->setText(ev["message"].toString());
    } else if (type == "delta") {
        appendAnswer(ev["content"].toString());
    } else if (type == "thinking") {
        appendThinking(ev["content"].toString());
    } else if (type == "answer_was_thinking") {
        // What streamed in as answer so far was the model's reasoning (see
        // "Thinking" in agent.py): swap the answer block for a thinking block.
        if (m_answer) {
            auto *block = new ChatThinkingBlock;
            block->append(m_answerText);
            block->finish(m_answerClock.elapsed());
            addBlock(block, m_blocks->indexOf(m_answer));
            delete m_answer;
            m_answer = nullptr;
            m_answerText.clear();
        }
        closeThinking();
    } else if (type == "thinking_was_answer") {
        // The reverse: what was shown as reasoning was the whole answer.
        if (m_thinking) {
            const QString text = m_thinking->text();
            const int index = m_blocks->indexOf(m_thinking);
            delete m_thinking;
            m_thinking = nullptr;
            m_answer = new ChatMessageView;
            m_answerText = text;
            addBlock(m_answer, index);
            render();
        }
    } else if (type == "tool_call") {
        closeThinking();
        closeAnswer();
        addNote(QStringLiteral("⚙ %1(%2)").arg(ev["name"].toString(), compactJson(ev["arguments"], 300)));
    } else if (type == "tool_result") {
        addNote(QStringLiteral("↳ %1").arg(compactJson(ev["content"], 300)));
    } else if (type == "error") {
        closeThinking();
        closeAnswer();
        addNote("[error] " + ev["message"].toString());
    } else if (type == "stopped") {
        closeThinking();
        closeAnswer();
        addNote("[stopped]");
    } else if (type == "done") {
        closeThinking();
        closeAnswer();
        m_turnActive = false;
        m_status->setText(m_readyStatus);
        setBusy(false);
        emit turnFinished();
    }
}

void ChatPage::setBusy(bool busy) {
    m_input->setEnabled(!busy && m_ready);
    m_attach->setEnabled(!busy && m_ready);
    m_modelBox->setEnabled(!busy && m_ready);     // a change applies between messages, not during a reply
    m_effort->setEnabled(!busy && m_ready);
    updateSendButton();
    if (!busy && m_ready)
        m_input->setFocus();
}

// ---- image attachments ----------------------------------------------------------

void ChatPage::attachFiles() {
    const QStringList files = QFileDialog::getOpenFileNames(
        this, QStringLiteral("Attach file(s)"), QString(), QStringLiteral("All files (*)"));
    addFiles(files);
}

void ChatPage::addFiles(const QStringList &paths) {
    for (const QString &path : paths) {
        QString err;
        const QJsonObject a = fileAttachment(path, &err);
        if (a.isEmpty()) {
            log(QStringLiteral("[attach] %1: %2").arg(QFileInfo(path).fileName(), err));
            continue;
        }
        if (a.contains("downscaled"))
            log(QStringLiteral("[attach] %1: downscaled %2 to save tokens")
                    .arg(QFileInfo(path).fileName(), a["downscaled"].toString()));
        m_pendingPaths << path;
        m_pendingAttachments.append(a);
    }
    refreshAttachStrip();
    if (m_ready)
        m_input->setFocus();
}

void ChatPage::refreshAttachStrip() {
    while (m_attachLayout->count() > 1) {        // keep the trailing stretch
        QLayoutItem *item = m_attachLayout->takeAt(0);
        delete item->widget();
        delete item;
    }
    for (int i = 0; i < m_pendingPaths.size(); ++i) {
        const QString path = m_pendingPaths.at(i);
        const QString name = QFileInfo(path).fileName();
        auto *chip = new QFrame;
        chip->setObjectName("chip");
        auto *row = new QHBoxLayout(chip);
        row->setContentsMargins(2, 2, 2, 2);
        row->setSpacing(2);
        auto *thumb = new QLabel;
        const QPixmap pm(path);
        if (!pm.isNull())
            thumb->setPixmap(pm.scaled(44, 44, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        else
            thumb->setText(QStringLiteral("📄 %1").arg(name));
        thumb->setToolTip(name);
        auto *rm = new QToolButton;
        rm->setText(QStringLiteral("✕"));
        rm->setAutoRaise(true);
        rm->setToolTip(QStringLiteral("Remove"));
        connect(rm, &QToolButton::clicked, this, [this, path] {
            const int idx = m_pendingPaths.indexOf(path);
            if (idx >= 0) {
                m_pendingPaths.removeAt(idx);
                m_pendingAttachments.removeAt(idx);
                refreshAttachStrip();
            }
        });
        row->addWidget(thumb);
        row->addWidget(rm);
        m_attachLayout->insertWidget(i, chip);
    }
    m_attachStrip->setVisible(!m_pendingPaths.isEmpty());
}

void ChatPage::addUserAttachments(const QStringList &paths) {
    auto *strip = new QWidget;
    auto *row = new QHBoxLayout(strip);
    row->setContentsMargins(0, 2, 0, 2);
    row->setSpacing(4);
    for (const QString &path : paths) {
        auto *lbl = new QLabel;
        const QPixmap pm(path);
        if (!pm.isNull())
            lbl->setPixmap(pm.scaled(160, 160, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        else
            lbl->setText(QStringLiteral("📄 %1").arg(QFileInfo(path).fileName()));
        lbl->setToolTip(path);
        row->addWidget(lbl);
    }
    row->addStretch(1);
    addBlock(strip);
}

void ChatPage::stopAgent() {
    m_ready = false;
    if (m_agent.state() == QProcess::NotRunning)
        return;
    const qint64 pid = m_agent.processId();
    m_agent.closeWriteChannel();            // EOF on stdin: the agent closes its MCP servers and exits
    if (!m_agent.waitForFinished(4000))
        proc::stop(m_agent, 500);
    proc::sweepGroup(pid);                  // an MCP server that outlived the agent
}

void ChatPage::log(const QString &text) {
    m_log->appendPlainText(text);
}

// ---- transcript -----------------------------------------------------------------

void ChatPage::clearTranscript() {
    m_renderTimer.stop();
    m_answer = nullptr;
    m_thinking = nullptr;
    m_answerText.clear();
    while (m_blocks->count() > 1) {          // keep the stretch at the end
        QLayoutItem *item = m_blocks->takeAt(0);
        delete item->widget();
        delete item;
    }
    m_stickToBottom = true;
}

// Insert before the bottom stretch (index < 0), or at `index`.
void ChatPage::addBlock(QWidget *block, int index) {
    m_blocks->insertWidget(index < 0 ? m_blocks->count() - 1 : index, block);
}

void ChatPage::addSpeaker(const QString &who) {
    auto *label = new QLabel(who.toUpper());
    label->setTextFormat(Qt::PlainText);
    label->setObjectName("speaker");
    label->setContentsMargins(0, 10, 0, 0);
    addBlock(label);
}

// What the user wrote, in a tinted bubble that is only as wide as it needs to be.
void ChatPage::addUserText(const QString &text) {
    auto *bubble = new QFrame;
    bubble->setObjectName("userBubble");
    auto *inner = new QVBoxLayout(bubble);
    inner->setContentsMargins(10, 6, 10, 6);
    auto *mine = new ChatMessageView;
    mine->showPlain(text);
    inner->addWidget(mine);
    addBlock(bubble);
}

// Show the conversation saved in `messagesPath` (format: python/history.py).
void ChatPage::replay(const QString &messagesPath) {
    m_replayed = false;
    QFile f(messagesPath);
    if (messagesPath.isEmpty() || !f.open(QIODevice::ReadOnly))
        return;
    const QJsonArray messages = QJsonDocument::fromJson(f.readAll()).object()["messages"].toArray();
    bool inReply = false;                    // the speaker label of the current reply is already up
    for (const QJsonValue &v : messages) {
        const QJsonObject m = v.toObject();
        const QString role = m["role"].toString();
        if (role == "user") {
            inReply = false;
            addSpeaker("You");
            QStringList paths;
            for (const QJsonValue &a : m["_attachments"].toArray())
                paths << a.toObject()["path"].toString(a.toObject()["name"].toString());
            if (!paths.isEmpty())
                addUserAttachments(paths);
            QString text = m["_text"].toString();
            if (!m.contains("_text")) {      // not written by this app's backends: show what the model was sent
                if (m["content"].isString())
                    text = m["content"].toString();
                for (const QJsonValue &part : m["content"].toArray())
                    text += part.toObject()["text"].toString();
            }
            if (!text.isEmpty())
                addUserText(text);
        } else if (role == "assistant") {
            if (!inReply)
                addSpeaker(m["_model"].toString(QStringLiteral("Assistant")));
            inReply = true;
            if (m.contains("_error")) {
                addNote(m["_stopped"].toBool() ? QStringLiteral("[stopped]") : "[error] " + m["_error"].toString());
                continue;
            }
            if (!m["_thinking"].toString().isEmpty() || m["_thinking_hidden"].toBool()) {
                auto *block = new ChatThinkingBlock;
                block->append(m["_thinking"].toString());
                block->finishSaved();
                addBlock(block);
            }
            if (!m["content"].toString().isEmpty()) {
                auto *answer = new ChatMessageView;
                addBlock(answer);
                answer->showMarkdown(m["content"].toString());
            }
            if (m["_stopped"].toBool())
                addNote("[stopped]");
            for (const QJsonValue &call : m["tool_calls"].toArray()) {
                const QJsonObject fn = call.toObject()["function"].toObject();
                addNote(QStringLiteral("⚙ %1(%2)").arg(fn["name"].toString(), compactJson(fn["arguments"], 300)));
            }
        } else if (role == "tool") {
            addNote(QStringLiteral("↳ %1").arg(compactJson(m["content"], 300)));
        }
    }
    m_replayed = !messages.isEmpty();
}

void ChatPage::addNote(const QString &text) {
    auto *note = new ChatMessageView;
    note->setObjectName("note");
    note->dim();
    note->showPlain(text);
    addBlock(note);
}

void ChatPage::appendAnswer(const QString &text) {
    closeThinking();                         // answer text means the reasoning is over
    if (!m_answer) {
        m_answer = new ChatMessageView;
        m_answerText.clear();
        m_answerClock.start();
        addBlock(m_answer);
    }
    m_answerText += text;
    if (!m_renderTimer.isActive())
        m_renderTimer.start();
}

void ChatPage::appendThinking(const QString &text) {
    if (!m_thinking) {
        closeAnswer();                       // reasoning after some answer text starts a new block
        m_thinking = new ChatThinkingBlock;
        addBlock(m_thinking);
    }
    m_thinking->append(text);
    if (!m_renderTimer.isActive())
        m_renderTimer.start();
}

void ChatPage::closeAnswer() {
    if (!m_answer)
        return;
    render();
    m_answer = nullptr;
    m_answerText.clear();
}

void ChatPage::closeThinking() {
    if (!m_thinking)
        return;
    m_thinking->finish();
    m_thinking = nullptr;
}

void ChatPage::render() {
    if (m_answer)
        m_answer->showMarkdown(m_answerText);
    if (m_thinking)
        m_thinking->render();
}
