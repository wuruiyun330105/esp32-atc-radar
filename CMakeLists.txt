cmake_minimum_required(VERSION 3.16)
project(esp32_tower_radar C)

set(CMAKE_C_STANDARD 11)

# ---------------------------------------------------------------------------
# LVGL 源码目录
#   默认 third_party/lvgl（由 scripts/setup_lvgl.sh 下载）
#   也可以自己指定：cmake -DLVGL_DIR=/path/to/lvgl-9.3.0 ..
# ---------------------------------------------------------------------------
set(LVGL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/lvgl" CACHE PATH "LVGL 9.x 源码目录")

if(NOT EXISTS "${LVGL_DIR}/src/lvgl.h")
    message(FATAL_ERROR
        "\n找不到 LVGL：${LVGL_DIR}\n"
        "先运行一次下载脚本：\n"
        "    bash scripts/setup_lvgl.sh          # macOS / Linux / WSL\n"
        "    powershell scripts/setup_lvgl.ps1   # Windows\n"
        "或者用 -DLVGL_DIR=你的 LVGL 目录 指定已有副本。\n")
endif()

# 模拟器专用 lv_conf.h（与设备端 device/lv_conf.h 相互独立，互不影响）
set(SIM_LV_CONF "${CMAKE_CURRENT_SOURCE_DIR}/sim/lv_conf.h")
if(NOT EXISTS "${SIM_LV_CONF}")
    message(FATAL_ERROR "缺少 ${SIM_LV_CONF}")
endif()

add_compile_definitions(SIMULATOR=1)
add_compile_definitions(LV_LVGL_H_INCLUDE_SIMPLE=1)

file(GLOB_RECURSE LVGL_SOURCES "${LVGL_DIR}/src/*.c")
file(GLOB APP_UI   "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/*.c")
file(GLOB APP_DATA "${CMAKE_CURRENT_SOURCE_DIR}/src/data/*.c")

add_executable(sim
    src/main_pc.c
    src/app_pages.c
    ${APP_UI}
    ${APP_DATA}
    ${LVGL_SOURCES}
)

# 关键：让 LVGL 用工程内的 sim/lv_conf.h，而不是它自己目录下的那份。
# 这样 LVGL 源码目录可以是一份干净的 clone，不需要往里拷任何文件。
target_compile_definitions(sim PRIVATE LV_CONF_PATH="${SIM_LV_CONF}")

target_include_directories(sim PRIVATE
    ${LVGL_DIR}
    ${LVGL_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui
    ${CMAKE_CURRENT_SOURCE_DIR}/src/data
    ${CMAKE_CURRENT_SOURCE_DIR}/src/port
)

# ---------------------------------------------------------------------------
# SDL2（模拟器窗口 + 鼠标键盘）
#   Ubuntu/Debian : sudo apt install libsdl2-dev
#   macOS         : brew install sdl2
# ---------------------------------------------------------------------------
find_package(SDL2 REQUIRED)

if(TARGET SDL2::SDL2)
    target_link_libraries(sim PRIVATE SDL2::SDL2)
else()
    target_include_directories(sim PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(sim PRIVATE ${SDL2_LIBRARIES})
endif()

find_package(Threads REQUIRED)
target_link_libraries(sim PRIVATE Threads::Threads)
if(NOT WIN32)
    target_link_libraries(sim PRIVATE m)
endif()

if(MSVC)
    target_compile_options(sim PRIVATE /w)
else()
    target_compile_options(sim PRIVATE -w)
endif()

message(STATUS "LVGL   : ${LVGL_DIR}")
message(STATUS "lv_conf: ${SIM_LV_CONF}")
message(STATUS "产物   : ${CMAKE_CURRENT_BINARY_DIR}/sim")
