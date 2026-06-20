#include "monitor_window.hpp"

#include <QApplication>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    MonitorWindow window;
    window.show();
    return app.exec();
}
