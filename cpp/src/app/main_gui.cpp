// main_gui.cpp — GUI 入口（gui.py main 平移）。
#include <QApplication>

#include "gui_types.hpp"
#include "main_window.hpp"

int main(int argc, char** argv) {
    qRegisterMetaType<JobResult>("JobResult");
    qRegisterMetaType<pt::DeviceEvent>("pt::DeviceEvent");
    QApplication app(argc, argv);
    MainWindow window;
    window.show();
    return QApplication::exec();
}
