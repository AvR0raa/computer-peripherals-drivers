// main.cpp — doctest 测试入口（所有测试编译进 pt_tests 可执行文件）。
#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest.h"

int main(int argc, char** argv) {
    doctest::Context ctx(argc, argv);
    return ctx.run();
}
