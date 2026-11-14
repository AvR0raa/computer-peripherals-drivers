// main_cli.cpp — M1 设备识别器命令行入口（cli.py 平移）。
//
//   cli list [--json]            列出全部 HID 接口 + 指纹状态
//   cli info <VID:PID> [--desc]  单设备各接口详情（--desc 读描述符）
//   cli watch [--interval 1.0]   热插拔监视（Ctrl+C 退出）
//   cli battery                  全部设备电量扫描
//   cli mouse [--slot N] [--set DPI] [--rate HZ]
//                                罗技鼠标报告（唤醒/DPI/电量）+ 可选设置
//
// 组合 core/ 各模块，自身不含业务逻辑。
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <csignal>
#endif

#include "core/battery.hpp"
#include "core/device_manager.hpp"
#include "core/dpi.hpp"
#include "core/fingerprint.hpp"
#include "core/hid_api.hpp"
#include "core/hidpp.hpp"
#include "core/report_io.hpp"

using namespace pt;

namespace {

std::atomic<bool> g_stop{false};
void on_sigint(int) { g_stop = true; }

// "291D:385D" / "0x291d:0x385d" -> (vid, pid)；非法返回空
std::optional<std::pair<unsigned, unsigned>> parse_vidpid(const std::string& text) {
    std::string lower;
    for (char c : text) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const auto colon = lower.find(':');
    if (colon == std::string::npos || lower.find(':', colon + 1) != std::string::npos)
        return std::nullopt;
    try {
        const unsigned vid = static_cast<unsigned>(std::stoul(lower.substr(0, colon), nullptr, 16));
        const unsigned pid =
            static_cast<unsigned>(std::stoul(lower.substr(colon + 1), nullptr, 16));
        return std::make_pair(vid, pid);
    } catch (...) {
        return std::nullopt;
    }
}

// usage 显示：厂商页标 V 前缀
std::string usage_short(unsigned usage_page, unsigned usage) {
    const char* mark = usage_page >= 0xFF00 ? "V" : " ";
    return hex(usage_page, 4) + ":" + hex(usage, 4) + mark;
}

Json fingerprint_dict(const DeviceInfo& d, const Json& db) {
    const auto result = fingerprint(d, db);
    Json sdk = Json::array();
    for (const auto& s : result.sdk) sdk.push_back(s);
    Json out = Json::object();
    out["status"] = status_value(result.status);
    out["brand"] = result.brand ? Json(*result.brand) : Json();
    out["brand_zh"] = result.brand_zh ? Json(*result.brand_zh) : Json();
    out["sdk"] = sdk;
    out["detail"] = result.detail;
    return out;
}

// ---- list ----

int cmd_list(bool json_flag) {
    const auto db = load_db(default_db_path());
    const auto devices = list_devices();
    if (json_flag) {
        Json arr = Json::array();
        for (const auto& d : devices) {
            Json item = Json::object();
            item["vid"] = d.vid_hex();
            item["pid"] = d.pid_hex();
            item["interface"] = d.interface_number;
            item["product"] = d.product;
            item["manufacturer"] = d.manufacturer;
            item["serial"] = d.serial_number;
            item["usage_page"] = "0x" + hex(d.usage_page, 4);
            item["usage"] = "0x" + hex(d.usage, 4);
            item["path"] = d.path;
            const Json fp = fingerprint_dict(d, db);  // 先落局部：临时对象迭代 = 悬垂
            for (const auto& [k, v] : fp.items()) item[k] = v;
            arr.push_back(std::move(item));
        }
        Json payload = Json::object();
        payload["count"] = devices.size();
        payload["devices"] = arr;
        std::printf("%s\n", payload.dump(2).c_str());
        return 0;
    }

    std::printf("共 %zu 个 HID 接口\n", devices.size());
    std::printf("%-4s %-10s %2s  %-28s %-11s 说明\n", "状态", "VID:PID", "IF", "产品", "UP:USAGE");
    std::printf("%s\n", std::string(96, '-').c_str());
    for (const auto& d : devices) {
        const auto result = fingerprint(d, db);
        const std::string product = d.product.empty() ? "(未命名)" : d.product;
        std::printf("%-4s %s:%-5s %2d  %-28s %-11s %s\n", status_mark(result.status),
                    d.vid_hex().c_str(), d.pid_hex().c_str(), d.interface_number,
                    product.substr(0, 28).c_str(), usage_short(d.usage_page, d.usage).c_str(),
                    result.detail.c_str());
    }
    std::printf("%s\n", std::string(96, '-').c_str());
    std::printf("OK=已知(可直通)  SDK=有官方SDK  ???=未知(可学习)；UP:USAGE 末尾 V=厂商自定义页\n");
    return 0;
}

// ---- info ----

Bytes read_descriptor(const std::string& path) {
    // 打开设备只读描述符；失败返回空（M1 绝不写、不做别的）
    try {
        auto dev = hid::open_path(path);
        return hid::get_report_descriptor(dev.get());
    } catch (...) {
        return {};
    }
}

int cmd_info(const std::string& vidpid_text, bool desc) {
    const auto db = load_db(default_db_path());
    const auto parsed = parse_vidpid(vidpid_text);
    if (!parsed) {
        std::fprintf(stderr, "错误: 格式应为 VID:PID，例如 046D:C54D，收到: %s\n",
                     vidpid_text.c_str());
        return 1;
    }
    const auto vid = parsed->first;
    const auto pid = parsed->second;

    const auto devices = find_devices(vid, pid);
    if (devices.empty()) {
        std::fprintf(stderr, "未找到设备 %s\n", vidpid_text.c_str());
        return 1;
    }

    const auto result = fingerprint(devices[0], db);
    std::printf("设备 %s:%s  品牌: %s  状态: %s  %s\n", hex(vid, 4).c_str(), hex(pid, 4).c_str(),
                (result.brand_zh ? result.brand_zh->c_str()
                                 : (result.brand ? result.brand->c_str() : "?")),
                status_mark(result.status), result.detail.c_str());
    if (!result.entry.is_null())
        std::printf("  条目备注: %s\n",
                    result.entry.value("note", std::string("(无)")).c_str());

    for (const auto& d : devices) {
        std::printf("\n── 接口 %d  usage_page=0x%s usage=0x%s  序列号=%s\n", d.interface_number,
                    hex(d.usage_page, 4).c_str(), hex(d.usage, 4).c_str(),
                    d.serial_number.empty() ? "-" : d.serial_number.c_str());
        if (!desc) continue;
        const Bytes raw = read_descriptor(d.path);
        if (raw.empty()) {
            std::printf("   (描述符读取失败：设备被系统占用或接口不可读)\n");
            continue;
        }
        const auto info = parse_report_descriptor(raw);
        for (const auto& line : summary(info)) std::printf("   %s\n", line.c_str());
    }
    return 0;
}

// ---- watch ----

int cmd_watch(double interval) {
    const auto db = load_db(default_db_path());
    std::printf("热插拔监视中（轮询 %gs）… Ctrl+C 退出\n", interval);
    watch_devices(
        [&](const DeviceEvent& event) {
            const auto& d = event.device;
            const auto result = fingerprint(d, db);
            const char* arrow = event.kind == "added" ? "＋接入" : "－移除";
            std::printf("%s [%s] %s:%s if=%d %s  %s\n", arrow, status_mark(result.status),
                        d.vid_hex().c_str(), d.pid_hex().c_str(), d.interface_number,
                        d.product.empty() ? "(未命名)" : d.product.c_str(),
                        result.detail.c_str());
            std::fflush(stdout);
        },
        interval, &g_stop);
    std::printf("\n已退出\n");
    return 0;
}

// ---- battery ----

int cmd_battery() {
    const auto devices = list_devices();
    std::map<std::string, std::vector<DeviceInfo>> grouped;
    for (const auto& d : devices) grouped[d.vidpid()].push_back(d);

    const auto results = scan_all(grouped, read_descriptor);
    std::printf("%-10s %-26s %-9s 来源/说明\n", "VID:PID", "设备", "电量");
    std::printf("%s\n", std::string(84, '-').c_str());
    for (const auto& r : results) {
        std::string product = r.product;
        if (product.size() > 26) product = product.substr(0, 26);
        std::printf("%-10s %-26s %-9s %s\n", r.vidpid.c_str(), product.c_str(),
                    r.display().c_str(), r.detail.c_str());
    }
    std::printf("%s\n", std::string(84, '-').c_str());
    return 0;
}

// ---- mouse（罗技完整报告）----

int cmd_mouse(int slot, const std::optional<int>& set_dpi_value,
              const std::optional<int>& rate_value) {
    std::vector<DeviceInfo> ifaces;
    for (const auto& d : list_devices())
        if (d.vid_hex() == "046D" && d.pid_hex() == "C54D") ifaces.push_back(d);
    const DeviceInfo* short_if = nullptr;
    const DeviceInfo* long_if = nullptr;
    for (const auto& d : ifaces) {
        // FF00 页才是 HID++ 通道；仅匹配 usage 会误抓鼠标接口（真机 bug 实证）
        if (d.usage_page == 0xFF00 && d.usage == 0x0001) short_if = &d;
        if (d.usage_page == 0xFF00 && d.usage == 0x0002) long_if = &d;
    }
    if (!short_if && !long_if) {
        std::fprintf(stderr, "未找到 C54D 接收器\n");
        return 1;
    }
    HidppLink link(short_if ? short_if->path : std::string(),
                   long_if ? long_if->path : std::string());
    const int dev = slot ? slot : 0xFF;
    const std::string slot_text = dev != 0xFF ? std::to_string(dev) : "0xFF/直连";
    std::printf("连接接收器（槽位 %s）…\n", slot_text.c_str());
    std::printf(">>> 请反复移动并点击鼠标（约 12 秒唤醒窗口）<<<\n");
    link.open();
    struct LinkCloser {
        HidppLink& l;
        ~LinkCloser() { l.close(); }
    } closer{link};

    const auto version = ensure_online(link, dev, 10, 1.2);
    if (!version) {
        std::printf("设备不在线。确认: ①鼠标开着 ②已配对到该接收器 ③正在动鼠标\n");
        return 1;
    }
    char verbuf[32];
    std::snprintf(verbuf, sizeof verbuf, "%.1f", *version);
    std::printf("✅ 在线，HID++ %s\n", verbuf);

    const auto dpi = read_dpi(link, dev);
    if (dpi.x) {
        std::string line = "DPI: " + std::to_string(*dpi.x);
        if (dpi.y) line += "  Y轴: " + std::to_string(*dpi.y);
        if (dpi.lod) line += "  LOD: " + std::to_string(*dpi.lod);
        if (dpi.default_dpi) line += "  默认: " + std::to_string(*dpi.default_dpi);
        std::printf("%s\n", line.c_str());
    } else {
        std::printf("DPI: 读取失败（0x2201/0x2202 均无响应）\n");
    }
    const auto dpi_list = get_dpi_list(link, dev);
    if (!dpi_list.empty())
        std::printf("档位表: %zu 档，%d - %d\n", dpi_list.size(), dpi_list.front(),
                    dpi_list.back());
    const auto rate = read_poll_rate(link, dev);
    const auto rates = get_poll_rates(link, dev);
    if (rate) {
        std::string supported;
        for (std::size_t i = 0; i < rates.size(); ++i) {
            if (i) supported += ", ";
            supported += std::to_string(rates[i]);
        }
        std::printf("报告率: %dHz%s\n", *rate,
                    rates.empty() ? "" : ("（支持 " + supported + "）").c_str());
    }
    if (rate_value) {
        std::printf("\n设置报告率 → %dHz …\n", *rate_value);
        const auto [ok, msg] = set_poll_rate(link, dev, *rate_value);
        std::printf("  %s %s\n", ok ? "✅" : "❌", msg.c_str());
        if (!ok) return 1;
    }
    const auto battery = read_battery(link, dev);
    if (battery) {
        const std::string shown = battery->percent
                                      ? std::to_string(*battery->percent)
                                      : battery->detail;
        std::printf("电量: %s %s (%s)\n", shown.c_str(), battery->charging ? "充电中" : "",
                    battery->source.c_str());
    } else {
        std::printf("电量: 无电量 feature\n");
    }
    if (set_dpi_value) {
        std::printf("\n设置 DPI → %d（带读回验证+回滚）…\n", *set_dpi_value);
        const auto outcome = pt::set_dpi(link, dev, *set_dpi_value);
        for (const auto& line : outcome.log) std::printf("  %s\n", line.c_str());
        return outcome.ok ? 0 : 1;
    }
    return 0;
}

// ---- 参数解析与入口 ----

void print_usage() {
    std::printf(
        "M1 设备识别器：枚举/识别/监视 HID 外设（只读）\n\n"
        "用法: cli <命令> [参数]\n\n"
        "  list [--json]              列出全部 HID 接口 + 指纹状态\n"
        "  info <VID:PID> [--desc]    单设备详情（--desc 读取并解析报告描述符）\n"
        "  watch [--interval 秒]      热插拔监视\n"
        "  battery                    全部设备电量扫描\n"
        "  mouse [--slot N] [--set DPI] [--rate HZ]\n"
        "                             罗技鼠标报告（唤醒/DPI/电量）+ 可选设置\n");
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);  // 对应 Python stdout.reconfigure(encoding="utf-8")
    std::signal(SIGINT, on_sigint);
#endif
    if (argc < 2) {
        print_usage();
        return 1;
    }
    const std::string command = argv[1];
    int result = 1;
    try {
        if (command == "list") {
            bool json_flag = false;
            for (int i = 2; i < argc; ++i)
                if (std::strcmp(argv[i], "--json") == 0) json_flag = true;
            result = cmd_list(json_flag);
        } else if (command == "info") {
            if (argc < 3) {
                print_usage();
                return 1;
            }
            bool desc = false;
            for (int i = 3; i < argc; ++i)
                if (std::strcmp(argv[i], "--desc") == 0) desc = true;
            result = cmd_info(argv[2], desc);
        } else if (command == "watch") {
            double interval = 1.0;
            for (int i = 2; i < argc; ++i)
                if (std::strcmp(argv[i], "--interval") == 0 && i + 1 < argc)
                    interval = std::atof(argv[++i]);
            result = cmd_watch(interval);
        } else if (command == "battery") {
            result = cmd_battery();
        } else if (command == "mouse") {
            int slot = 1;
            std::optional<int> set_val, rate_val;
            for (int i = 2; i < argc; ++i) {
                if (std::strcmp(argv[i], "--slot") == 0 && i + 1 < argc)
                    slot = std::atoi(argv[++i]);
                else if (std::strcmp(argv[i], "--set") == 0 && i + 1 < argc)
                    set_val = std::atoi(argv[++i]);
                else if (std::strcmp(argv[i], "--rate") == 0 && i + 1 < argc)
                    rate_val = std::atoi(argv[++i]);
            }
            result = cmd_mouse(slot, set_val, rate_val);
        } else {
            print_usage();
            return 1;
        }
    } catch (const HIDError& exc) {
        std::fprintf(stderr, "HID 错误: %s\n", exc.what());
        return 1;
    } catch (const std::exception& exc) {  // 顶层兜底：转人话，不留栈给用户
        std::fprintf(stderr, "错误: %s\n", exc.what());
        return 1;
    }
    return result;
}
