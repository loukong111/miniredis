#include "monitor_window.hpp"

#include <QApplication>
#include <QStringList>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    MonitorWindow window;

    QStringList args = app.arguments();
    int screenshot_arg = args.indexOf("--export-screenshots");
    if (screenshot_arg >= 0) {
        QString directory = (screenshot_arg + 1 < args.size()) ? args.at(screenshot_arg + 1) : "docs/images";
        return window.exportScreenshots(directory) ? 0 : 1;
    }

    window.show();
    return app.exec();
}
