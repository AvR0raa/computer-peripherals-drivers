// test_profiles.cpp — 任务 3.5 单元测试（tests/test_profiles.py 平移）。
#include <fstream>
#include <thread>

#include "doctest.h"
#include "core/profiles.hpp"

using namespace pt;

static DeviceProfile make_profile(const std::string& name = "我的配置") {
    DeviceProfile p;
    p.vidpid = "291D:385D";
    p.name = name;
    p.features.push_back(ProfileFeature{"learned_ff21_237", {{"value", 1600}, {"mode", 2}}});
    p.features.push_back(ProfileFeature{"ping", {}});
    return p;
}

TEST_CASE("save/load 往返") {
    const auto dir = std::filesystem::temp_directory_path() / "pt_profiles_test1";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    const auto path = save_profile(make_profile(), dir);
    CHECK(std::filesystem::exists(path));
    CHECK(path.parent_path() == profile_dir("291D:385D", dir));
    const auto loaded = load_profile(path);
    CHECK(loaded.vidpid == "291D:385D");
    CHECK(loaded.name == "我的配置");
    REQUIRE(loaded.features.size() == 2);
    CHECK(loaded.features[0].feature_id == "learned_ff21_237");
    REQUIRE(loaded.features[0].params.size() == 2);
    CHECK(loaded.features[0].params.at("value") == 1600);
    CHECK(loaded.features[0].params.at("mode") == 2);
    CHECK(loaded.features[1].params.empty());
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("文件名安全化") {
    const auto dir = std::filesystem::temp_directory_path() / "pt_profiles_test2";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    const auto path = save_profile(make_profile("坏名字: *\"?.json"), dir);
    const std::string name = path.filename().string();
    CHECK(name.find(':') == std::string::npos);
    CHECK(name.find('*') == std::string::npos);
    CHECK(path.extension() == ".json");
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("拒绝错误 schema") {
    const auto dir = std::filesystem::temp_directory_path() / "pt_profiles_test3";
    std::error_code ec;
    std::filesystem::create_directories(dir);
    const auto path = dir / "x.json";
    { std::ofstream f(path, std::ios::binary); f << R"({"$schema": "wrong"})"; }
    CHECK_THROWS_AS(load_profile(path), const ProfileError&);
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("拒绝非法 vidpid") {
    const auto dir = std::filesystem::temp_directory_path() / "pt_profiles_test4";
    std::error_code ec;
    std::filesystem::create_directories(dir);
    const auto path = dir / "x.json";
    { std::ofstream f(path, std::ios::binary);
      f << R"({"$schema": "device-profile/v1", "vidpid": "novid"})"; }
    CHECK_THROWS_AS(load_profile(path), const ProfileError&);
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("list_profiles 按修改时间倒序") {
    const auto dir = std::filesystem::temp_directory_path() / "pt_profiles_test5";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    const auto p1 = save_profile(make_profile("a"), dir);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto p2 = save_profile(make_profile("b"), dir);
    const auto files = list_profiles("291D:385D", dir);
    REQUIRE(files.size() == 2);
    CHECK(files[0] == p2);
    CHECK(files[1] == p1);
    CHECK(list_profiles("046D:C54D", dir).empty());  // 其他设备无档案
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("空 params 容忍") {
    const auto dir = std::filesystem::temp_directory_path() / "pt_profiles_test6";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    const auto path = save_profile(make_profile("empty"), dir);
    const auto loaded = load_profile(path);
    CHECK(loaded.features[1].params.empty());
    std::filesystem::remove_all(dir, ec);
}
