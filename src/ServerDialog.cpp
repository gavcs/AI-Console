#include "ServerDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>

namespace {

// "python3 server.py --flag" <-> {"python3", "server.py", "--flag"}.
// Same quoting rules as QProcess::splitCommand: "double quotes" keep spaces
// together, and """ is a literal quote.
QString joinCommand(const QString &program, const QStringList &args) {
    QStringList parts;
    for (QString a : QStringList{program} + args) {
        if (a.isEmpty() || a.contains(' ') || a.contains('\t') || a.contains('"')) {
            a.replace(QLatin1Char('"'), QLatin1String("\"\"\""));
            a = '"' + a + '"';
        }
        parts << a;
    }
    return program.isEmpty() ? QString() : parts.join(' ');
}

// "KEY=VALUE" lines <-> map.
QString pairsToText(const QMap<QString, QString> &m) {
    QStringList lines;
    for (auto it = m.begin(); it != m.end(); ++it)
        lines << it.key() + '=' + it.value();
    return lines.join('\n');
}

QMap<QString, QString> textToPairs(const QString &text) {
    QMap<QString, QString> m;
    for (const QString &line : text.split('\n', Qt::SkipEmptyParts)) {
        const int eq = line.indexOf('=');
        if (eq > 0)
            m.insert(line.left(eq).trimmed(), line.mid(eq + 1).trimmed());
    }
    return m;
}

QPlainTextEdit *smallEdit(const QString &placeholder) {
    auto *e = new QPlainTextEdit;
    e->setPlaceholderText(placeholder);
    e->setFixedHeight(54);
    e->setTabChangesFocus(true);
    return e;
}

} // namespace

// One tab of the dialog: how to reach the server in one location.
class LocationForm : public QWidget {
public:
    explicit LocationForm(const LocationConfig &l) : m_id(l.id) {
        m_label = new QLineEdit(l.label);
        m_label->setPlaceholderText("Shown in the dropdown, e.g. Local");

        m_type = new QComboBox;
        m_type->addItem("Run a command (stdio)", "stdio");
        m_type->addItem("Connect to a URL (streamable HTTP)", "http");
        m_type->addItem("Connect to a URL (SSE, older servers)", "sse");
        m_type->setCurrentIndex(qMax(0, m_type->findData(l.transport)));

        m_command = new QLineEdit(joinCommand(l.command, l.args));
        m_command->setPlaceholderText("e.g. npx -y @modelcontextprotocol/server-filesystem /some/folder");
        m_cwd = new QLineEdit(l.cwd);
        m_cwd->setPlaceholderText("optional");
        m_env = smallEdit("optional, one KEY=VALUE per line");
        m_env->setPlainText(pairsToText(l.env));

        m_url = new QLineEdit(l.url);
        m_url->setPlaceholderText("e.g. http://127.0.0.1:8081/mcp");
        m_headers = smallEdit("optional, one NAME=VALUE per line, e.g. Authorization=Bearer abc");
        m_headers->setPlainText(pairsToText(l.headers));

        m_form = new QFormLayout;
        m_form->addRow("Label:", m_label);
        m_form->addRow("Type:", m_type);
        m_form->addRow("Command:", m_command);
        m_form->addRow("Working folder:", m_cwd);
        m_form->addRow("Environment:", m_env);
        m_form->addRow("URL:", m_url);
        m_form->addRow("Headers:", m_headers);

        // ---- background service ------------------------------------------
        const ServiceConfig &s = l.service;
        m_service = new QGroupBox("Needs a background service running first");
        m_service->setCheckable(true);
        m_service->setChecked(s.enabled);
        m_host = new QLineEdit(s.host);
        m_host->setPlaceholderText("empty = this Mac; otherwise an ssh destination, e.g. user@host");
        m_port = new QSpinBox;
        m_port->setRange(0, 65535);
        m_port->setValue(s.port);
        m_localPort = new QSpinBox;
        m_localPort->setRange(0, 65535);
        m_localPort->setSpecialValueText("same port");
        m_localPort->setValue(s.localPort);
        m_start = new QLineEdit(s.start);
        m_start->setPlaceholderText("optional: shell command that starts it there if it isn't running");
        m_healthPath = new QLineEdit(s.healthPath);
        m_healthPath->setPlaceholderText("/");
        m_infoPath = new QLineEdit(s.infoPath);
        m_infoPath->setPlaceholderText("optional: HTTP path describing the service, printed in the log");
        m_logFile = new QLineEdit(s.logFile);
        m_logFile->setPlaceholderText("optional: its log file there; new lines are copied into the log");
        m_hint = new QLineEdit(s.hint);
        m_hint->setPlaceholderText("optional: shown if it hasn't come up after a minute");

        auto *sf = new QFormLayout(m_service);
        sf->addRow("Runs on:", m_host);
        sf->addRow("Service port:", m_port);
        sf->addRow("Port on this Mac:", m_localPort);
        sf->addRow("Start command:", m_start);
        sf->addRow("Health path:", m_healthPath);
        sf->addRow("Info path:", m_infoPath);
        sf->addRow("Log file:", m_logFile);
        sf->addRow("Hint:", m_hint);

        auto *note = new QLabel(
            "The service must answer HTTP on its port. For a remote service the app opens one ssh connection that "
            "starts it if needed and forwards \"Port on this Mac\" to it. In the command, URL, environment and "
            "headers, ${SERVICE_URL} and ${SERVICE_PORT} stand for where the service is reached on this Mac; "
            "${HOME} and ${APP_PYTHON} (this app's python folder) also work.");
        note->setWordWrap(true);

        auto *layout = new QVBoxLayout(this);
        layout->addLayout(m_form);
        layout->addWidget(m_service);
        layout->addWidget(note);
        layout->addStretch();

        connect(m_type, &QComboBox::currentIndexChanged, this, [this] { updateRows(); });
        connect(m_host, &QLineEdit::textChanged, this, [this] { updateRows(); });
        updateRows();
    }

