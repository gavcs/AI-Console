#pragma once
#include <QApplication>
#include <QColor>
#include <QDir>
#include <QGuiApplication>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QString>
#include <QStyleHints>

// ---------------------------------------------------------------------------
// The app-wide look: one palette and one stylesheet, in a light and a dark
// version that follow the system setting. Widgets opt into the special cases
// by object name (setObjectName), listed next to each rule below; nothing
// else in the app sets colours, apart from the status dots.
// ---------------------------------------------------------------------------

namespace style {

struct Colors {
    const char *window, *sidebar, *card, *border, *text, *subtle, *accent, *accentHover,
               *input, *bubble, *log, *hover, *selected, *notice, *noticeBorder;
};

inline bool isDark() {
    return QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
}

inline Colors colors() {
    static const Colors light{"#f4f5f8", "#e9ebf0", "#ffffff", "#d8dce4", "#1c2028", "#6c7482", "#4a67e8", "#3c57d0",
                              "#ffffff", "#e4e9ff", "#f8f9fb", "#dfe3ea", "#d5dcfa", "#fff6dd", "#e6cf8a"};
    static const Colors dark{"#1a1c21", "#141619", "#23262d", "#363b46", "#e4e7ec", "#8f97a5", "#7089ff", "#8499ff",
                             "#1d2026", "#2b3457", "#16181c", "#2a2e37", "#2f3a66", "#3a3320", "#6b5c2a"};
    return isDark() ? dark : light;
}

// Colour for dimmed HTML text inside labels (rich text does not see the stylesheet).
inline QString subtleColor() { return QString::fromLatin1(colors().subtle); }

// The tick inside a ticked checkbox. A stylesheet can only take it from an
// image file, so one is drawn and kept in the temp folder.
inline QString checkmarkFile() {
    static const QString path = [] {
        QPixmap pm(28, 28);                       // drawn at 2x, shown at 14 px
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(Qt::white, 3.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPolyline(QPolygonF{{6.5, 14.5}, {12, 20}, {21.5, 8.5}});
        p.end();
        const QString file = QDir::tempPath() + QStringLiteral("/ai_console_check.png");
        pm.save(file);
        return file;
    }();
    return path;
}

inline QString sheet(const Colors &c) {
    QString s = QStringLiteral(R"(
QWidget { font-size: 13px; }
QMainWindow, QStackedWidget { background: %window; }
QToolTip { background: %card; color: %text; border: 1px solid %border; padding: 4px; }

/* sidebar (MainWindow): #sidebar, #appTitle, the session list #sessions */
#sidebar { background: %sidebar; border-right: 1px solid %border; }
#appTitle { font-size: 16px; font-weight: 700; padding: 4px 6px; }
QListWidget#sessions { background: transparent; border: none; outline: 0; }
QListWidget#sessions::item { padding: 7px 10px; margin: 1px 0; border-radius: 8px; color: %text; }
QListWidget#sessions::item:hover { background: %hover; }
QListWidget#sessions::item:selected { background: %selected; color: %text; }

/* cards */
QGroupBox { background: %card; border: 1px solid %border; border-radius: 10px;
            margin-top: 24px; padding: 10px 12px 12px 12px; font-weight: 600; }
QGroupBox::title { subcontrol-origin: margin; subcontrol-position: top left; left: 4px; top: 4px; color: %subtle; }
QGroupBox QWidget { font-weight: normal; }
#pageTitle { font-size: 22px; font-weight: 700; }
#pageHeader { font-size: 14px; }
#subtle, QStatusBar, QStatusBar QLabel { color: %subtle; }
/* #notice: something the user should fix (start page) */
QLabel#notice { background: %notice; border: 1px solid %noticeBorder; border-radius: 8px; padding: 9px 12px; }

/* buttons: #primary is the one main action of a page, flat ones are links */
QPushButton { background: %card; color: %text; border: 1px solid %border; border-radius: 6px;
              padding: 5px 13px; min-height: 18px; }
QPushButton:hover { border-color: %accent; }
QPushButton:pressed { background: %hover; }
QPushButton:disabled { color: %subtle; background: transparent; }
QPushButton#primary, QPushButton:default { background: %accent; color: white; border-color: %accent; font-weight: 600; }
QPushButton#primary:hover, QPushButton:default:hover { background: %accentHover; }
QPushButton#primary:disabled, QPushButton:default:disabled { background: %border; border-color: %border; color: %subtle; }
QPushButton:flat { background: transparent; border: none; color: %accent; padding: 5px 6px; }
QPushButton:flat:hover { text-decoration: underline; }
QToolButton { border: none; background: transparent; color: %subtle; padding: 2px 4px; border-radius: 4px; }
QToolButton:hover { background: %hover; color: %text; }

/* radio buttons and checkboxes: drawn here so they read the same in light and dark */
QRadioButton, QCheckBox { spacing: 7px; color: %text; }
QRadioButton:disabled, QCheckBox:disabled { color: %subtle; }
QRadioButton::indicator, QCheckBox::indicator { width: 14px; height: 14px; border: 1px solid %subtle; background: %input; }
QRadioButton::indicator { border-radius: 8px; }
QCheckBox::indicator { border-radius: 4px; }
QRadioButton::indicator:hover, QCheckBox::indicator:hover { border-color: %accent; }
QRadioButton::indicator:checked { border: 5px solid %accent; width: 6px; height: 6px; background: white; }
QCheckBox::indicator:checked { background: %accent; border-color: %accent; image: url("%check"); }
QRadioButton::indicator:disabled, QCheckBox::indicator:disabled { border-color: %border; }
QRadioButton::indicator:checked:disabled { border-color: %subtle; }
QCheckBox::indicator:checked:disabled { background: %subtle; border-color: %subtle; }

/* text inputs; process output and the chat's agent log are #log */
QLineEdit, QPlainTextEdit { background: %input; color: %text; border: 1px solid %border; border-radius: 6px;
                            padding: 5px 8px; selection-background-color: %accent; selection-color: white; }
QLineEdit:focus, QPlainTextEdit:focus { border-color: %accent; }
QLineEdit:disabled, QPlainTextEdit:disabled { color: %subtle; background: %window; }
QPlainTextEdit#chatInput { padding: 6px 10px; border-radius: 10px; font-size: 14px; }
QLineEdit#sessionFilter { border-radius: 8px; padding: 4px 8px; }
QPlainTextEdit#log { background: %log; font-family: Menlo, Monaco, monospace; font-size: 11px; border-radius: 8px; }

/* chat transcript: #transcript (scroll area), #transcriptColumn, #speaker, #userBubble, #note */
QScrollArea#transcript { background: %card; border: 1px solid %border; border-radius: 10px; }
#transcriptColumn { background: %card; }
QLabel#speaker { color: %subtle; font-size: 11px; font-weight: 700; }
QFrame#userBubble { background: %bubble; border-radius: 10px; }
QTextBrowser { background: transparent; border: none; }
QTextBrowser#note { font-family: Menlo, Monaco, monospace; font-size: 11px; }
QFrame#chip { background: %hover; border-radius: 8px; }

QSplitter::handle { background: transparent; }
QScrollBar:vertical { background: transparent; width: 10px; margin: 2px; }
QScrollBar::handle:vertical { background: %border; border-radius: 3px; min-height: 30px; }
QScrollBar::handle:vertical:hover { background: %subtle; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }
)");
    const std::pair<const char *, const char *> tokens[] = {
        {"%window", c.window}, {"%sidebar", c.sidebar}, {"%card", c.card}, {"%border", c.border},
        {"%text", c.text}, {"%subtle", c.subtle}, {"%accentHover", c.accentHover}, {"%accent", c.accent},
        {"%input", c.input}, {"%bubble", c.bubble}, {"%log", c.log}, {"%hover", c.hover}, {"%selected", c.selected},
        {"%noticeBorder", c.noticeBorder}, {"%notice", c.notice}};
    for (const auto &[token, value] : tokens)
        s.replace(QLatin1String(token), QLatin1String(value));
    s.replace(QLatin1String("%check"), checkmarkFile());
    return s;
}

inline QPalette palette(const Colors &c) {
    QPalette p;
    const QColor text(c.text), subtle(c.subtle);
    p.setColor(QPalette::Window, QColor(c.window));
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, QColor(c.input));
    p.setColor(QPalette::AlternateBase, QColor(c.card));
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, QColor(c.card));
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::ToolTipBase, QColor(c.card));
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::PlaceholderText, subtle);
    p.setColor(QPalette::Highlight, QColor(c.accent));
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::Link, QColor(c.accent));
    p.setColor(QPalette::Mid, QColor(c.border));
    for (QPalette::ColorRole role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        p.setColor(QPalette::Disabled, role, subtle);
    return p;
}

// Call once after creating the QApplication. Follows light/dark changes.
inline void apply(QApplication &app) {
    app.setStyle(QStringLiteral("Fusion"));   // the one style that draws the same everywhere and takes a palette
    const auto set = [&app] {
        app.setPalette(palette(colors()));
        app.setStyleSheet(sheet(colors()));
    };
    set();
    QObject::connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, &app, set);
}

} // namespace style
