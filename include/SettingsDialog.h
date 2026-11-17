#pragma once
#include <QDialog>
#include "Settings.h"

class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;

// Edit the app's settings (Settings.h): the model server and how to start it,
// and where Python, the claude CLI and generated scripts are.
//
// Choosing a provider fills in its address, start command and an example
// model; all three can then be changed. Nothing is saved by the dialog itself:
// the caller saves settings() if the dialog was accepted.
class SettingsDialog : public QDialog {
public:
    explicit SettingsDialog(const AppSettings &current, QWidget *parent = nullptr);

    AppSettings settings() const { return m_result; }

private:
    void applyProvider(const Provider &p);   // fill the server fields from a preset
    void accept() override;

    QComboBox      *m_provider = nullptr;
    QLabel         *m_note     = nullptr;
    QLineEdit      *m_baseUrl  = nullptr;
    QLineEdit      *m_start    = nullptr;
    QLineEdit      *m_stop     = nullptr;
    QPlainTextEdit *m_models   = nullptr;
    QLineEdit      *m_apiKey   = nullptr;
    QLineEdit      *m_python   = nullptr;
    QLineEdit      *m_claude   = nullptr;
    QLineEdit      *m_scripts  = nullptr;
    AppSettings     m_result;
};