    QLineEdit *labelEdit() const { return m_label; }

    LocationConfig location() const {
        LocationConfig l;
        l.id        = m_id;
        l.label     = m_label->text().trimmed();
        l.transport = m_type->currentData().toString();
        if (l.transport == "stdio") {
            QStringList parts = QProcess::splitCommand(m_command->text().trimmed());
            if (!parts.isEmpty())
                l.command = parts.takeFirst();
            l.args = parts;
            l.cwd  = m_cwd->text().trimmed();
            l.env  = textToPairs(m_env->toPlainText());
        } else {
            l.url     = m_url->text().trimmed();
            l.headers = textToPairs(m_headers->toPlainText());
        }
        ServiceConfig &s = l.service;
        s.enabled = m_service->isChecked();
        if (s.enabled) {
            s.host       = m_host->text().trimmed();
            s.port       = m_port->value();
            s.localPort  = s.host.isEmpty() ? 0 : m_localPort->value();
            s.start      = m_start->text().trimmed();
            s.healthPath = m_healthPath->text().trimmed();
            s.infoPath   = m_infoPath->text().trimmed();
            s.logFile    = m_logFile->text().trimmed();
            s.hint       = m_hint->text().trimmed();
        }
        return l;
    }

private:
    void updateRows() {
        const bool stdio = m_type->currentData().toString() == "stdio";
        m_form->setRowVisible(m_command, stdio);
        m_form->setRowVisible(m_cwd, stdio);
        m_form->setRowVisible(m_env, stdio);
        m_form->setRowVisible(m_url, !stdio);
        m_form->setRowVisible(m_headers, !stdio);
        m_localPort->setEnabled(!m_host->text().trimmed().isEmpty());   // only a remote service is forwarded
    }

    QString         m_id;
    QFormLayout    *m_form;
    QLineEdit      *m_label, *m_command, *m_cwd, *m_url;
    QComboBox      *m_type;
    QPlainTextEdit *m_env, *m_headers;
    QGroupBox      *m_service;
    QLineEdit      *m_host, *m_start, *m_healthPath, *m_infoPath, *m_logFile, *m_hint;
    QSpinBox       *m_port, *m_localPort;
};

ServerDialog::ServerDialog(const ServerConfig &server, const QStringList &takenNames, bool isNew, QWidget *parent)
    : QDialog(parent), m_taken(takenNames) {
    setWindowTitle(isNew ? "Add MCP server" : "Edit MCP server");

    m_name = new QLineEdit;
    m_name->setPlaceholderText("letters, digits, - and _ ; becomes the prefix of its tool names");
    m_description = new QLineEdit;
    m_description->setPlaceholderText("optional");

    auto *top = new QFormLayout;
    if (isNew) {
        // Presets are starting points shipped in presets/: choosing one fills
        // in the form, to be adjusted (hosts, paths) before saving.
        const QList<ServerConfig> presets = mcp::presets();
        if (!presets.isEmpty()) {
            auto *preset = new QComboBox;
            preset->addItem("Blank");
            for (const ServerConfig &p : presets)
                preset->addItem(p.description.isEmpty() ? p.name : QStringLiteral("%1 — %2").arg(p.name, p.description));
            connect(preset, &QComboBox::currentIndexChanged, this, [this, presets](int i) {
                load(i > 0 ? presets.at(i - 1) : ServerConfig{});
            });
            top->addRow("Start from:", preset);
        }
    }
    top->addRow("Name:", m_name);
    top->addRow("Description:", m_description);

    m_tabs = new QTabWidget;
    auto *addLoc    = new QPushButton("Add location");
    auto *removeLoc = new QPushButton("Remove this location");
    connect(addLoc, &QPushButton::clicked, this, [this] {
        addLocation(LocationConfig{});
        m_tabs->setCurrentIndex(m_tabs->count() - 1);
    });
    connect(removeLoc, &QPushButton::clicked, this, [this] {
        if (m_tabs->count() > 1)
            delete m_tabs->currentWidget();
    });
    auto *locNote = new QLabel("One location is enough for most servers. Add another when the same tool can run in "
                               "two places (say, on this Mac and on another machine): the launch screen then lets "
                               "you choose per session.");
    locNote->setWordWrap(true);
    auto *locButtons = new QHBoxLayout;
    locButtons->addWidget(addLoc);
    locButtons->addWidget(removeLoc);
    locButtons->addStretch();

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    if (!isNew) {
        QPushButton *del = buttons->addButton("Delete server", QDialogButtonBox::DestructiveRole);
        connect(del, &QPushButton::clicked, this, [this] {
            if (QMessageBox::question(this, "Delete server",
                                      QStringLiteral("Remove \"%1\" from the app?").arg(m_name->text())) == QMessageBox::Yes) {
                m_delete = true;
                accept();
            }
        });
    }
    connect(buttons, &QDialogButtonBox::accepted, this, &ServerDialog::save);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(top);
    layout->addWidget(m_tabs, 1);
    layout->addLayout(locButtons);
    layout->addWidget(locNote);
    layout->addWidget(buttons);
    resize(640, 720);

    load(server);
}

