#pragma once
#include <QDialog>
#include <QStringList>
#include "McpServers.h"

class QComboBox;
class QLineEdit;
class QTabWidget;
class LocationForm;

// Add or edit one MCP server (one entry of mcp_servers.json).
//
// A server has a name and one or more locations; each location is a tab that
// says how to reach the MCP server (a command to run, or a URL) and,
// optionally, which background service must be running first.
class ServerDialog : public QDialog {
public:
    // `server` is the entry to edit (default-constructed = add a new one).
    // `takenNames` are the names of the other servers.
    ServerDialog(const ServerConfig &server, const QStringList &takenNames, bool isNew, QWidget *parent = nullptr);

    ServerConfig server() const { return m_result; }
    bool deleteRequested() const { return m_delete; }

    // Paste-a-snippet dialog. Returns the servers parsed from what the user
    // pasted, or an empty list if cancelled.
    static QList<ServerConfig> importJson(QWidget *parent);

private:
    void load(const ServerConfig &server);
    void addLocation(const LocationConfig &location);
    void save();

    QStringList m_taken;
    QLineEdit  *m_name        = nullptr;
    QLineEdit  *m_description = nullptr;
    QTabWidget *m_tabs        = nullptr;
    ServerConfig m_result;
    bool         m_delete = false;
};
