// fake_backend.hpp — 测试用假鼠标后端（test_mouse_api / test_migration 共用）。
#pragma once
#include "core/mouse_api.hpp"

namespace pt::test {

class FakeBackend : public MouseBackend {
public:
    bool matches(const std::string& vid_hex, const std::vector<DeviceInfo>&) const override {
        return vid_hex == "ABCD";
    }
    void build(const std::vector<DeviceInfo>&) override {}
    MouseCapabilities capabilities() override {
        MouseCapabilities caps;
        caps.dpi_list = {400, 800, 1600};
        caps.dpi_support = true;
        caps.poll_rates = {1000, 500};
        caps.poll_support = true;
        caps.battery_support = true;
        return caps;
    }
    DpiRead get_dpi() override {
        DpiRead r;
        r.x = dpi_;
        return r;
    }
    DpiOutcome set_dpi(int value) override {
        const int old = dpi_;
        dpi_ = value;
        DpiOutcome out;
        out.ok = true;
        out.wrote = value;
        out.readback = value;
        out.log.push_back("读旧值 " + std::to_string(old));
        out.log.push_back("写");
        out.log.push_back("读回 " + std::to_string(value));
        out.log.push_back("验证: 通过");
        return out;
    }
    std::optional<int> get_poll_rate() override { return 1000; }
    std::pair<bool, std::string> set_poll_rate(int hz) override {
        poll_rate_ = hz;
        return {true, "已设 " + std::to_string(hz) + "Hz"};
    }
    std::optional<BatteryResult> get_battery() override {
        BatteryResult r;
        r.vidpid = "ABCD:1234";
        r.percent = 70;
        return r;
    }
    std::string brand() const override { return "Fake"; }

    int dpi() const { return dpi_; }              // 测试断言用
    std::optional<int> poll_rate() const { return poll_rate_; }

private:
    int dpi_ = 800;
    std::optional<int> poll_rate_;
};


}  // namespace pt::test