void ServerDialog::load(const ServerConfig &server) {
    m_name->setText(server.name);
    m_description->setText(server.description);
    while (m_tabs->count() > 0)
        delete m_tabs->widget(0);
    for (const LocationConfig &l : server.locations)
        addLocation(l);
    if (m_tabs->count() == 0)
        addLocation(LocationConfig{});
}

void ServerDialog::addLocation(const LocationConfig &location) {
    auto *form = new LocationForm(location);
    const int index = m_tabs->addTab(form, location.label.isEmpty() ? QStringLiteral("Location %1").arg(m_tabs->count() + 1)
                                                                   : location.label);
    Q_UNUSED(index);
    connect(form->labelEdit(), &QLineEdit::textChanged, this, [this, form](const QString &text) {
        const int i = m_tabs->indexOf(form);
        m_tabs->setTabText(i, text.trimmed().isEmpty() ? QStringLiteral("Location %1").arg(i + 1) : text.trimmed());
    });
}

void ServerDialog::save() {
    ServerConfig s;
    s.name        = m_name->text().trimmed();
    s.description = m_description->text().trimmed();
    QString why;
    if (!mcp::validName(s.name))
        why = "Give the server a name using letters, digits, - and _ (at most 32 characters).";
    else if (m_taken.contains(s.name))
        why = QStringLiteral("There is already a server called \"%1\".").arg(s.name);
    for (int i = 0; i < m_tabs->count() && why.isEmpty(); ++i) {
        const LocationConfig l = static_cast<LocationForm *>(m_tabs->widget(i))->location();
        const QString problem = mcp::problem(l);
        if (!problem.isEmpty()) {
            why = QStringLiteral("%1 %2.").arg(m_tabs->tabText(i), problem);
            m_tabs->setCurrentIndex(i);
        }
        s.locations << l;
    }
    if (!why.isEmpty()) {
        QMessageBox::warning(this, "Can't save", why);
        return;
    }
    mcp::normalize(s);   // ids and default labels
    m_result = s;
    accept();
}

QList<ServerConfig> ServerDialog::importJson(QWidget *parent) {
    QDialog dlg(parent);
    dlg.setWindowTitle("Import MCP servers from JSON");
    auto *help = new QLabel("Paste the configuration from a server's README. The usual layouts work:\n"
                            "{ \"mcpServers\": { \"name\": { \"command\": \"...\", \"args\": [...] } } }   or   "
                            "{ \"name\": { \"type\": \"http\", \"url\": \"...\" } }");
    help->setWordWrap(true);
    auto *text = new QPlainTextEdit;
    text->setPlaceholderText("{\n  \"mcpServers\": {\n    \"example\": { \"command\": \"npx\", \"args\": [\"-y\", \"some-mcp-server\"] }\n  }\n}");
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    QPushButton *import = buttons->addButton("Import", QDialogButtonBox::AcceptRole);
    QList<ServerConfig> result;
    QObject::connect(import, &QPushButton::clicked, &dlg, [&] {
        QString error;
        result = mcp::parseServers(text->toPlainText().toUtf8(), &error);
        if (result.isEmpty()) {
            QMessageBox::warning(&dlg, "Can't import", error.isEmpty() ? "No servers found in that JSON." : error);
            return;
        }
        dlg.accept();
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    auto *layout = new QVBoxLayout(&dlg);
    layout->addWidget(help);
    layout->addWidget(text, 1);
    layout->addWidget(buttons);
    dlg.resize(600, 420);
    return dlg.exec() == QDialog::Accepted ? result : QList<ServerConfig>{};
}
