 

find_package(EnTT CONFIG REQUIRED)

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

# vgui 原生纹理离屏读回测试（create/crop/tint/flip/update/release）
set(SRC_VGUI_TEXTURE_TEST
    vgui_texture_test.cpp
)

# C API 头文件/链接冒烟测试（以 C 编译）
set(SRC_C_API_TEST
    vgui_c_api_test.c
)

include_directories(${CMAKE_CURRENT_SOURCE_DIR})
# ---- 可执行文件 ----

add_executable(vgui_demo ${SRC_DEMO})
target_link_libraries(vgui_demo PRIVATE libovg libvgui EnTT::EnTT ZLIB::ZLIB)

add_executable(vgui_edit_test ${SRC_EDIT_TEST})
target_link_libraries(vgui_edit_test PRIVATE libovg libvgui EnTT::EnTT ZLIB::ZLIB)

add_executable(vgui_model_test ${SRC_MODEL_TEST})
target_link_libraries(vgui_model_test PRIVATE libovg libvgui EnTT::EnTT ZLIB::ZLIB)

add_executable(vgui_editor ${SRC_UI_EDITOR})
target_link_libraries(vgui_editor PRIVATE vgui_editor_kit libovg libvgui EnTT::EnTT ZLIB::ZLIB nlohmann_json::nlohmann_json)
target_compile_definitions(vgui_editor PRIVATE VGUI_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

add_executable(vgui_editor_kit_test ${SRC_EDITOR_KIT_TEST})
target_link_libraries(vgui_editor_kit_test PRIVATE vgui_editor_kit libovg EnTT::EnTT ZLIB::ZLIB)

add_executable(vgui_game_editor ${SRC_GAME_EDITOR})
target_link_libraries(vgui_game_editor PRIVATE vgui_editor_kit libovg libvgui EnTT::EnTT ZLIB::ZLIB)

add_executable(vgui_phase0 ${SRC_PHASE0})
target_link_libraries(vgui_phase0 PRIVATE libovg libvgui EnTT::EnTT ZLIB::ZLIB)

add_executable(ovg_texture_test ${SRC_OVG_TEXTURE_TEST})
target_link_libraries(ovg_texture_test PRIVATE libovg SDL3::SDL3 glm::glm-header-only)

add_executable(ovg_tools_test ${SRC_OVG_TOOLS_TEST})
target_link_libraries(ovg_tools_test PRIVATE libovg SDL3::SDL3 glm::glm-header-only)

add_executable(vgui_texture_test ${SRC_VGUI_TEXTURE_TEST})
target_link_libraries(vgui_texture_test PRIVATE libovg libvgui SDL3::SDL3 glm::glm-header-only)

add_executable(vgui_c_api_test ${SRC_C_API_TEST})
target_link_libraries(vgui_c_api_test PRIVATE libovg libvgui EnTT::EnTT ZLIB::ZLIB)
set_target_properties(vgui_c_api_test PROPERTIES LINKER_LANGUAGE CXX)

add_custom_command(
    TARGET vgui_demo POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/sample/res
        $<TARGET_FILE_DIR:vgui_demo>/res
)
  
