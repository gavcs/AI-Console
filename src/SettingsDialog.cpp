#include "SettingsDialog.h"

#include <QComboBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>

#include "Commands.h"

namespace {
QLabel *hint(const QString &text) {
    auto *l = new QLabel(text);
    l->setObjectName("subtle");
    l->setWordWrap(true);
    return l;
}

// A line edit with a Browse… button that fills it.
QHBoxLayout *withBrowse(QLineEdit *edit, QWidget *parent, bool folder) {
    auto *browse = new QPushButton("Browse…");
    QObject::connect(browse, &QPushButton::clicked, parent, [edit, parent, folder] {
        const QString start = edit->text().isEmpty() ? QDir::homePath() : edit->text();
        const QString picked = folder ? QFileDialog::getExistingDirectory(parent, "Choose a folder", start)
                                      : QFileDialog::getOpenFileName(parent, "Choose a program", start);
        if (!picked.isEmpty())
            edit->setText(picked);
    });
    auto *row = new QHBoxLayout;
    row->addWidget(edit, 1);
    row->addWidget(browse);
    return row;
}
} // namespace

SettingsDialog::SettingsDialog(const AppSettings &current, QWidget *parent) : QDialog(parent), m_result(current) {
    setWindowTitle("Settings");
    setMinimumWidth(640);

    // ---- model server -----------------------------------------------------
    m_provider = new QComboBox;
    for (const Provider &p : settings::providers())
        m_provider->addItem(p.name, p.id);
    m_provider->setCurrentIndex(qMax(0, m_provider->findData(current.provider)));
    m_note = hint(settings::provider(current.provider).note);
    // activated, not currentIndexChanged: only a choice made by hand refills
    // the fields, so opening the dialog never overwrites what was saved.
    connect(m_provider, &QComboBox::activated, this, [this] {
        applyProvider(settings::provider(m_provider->currentData().toString()));
    });

    m_baseUrl = new QLineEdit(current.baseUrl);
    m_baseUrl->setPlaceholderText("http://127.0.0.1:8000/v1");
    m_start = new QLineEdit(current.startCommand);
    m_start->setPlaceholderText("nothing: the server is started somewhere else");
    m_start->setCursorPosition(0);            // show the start of a long command, not its end
    m_models = new QPlainTextEdit(current.models.join('\n'));
    m_models->setPlaceholderText("one model id per line");
    m_models->setFixedHeight(64);
    m_models->setTabChangesFocus(true);
    m_apiKey = new QLineEdit(current.apiKey);
    m_apiKey->setEchoMode(QLineEdit::PasswordEchoOnEdit);
    m_apiKey->setPlaceholderText("none");

    auto *serverBox  = new QGroupBox("Model server");
    auto *serverForm = new QFormLayout(serverBox);
    serverForm->addRow("Provider:", m_provider);
    serverForm->addRow(QString(), m_note);
    serverForm->addRow("Address:", m_baseUrl);
    serverForm->addRow("Start command:", m_start);
    serverForm->addRow(QString(), hint("What “Start server” runs. {model} becomes the model chosen on the start page, "
                                       "{port} and {host} come from the address. Leave it empty if the server is "
                                       "started elsewhere; the app then only shows whether it is reachable."));
    serverForm->addRow("Models:", m_models);
    serverForm->addRow("API key:", m_apiKey);
    serverForm->addRow(QString(), hint("Only for a server that asks for one. It is stored as plain text in settings.json."));

    // ---- programs ---------------------------------------------------------
    m_python = new QLineEdit(current.python);
    m_python->setPlaceholderText("python3");
    m_claude = new QLineEdit(current.claude);
    const QString found = commands::findClaude();
    m_claude->setPlaceholderText(found.isEmpty() ? QStringLiteral("not found: install Claude Code or choose it here")
                                                 : QStringLiteral("found automatically: %1").arg(found));
    m_scripts = new QLineEdit(current.scriptsDir);
    m_scripts->setPlaceholderText(commands::defaultScriptsDir());

    auto *programBox  = new QGroupBox("Programs and folders");
    auto *programForm = new QFormLayout(programBox);
    programForm->addRow("Python:", withBrowse(m_python, this, false));
    programForm->addRow(QString(), hint("Runs the agent and your scripts; it needs the mcp package "
                                        "(pip install -r requirements.txt). Point it at a virtual environment's "
                                        "python if that is where you installed it."));
    programForm->addRow("Claude Code:", withBrowse(m_claude, this, false));
    programForm->addRow("Scripts folder:", withBrowse(m_scripts, this, true));

    // ---- buttons ----------------------------------------------------------
    auto *dataFolder = new QPushButton("Open data folder");
    dataFolder->setToolTip("Where settings.json, mcp_servers.json and the saved sessions are kept");
    connect(dataFolder, &QPushButton::clicked, this,
            [] { QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(settings::path()).absolutePath())); });
    auto *reset = new QPushButton("Defaults");
    reset->setToolTip("Put the original values back in every field (nothing is saved until OK)");
    connect(reset, &QPushButton::clicked, this, [this] {
        const AppSettings d = settings::defaults();
        m_provider->setCurrentIndex(m_provider->findData(d.provider));
        applyProvider(settings::provider(d.provider));
        m_apiKey->clear();
        m_python->setText(d.python);
        m_claude->clear();
        m_scripts->clear();
    });
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &SettingsDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &SettingsDialog::reject);
    auto *bottom = new QHBoxLayout;
    bottom->addWidget(dataFolder);
    bottom->addWidget(reset);
    bottom->addStretch(1);
    bottom->addWidget(buttons);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(serverBox);
    layout->addWidget(programBox);
    layout->addWidget(hint("Changes apply to sessions started from now on. A chat that is open keeps what it was "
                           "started with until you use Continue on the start page."));
    layout->addLayout(bottom);
}

void SettingsDialog::applyProvider(const Provider &p) {
    m_note->setText(p.note);
    m_baseUrl->setText(p.baseUrl);
    m_start->setText(p.startCommand);
    m_start->setCursorPosition(0);
    m_models->setPlainText(p.model);
}

void SettingsDialog::accept() {
    const QUrl url(m_baseUrl->text().trimmed());
    if (!url.isValid() || url.host().isEmpty() || (url.scheme() != "http" && url.scheme() != "https")) {
        QMessageBox::warning(this, "Settings", "The server address should look like http://127.0.0.1:8000/v1");
        return;
    }
    m_result.provider     = m_provider->currentData().toString();
    m_result.baseUrl      = m_baseUrl->text().trimmed();
    m_result.startCommand = m_start->text().trimmed();
    m_result.models.clear();
    for (const QString &line : m_models->toPlainText().split('\n', Qt::SkipEmptyParts))
        if (!line.trimmed().isEmpty())
            m_result.models << line.trimmed();
    m_result.apiKey     = m_apiKey->text().trimmed();
    m_result.python     = m_python->text().trimmed().isEmpty() ? QStringLiteral("python3") : m_python->text().trimmed();
    m_result.claude     = m_claude->text().trimmed();
    m_result.scriptsDir = m_scripts->text().trimmed();
    QDialog::accept();
}
