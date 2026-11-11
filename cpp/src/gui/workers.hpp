// workers.hpp — 工作线程（gui.py 的 HotplugWatcher / Worker 平移）。
// Qt 信号跨线程为队列连接：自定义载荷类型已在 main_gui.cpp 注册 metatype。
#pragma once
#include <QThread>

#include <atomic>
#include <functional>
#include <memory>

#include "gui_types.hpp"

namespace pt::gui {

// 轮询差分热插拔（任务 1.1 的 GUI 版）。
class HotplugWatcher : public QThread {
    Q_OBJECT
public:
    explicit HotplugWatcher(QObject* parent = nullptr) : QThread(parent) {}
    void run() override {
        try {
            watch_devices([this](const DeviceEvent& ev) { emit eventHappened(ev); }, 1.0,
                          &stop_, EnumerateFn{});
        } catch (...) {
            // 退出阶段的枚举异常忽略
        }
    }
    void stop_watching() { stop_ = true; }

signals:
    void eventHappened(const pt::DeviceEvent& ev);

private:
    std::atomic<bool> stop_{false};
};

// 一次性工作线程：fn() 在后台跑，结果/异常经信号回 GUI 线程。
class Worker : public QThread {
    Q_OBJECT
public:
    using Job = std::function<std::shared_ptr<void>()>;
    explicit Worker(Job job, QObject* parent = nullptr)
        : QThread(parent), job_(std::move(job)) {}
    void run() override {
        JobResult result;
        try {
            result.data = job_();
            result.ok = true;
        } catch (const std::exception& exc) {
            result.ok = false;
            result.error = QString("%1: %2").arg(typeid(exc).name(), exc.what());
        } catch (...) {
            result.ok = false;
            result.error = "未知异常";
        }
        emit done(result);
    }

signals:
    void done(const JobResult& result);

private:
    Job job_;
};

}  // namespace pt::gui
