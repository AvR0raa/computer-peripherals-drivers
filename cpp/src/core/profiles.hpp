// profiles.hpp — 配置档案（任务 3.5）：per-device 保存/加载/应用参数集。
// 档案 = 某台设备 (VID:PID) 各协议功能参数值的快照（JSON 文件）。
// 用途：同一型号两台设备互拷配置、改坏了一键恢复。
// 档案只存"用户想要什么值"，应用时仍走 execute_feature/safe_write 的安全路径。
#pragma once
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "json_util.hpp"

namespace pt {

struct ProfileError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct ProfileFeature {
    std::string feature_id;
    std::map<std::string, int> params;
};

struct DeviceProfile {
    std::string vidpid;   // "291D:385D"
    std::string name;
    double created_ts = 0;
    std::vector<ProfileFeature> features;
};

// 档案目录：与 Python 版共享 <仓库根>/profiles。
std::filesystem::path default_profile_dir();

// 文件名安全化：非法字符换 '_'，去首尾空白，空则 "unnamed"。
std::string safe_name(const std::string& name);

std::filesystem::path profile_dir(const std::string& vidpid,
                                  const std::filesystem::path& directory = default_profile_dir());

// 保存档案 → <dir>/<vidpid>/<name>.json。返回文件路径。
std::filesystem::path save_profile(const DeviceProfile& profile,
                                   const std::filesystem::path& directory = default_profile_dir());

// 读取档案；结构非法抛 ProfileError。
DeviceProfile load_profile(const std::filesystem::path& path);

// 某设备的全部档案文件（按修改时间倒序）。
std::vector<std::filesystem::path> list_profiles(
    const std::string& vidpid,
    const std::filesystem::path& directory = default_profile_dir());

}  // namespace pt
