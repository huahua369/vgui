set(VGUI_EDITOR_KIT_SOURCES
    asset_browser.cpp
    asset_preview.cpp
    breadcrumb.cpp
    canvas_view.cpp
    color_picker.cpp
    command_registry.cpp
    command_stack.cpp
    combo_box.cpp
    console_view.cpp
    curve_editor.cpp
    drag_bridge.cpp
    editor_context.cpp
    editor_layout.cpp
    editor_shell.cpp
    gradient_editor.cpp
    outliner_view.cpp
    property_grid.cpp
    search_box.cpp
    spinbox.cpp
    status_bar.cpp
    table_view.cpp
    thumbnail_cache.cpp
    toolbar.cpp
    vector_editor.cpp
    viewport_camera.cpp
    viewport_gizmo.cpp
    viewport_grid.cpp
    viewport_selection.cpp
    viewport_view.cpp
)

add_library(vgui_editor_kit STATIC ${VGUI_EDITOR_KIT_SOURCES})

target_include_directories(vgui_editor_kit
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/gui
)

target_link_libraries(vgui_editor_kit PUBLIC libvgui PRIVATE nlohmann_json::nlohmann_json)
