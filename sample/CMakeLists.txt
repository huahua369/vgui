find_package(EnTT CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(zlib CONFIG REQUIRED)

# vgui C API 控件演示（独立目标，不影响原示例）
set(SRC_DEMO
    vgui_demo.cpp
)

# 编辑框行为自测（无需窗口/GPU，直接跑逻辑）
set(SRC_EDIT_TEST
    vgui_edit_test.cpp
)

# 列表/树/菜单模型自测（无需窗口/GPU，直接跑逻辑）
set(SRC_MODEL_TEST
    vgui_model_test.cpp
)

# UI 编辑器（文档 JSON + C++ 代码生成 + 实时预览）
set(SRC_UI_EDITOR
    ui_editor_main.cpp
    ../gui/ui_editor.cpp
)

# vgui_editor_kit MVP 骨架自测（无需窗口/GPU）
set(SRC_EDITOR_KIT_TEST
    vgui_editor_kit_test.cpp
)

# 游戏编辑器 dogfood 示例（editor_shell + M3 Outliner 全流程）
set(SRC_GAME_EDITOR
    vgui_game_editor.cpp
    phase0_renderer.cpp
)

# Phase 0: 3D FBO -> native texture -> VGUI_IMAGE
set(SRC_PHASE0
    vgui_phase0.cpp
    phase0_renderer.cpp
)

# OVG 原生纹理离屏读回测试（方向/裁剪/tint/flip/九宫格/旋转/矩阵/clip/update/fini）
set(SRC_OVG_TEXTURE_TEST
    ovg_texture_test.cpp
)

# OVG tools.hpp 常用图形离屏像素断言测试（基础形状/渐变/虚线/符号/编辑器组件/取色器）
set(SRC_OVG_TOOLS_TEST
    ovg_tools_test.cpp
)

# A08 OVGS 图片节点资源解析/上传缓存测试（mock ovg_ctx_cb，无需 GPU）
set(SRC_OVG_SCENE_JSON_TEST
    ovg_scene_json_test.cpp
)

# vgui 原生纹理离屏读回测试（create/crop/tint/flip/update/release）
set(SRC_VGUI_TEXTURE_TEST
    vgui_texture_test.cpp
)

# E03 截图回归测试：离屏渲染编辑器关键状态，与基线 PNG 容差对比
set(SRC_SCREENSHOT_TEST
    vgui_screenshot_test.cpp
)

# C API 头文件/链接冒烟测试（以 C 编译）
set(SRC_C_API_TEST
    vgui_c_api_test.c
)

# sample 需要的私有头文件（内部 pch/editor 头、SDL3 backend 头、OVG 头）。
# 这些路径不会出现在安装后的 vgui 公共接口中。
add_library(vgui_sample_common INTERFACE)
target_include_directories(vgui_sample_common INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${VGUI_SOURCE_DIR}/gui
    ${VGUI_SOURCE_DIR}/backends
    ${VGUI_SOURCE_DIR}/third_party/ovg/ovg)

# ---- 公共头文件自包含检查 ----
# 每个安装的公共头单独生成一个翻译单元，只通过公共 include 目录解析依赖。
# 这样既检查头文件自包含，也保证它们不依赖 gui/、backends/ 或 OVG 私有头。
set(VGUI_PUBLIC_HEADERS
    "${VGUI_SOURCE_DIR}/include/vgui/vgui.h"
    "${VGUI_SOURCE_DIR}/include/vgui.h")
if(VGUI_BUILD_EDITOR_KIT)
    file(GLOB VGUI_PUBLIC_EDITOR_HEADERS CONFIGURE_DEPENDS
        "${VGUI_SOURCE_DIR}/include/vgui/editor/*.h")
    list(APPEND VGUI_PUBLIC_HEADERS
        ${VGUI_PUBLIC_EDITOR_HEADERS}
        "${VGUI_SOURCE_DIR}/include/vgui_editor_kit.h")
endif()

set(VGUI_HEADER_TEST_SOURCES)
foreach(header IN LISTS VGUI_PUBLIC_HEADERS)
    file(RELATIVE_PATH header_rel "${VGUI_SOURCE_DIR}/include" "${header}")
    string(REPLACE "\\" "/" header_rel "${header_rel}")
    string(MAKE_C_IDENTIFIER "${header_rel}" header_id)
    set(header_src "${CMAKE_CURRENT_BINARY_DIR}/header_self_containment/${header_id}.cpp")
    file(WRITE "${header_src}" "#include <${header_rel}>\n")
    list(APPEND VGUI_HEADER_TEST_SOURCES "${header_src}")
endforeach()

add_library(vgui_headers_test OBJECT ${VGUI_HEADER_TEST_SOURCES})
if(VGUI_BUILD_EDITOR_KIT)
    target_link_libraries(vgui_headers_test PRIVATE vgui::editor_kit)
else()
    target_link_libraries(vgui_headers_test PRIVATE vgui::vgui)
endif()

# ---- 可执行文件 ----

add_executable(vgui_demo ${SRC_DEMO})
target_link_libraries(vgui_demo PRIVATE vgui_sample_common vgui::vgui EnTT::EnTT ZLIB::ZLIB)

add_executable(vgui_edit_test ${SRC_EDIT_TEST})
target_link_libraries(vgui_edit_test PRIVATE vgui_sample_common vgui::vgui EnTT::EnTT ZLIB::ZLIB)

add_executable(vgui_model_test ${SRC_MODEL_TEST})
target_link_libraries(vgui_model_test PRIVATE vgui_sample_common vgui::vgui EnTT::EnTT ZLIB::ZLIB)

if(VGUI_BUILD_EDITOR_KIT)
add_executable(vgui_editor ${SRC_UI_EDITOR})
target_link_libraries(vgui_editor PRIVATE vgui_sample_common vgui::editor_kit EnTT::EnTT ZLIB::ZLIB nlohmann_json::nlohmann_json)
target_compile_definitions(vgui_editor PRIVATE VGUI_SOURCE_DIR="${VGUI_SOURCE_DIR}")

add_executable(vgui_editor_kit_test ${SRC_EDITOR_KIT_TEST})
target_link_libraries(vgui_editor_kit_test PRIVATE vgui_sample_common vgui::editor_kit EnTT::EnTT ZLIB::ZLIB)

add_executable(vgui_game_editor ${SRC_GAME_EDITOR})
target_link_libraries(vgui_game_editor PRIVATE vgui_sample_common vgui::editor_kit EnTT::EnTT ZLIB::ZLIB)
endif()

add_executable(vgui_phase0 ${SRC_PHASE0})
target_link_libraries(vgui_phase0 PRIVATE vgui_sample_common vgui::vgui EnTT::EnTT ZLIB::ZLIB)

add_executable(ovg_texture_test ${SRC_OVG_TEXTURE_TEST})
target_link_libraries(ovg_texture_test PRIVATE vgui_sample_common libovg SDL3::SDL3 glm::glm-header-only)

add_executable(ovg_tools_test ${SRC_OVG_TOOLS_TEST})
target_link_libraries(ovg_tools_test PRIVATE vgui_sample_common libovg SDL3::SDL3 glm::glm-header-only)

add_executable(ovg_scene_json_test ${SRC_OVG_SCENE_JSON_TEST})
target_link_libraries(ovg_scene_json_test PRIVATE vgui_sample_common libovg nlohmann_json::nlohmann_json)

add_executable(vgui_texture_test ${SRC_VGUI_TEXTURE_TEST})
target_link_libraries(vgui_texture_test PRIVATE vgui_sample_common vgui::vgui libovg SDL3::SDL3 glm::glm-header-only)

if(VGUI_BUILD_EDITOR_KIT)
add_executable(vgui_screenshot_test ${SRC_SCREENSHOT_TEST})
target_link_libraries(vgui_screenshot_test PRIVATE vgui_sample_common vgui::editor_kit libovg
    SDL3::SDL3 EnTT::EnTT ZLIB::ZLIB glm::glm-header-only)
target_compile_definitions(vgui_screenshot_test PRIVATE
    VGUI_SOURCE_DIR="${VGUI_SOURCE_DIR}")
add_test(NAME vgui_screenshots
    COMMAND vgui_screenshot_test
        --baseline-dir "${VGUI_SOURCE_DIR}/sample/screenshots"
        --out-dir "${CMAKE_BINARY_DIR}/screenshots")
set_tests_properties(vgui_screenshots PROPERTIES SKIP_RETURN_CODE 77)
endif()

add_executable(vgui_c_api_test ${SRC_C_API_TEST})
target_link_libraries(vgui_c_api_test PRIVATE vgui_sample_common vgui::vgui EnTT::EnTT ZLIB::ZLIB)
set_target_properties(vgui_c_api_test PROPERTIES LINKER_LANGUAGE CXX)

add_custom_command(
    TARGET vgui_demo POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${VGUI_SOURCE_DIR}/sample/res
        $<TARGET_FILE_DIR:vgui_demo>/res
)
