#include <QApplication>
#include "MainWindow.h"
#include "Settings.h"
#include "Style.h"

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("AI Console");   // QSettings location
    QCoreApplication::setApplicationName("AI Console");
    QCoreApplication::setApplicationVersion(AI_CONSOLE_VERSION);

    style::apply(app);

    MainWindow window;
    window.show();

    return app.exec();
}
