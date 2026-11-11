// main_gui.cpp — GUI 入口（gui.py main 平移）。
#include <QApplication>

#include "advanced_window.hpp"
#include "gui_types.hpp"
#include "main_window.hpp"

int main(int argc, char** argv) {
    qRegisterMetaType<JobResult>("JobResult");
    qRegisterMetaType<pt::DeviceEvent>("pt::DeviceEvent");
    QApplication app(argc, argv);
    // --advanced：跳过用户端直接开高级窗口（协议开发/学习模式；烟测也用它）
    if (QCoreApplication::arguments().contains("--advanced")) {
        AdvancedWindow window;
        window.show();
        return QApplication::exec();
    }
    MainWindow window;
    window.show();
    return QApplication::exec();
}
