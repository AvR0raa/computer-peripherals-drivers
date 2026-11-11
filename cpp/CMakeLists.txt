# PeripheralToolCpp — Python 版 PeripheralTool 的 C++17 重构
# 布局: src/core（协议引擎，无 GUI 依赖） | src/app（cli/gui 入口） | tests（doctest）
cmake_minimum_required(VERSION 3.20)
project(PeripheralToolCpp LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")  # 静态 CRT，便于单 exe 分发

if(MSVC)
  # NOMINMAX: common.hpp 引入 <windows.h>，防 min/max 宏污染 nlohmann/Qt
  # （不能用 WIN32_LEAN_AND_MEAN：hidapi 需要完整 windows.h 的 IOCTL/DevPKey 定义）
  add_compile_options(/permissive- /utf-8 /EHsc /D_CRT_SECURE_NO_WARNINGS /DNOMINMAX)
endif()

# ---- hidapi：设备 I/O 唯一后端（与 Python 版同一个 C 库） ----
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(HIDAPI_BUILD_HIDTEST OFF CACHE BOOL "" FORCE)
set(HIDAPI_BUILD_HIDAPI_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/hidapi/src hidapi-build EXCLUDE_FROM_ALL)

# ---- doctest（单头文件测试框架） ----
add_library(doctest_lib INTERFACE)
target_include_directories(doctest_lib INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/doctest)

# ---- pt_core：与 Python core/ 一一对应的协议引擎 ----
file(GLOB PT_CORE_SRC CONFIGURE_DEPENDS src/core/*.cpp)
add_library(pt_core STATIC ${PT_CORE_SRC})
target_include_directories(pt_core PUBLIC src)
target_include_directories(pt_core SYSTEM PUBLIC third_party)
target_compile_definitions(pt_core PRIVATE PT_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(pt_core PUBLIC hidapi::hidapi)
if(MSVC)
  target_compile_options(pt_core PRIVATE /W4)
endif()

# ---- cli（main_cli.cpp 暂缺时跳过，便于分步移植） ----
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/app/main_cli.cpp)
  add_executable(cli src/app/main_cli.cpp)
  target_link_libraries(cli PRIVATE pt_core)
  if(MSVC)
    target_compile_options(cli PRIVATE /W4)
  endif()
endif()

# ---- gui（Qt6 Widgets，找到 Qt 才启用；路径用 -DQt6_DIR=... 或默认 C:/Qt 探测） ----
file(GLOB QT_CMAKE_CONFIG
     "${CMAKE_CURRENT_SOURCE_DIR}/../Qt/*/msvc*/lib/cmake/Qt6"
     "C:/Qt/*/msvc*/lib/cmake/Qt6")
if(QT_CMAKE_CONFIG)
  list(GET QT_CMAKE_CONFIG 0 QT6_DIR_HINT)
  find_package(Qt6 COMPONENTS Widgets HINTS "${QT6_DIR_HINT}")
endif()
if(Qt6_FOUND)
  message(STATUS "Qt6 found: ${Qt6_DIR}")
  set(CMAKE_AUTOMOC ON)
  add_executable(gui WIN32
                 src/app/main_gui.cpp
                 src/gui/gui_helpers.cpp
                 src/gui/workers.cpp
                 src/gui/dpi_dialog.cpp
                 src/gui/main_window.cpp
                 src/gui/advanced_window.cpp
                 src/gui/advanced_window2.cpp
                 src/gui/tray.cpp
                 src/gui/migration_dialog.cpp)
  set_target_properties(gui PROPERTIES OUTPUT_NAME PeripheralTool)
  target_include_directories(gui PRIVATE src src/gui)
  target_link_libraries(gui PRIVATE pt_core Qt6::Widgets)
  if(MSVC)
    target_compile_options(gui PRIVATE /W4)
  endif()
else()
  message(STATUS "Qt6 not found — gui target skipped (set CMAKE_PREFIX_PATH to Qt)")
endif()

# ---- pt_tests：tests/test_*.cpp 全部编进一个测试可执行文件 ----
file(GLOB PT_TEST_SRC CONFIGURE_DEPENDS tests/test_*.cpp)
add_executable(pt_tests tests/main.cpp ${PT_TEST_SRC})
target_link_libraries(pt_tests PRIVATE pt_core doctest_lib)
if(MSVC)
  target_compile_options(pt_tests PRIVATE /W4)
endif()
