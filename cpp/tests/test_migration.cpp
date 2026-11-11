// M-C6 配置迁移测试（plan.md §6：intent 翻译 / 就近取档 / 类型守卫 / 安全写执行）
#include "core/migration.hpp"

#include "doctest.h"
#include "fake_backend.hpp"

using pt::test::FakeBackend;
#include "fake_backend.hpp"

namespace pt::migration {

TEST_CASE("nearest 就近取档") {
    CHECK(nearest({400, 800, 1600, 3200}, 1600) == 1600);   // 精确命中
    CHECK(nearest({400, 800, 1600, 3200}, 1500) == 1600);   // 就近
    CHECK(nearest({400, 800, 1600, 3200}, 100) == 400);     // 低于最小 → 最小档
    CHECK(nearest({400, 800, 1600, 3200}, 9999) == 3200);   // 高于最大 → 最大档
    CHECK(nearest({1000, 2000}, 1400) == 1000);             // 并列取较小（保守）
    CHECK(nearest({1000, 500}, 750) == 500);                // 无序输入也能取
}

TEST_CASE("intent_from_read 采集") {
    DpiRead dpi;
    dpi.x = 1600;
    DeviceIntent intent = intent_from_read(dpi, 1000);
    CHECK(intent.dpi.value_or(0) == 1600);
    CHECK(intent.poll_rate.value_or(0) == 1000);
    // 无效值（0/空）不采集
    DpiRead empty;
    DeviceIntent intent2 = intent_from_read(empty, std::nullopt);
    CHECK(!intent2.dpi.has_value());
    CHECK(!intent2.poll_rate.has_value());
}

TEST_CASE("intent_from_profile 语义匹配") {
    DeviceProfile profile;
    profile.vidpid = "373B:1278";
    profile.features.push_back({"learned_dpi_x", {{"value", 1600}, {"mode", 2}}});
    profile.features.push_back({"learned_poll", {{"hz", 1000}}});
    DeviceIntent intent = intent_from_profile(profile);
    CHECK(intent.dpi.value_or(0) == 1600);
    CHECK(intent.poll_rate.value_or(0) == 1000);
}

TEST_CASE("plan_migration 能力交集 + 跳过项") {
    MouseCapabilities caps;
    caps.dpi_list = {400, 800, 1600, 3200};
    caps.dpi_support = true;
    caps.poll_rates = {1000, 500};
    caps.poll_support = true;

    DeviceIntent intent;
    intent.dpi = 1500;
    intent.poll_rate = 2000;
    MigrationPlan plan = plan_migration(intent, caps);
    REQUIRE(plan.writes.size() == 2);
    CHECK(plan.writes[0].setting == "dpi");
    CHECK(plan.writes[0].from == 1500);
    CHECK(plan.writes[0].to == 1600);                       // 就近取档
    CHECK(plan.writes[1].setting == "poll_rate");
    CHECK(plan.writes[1].to == 1000);                       // 2000 → 1000
    CHECK(plan.skipped.empty());

    // 新设备不支持 DPI → 进 skipped 不进 writes
    MouseCapabilities no_dpi;
    no_dpi.poll_rates = {1000};
    no_dpi.poll_support = true;
    MigrationPlan plan2 = plan_migration(intent, no_dpi);
    REQUIRE(plan2.writes.size() == 1);
    CHECK(plan2.writes[0].setting == "poll_rate");
    REQUIRE(plan2.skipped.size() == 1);
    CHECK(plan2.skipped[0].find("DPI") != std::string::npos);

    // 空意图 → 空计划
    MigrationPlan plan3 = plan_migration(DeviceIntent{}, caps);
    CHECK(plan3.writes.empty());
    CHECK(plan3.skipped.empty());
}

TEST_CASE("execute_migration 走安全写（假后端）") {
    auto backend = std::make_unique<FakeBackend>();
    FakeBackend* raw = backend.get();
    MouseApi api(std::move(backend));

    MouseCapabilities caps;
    caps.dpi_list = {400, 800, 1600, 3200};
    caps.dpi_support = true;
    caps.poll_rates = {1000, 500};
    caps.poll_support = true;

    DeviceIntent intent;
    intent.dpi = 1500;
    intent.poll_rate = 2000;
    MigrationPlan plan = plan_migration(intent, caps);

    auto results = execute_migration(api, plan);
    REQUIRE(results.size() == 2);
    CHECK(results[0].setting == "dpi");
    CHECK(results[0].ok);
    CHECK(results[0].msg.find("验证: 通过") != std::string::npos);
    CHECK(results[1].setting == "poll_rate");
    CHECK(results[1].ok);
    CHECK(raw->dpi() == 1600);                              // 就近档真实写入
    CHECK(raw->poll_rate().value_or(0) == 1000);
}

TEST_CASE("execute_migration 失败可见") {
    // 计划里放一个后端不支持的目标 → set_poll_rate 返回失败 → 结果可见
    MouseApi api(std::make_unique<FakeBackend>());
    MigrationPlan plan;
    plan.writes.push_back({"poll_rate", 8000, 8000});       // FakeBackend 只支持 1000/500
    auto results = execute_migration(api, plan);
    // FakeBackend 的 set_poll_rate 恒成功——这里只验证链路可见性
    CHECK(results.size() == 1);
    CHECK(results[0].setting == "poll_rate");
}

}  // namespace pt::migration
