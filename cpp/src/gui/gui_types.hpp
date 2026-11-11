// gui_types.hpp — GUI 层跨线程信号携带的自定义类型（需注册 metatype 走队列连接）。
#pragma once
#include <QMetaType>

#include <memory>
#include <string>

#include "core/device_manager.hpp"

// Worker 后台任务的返回载荷：job 把结果装进 shared_ptr<void>，
// 回调侧按约定 static_pointer_cast 回具体类型（与 Python 的动态值等价）。
struct JobResult {
    bool ok = false;
    QString error;
    std::shared_ptr<void> data;
};

Q_DECLARE_METATYPE(JobResult)
Q_DECLARE_METATYPE(pt::DeviceEvent)
