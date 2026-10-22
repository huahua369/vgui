 

find_package(EnTT CONFIG REQUIRED)

# ---- 源文件 ----
set(SRC
    main.cpp 
) 

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
)

include_directories(${CMAKE_CURRENT_SOURCE_DIR})
# ---- 可执行文件 ----
add_executable(vgui ${SRC})
target_link_libraries(vgui PRIVATE libovg libvgui EnTT::EnTT ZLIB::ZLIB)

add_executable(vgui_demo ${SRC_DEMO})
target_link_libraries(vgui_demo PRIVATE libovg libvgui EnTT::EnTT ZLIB::ZLIB)

add_executable(vgui_edit_test ${SRC_EDIT_TEST})
target_link_libraries(vgui_edit_test PRIVATE libovg libvgui EnTT::EnTT ZLIB::ZLIB)

add_executable(vgui_model_test ${SRC_MODEL_TEST})
target_link_libraries(vgui_model_test PRIVATE libovg libvgui EnTT::EnTT ZLIB::ZLIB)

add_executable(vgui_editor ${SRC_UI_EDITOR})
target_link_libraries(vgui_editor PRIVATE libovg libvgui EnTT::EnTT ZLIB::ZLIB)

add_custom_command(
    TARGET vgui POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/sample/res
        $<TARGET_FILE_DIR:vgui>/res
)

add_custom_command(
    TARGET vgui_demo POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/sample/res
        $<TARGET_FILE_DIR:vgui_demo>/res
)
  
