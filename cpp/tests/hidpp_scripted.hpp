// hidpp_scripted.hpp — HID++ 脚本化应答构造器：与 core/hidpp 的 sw_id 轮换天然对齐。
// 原理：被测代码按顺序消耗 sw_id(1-7 循环)；本类按"应答顺序"预测同一序列，
// 双方一一对应。每个测试 new 一个 HidppScript（计数器归零）。
#pragma once
#include <cstdint>

#include "core/hidpp.hpp"
#include "test_report_io.hpp"

namespace pt_test {

class HidppScript {
public:
    HidppScript() {
        pt::hidpp_reset_sw_id();
        n_ = 0;
    }

    // Root.GetFeature 应答（funct 0）。feat_index=0 表示不支持。
    pt::Bytes gf(int dev, int feat_index) {
        return {0x10, static_cast<std::uint8_t>(dev), 0x00, static_cast<std::uint8_t>(sw()),
                static_cast<std::uint8_t>(feat_index), 0, 0};
    }

    // feature 调用应答：payload = req-id 回显之后的数据段。
    pt::Bytes call(int dev, int feat_index, int funct, const pt::Bytes& payload,
                   bool long_msg = false) {
        const int rid = long_msg ? 0x11 : 0x10;
        pt::Bytes wire{static_cast<std::uint8_t>(rid), static_cast<std::uint8_t>(dev),
                       static_cast<std::uint8_t>(feat_index),
                       static_cast<std::uint8_t>(((funct & 0x0F) << 4) | sw())};
        wire.insert(wire.end(), payload.begin(), payload.end());
        wire.resize(long_msg ? 20 : 7, 0x00);
        return wire;
    }

    // ping 应答（PRO X2 实测不回显 mark，构造器按需带）。
    pt::Bytes ping(int dev, int major, int minor, bool long_msg = false, int mark = 66) {
        const int rid = long_msg ? 0x11 : 0x10;
        pt::Bytes wire{static_cast<std::uint8_t>(rid), static_cast<std::uint8_t>(dev), 0x00,
                       static_cast<std::uint8_t>(0x10 | sw()),
                       static_cast<std::uint8_t>(major), static_cast<std::uint8_t>(minor),
                       static_cast<std::uint8_t>(mark)};
        wire.resize(long_msg ? 20 : 7, 0x00);
        return wire;
    }

    // 0x8F 错误应答（reqLo 回显 0x10|sw，与 ping 请求对齐）。
    pt::Bytes err(int dev, int error_code, bool long_msg = false) {
        const int rid = long_msg ? 0x11 : 0x10;
        pt::Bytes wire{static_cast<std::uint8_t>(rid), static_cast<std::uint8_t>(dev), 0x8F,
                       0x00, static_cast<std::uint8_t>(0x10 | sw()),
                       static_cast<std::uint8_t>(error_code), 0x00};
        wire.resize(long_msg ? 20 : 7, 0x00);
        return wire;
    }

private:
    int sw() { return ((n_++) % 7) + 1; }  // SUT 第 k 次消耗得到 ((k-1)%7)+1
    int n_ = 0;
};

}  // namespace pt_test
