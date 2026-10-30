set(VGUI_EDITOR_KIT_SOURCES
    asset_browser.cpp
    asset_preview.cpp
    breadcrumb.cpp
    canvas_view.cpp
    code_editor.cpp
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
    markdown_lex.cpp
    markdown_view.cpp
    node_graph_view.cpp
    outliner_view.cpp
    property_grid.cpp
    search_box.cpp
    spinbox.cpp
    status_bar.cpp
    table_view.cpp
    thumbnail_cache.cpp
    timeline_view.cpp
    toolbar.cpp
    vector_editor.cpp
    viewport_camera.cpp
    viewport_gizmo.cpp
    viewport_grid.cpp
    viewport_selection.cpp
    viewport_view.cpp
)

add_library(vgui_editor_kit STATIC ${VGUI_EDITOR_KIT_SOURCES})
add_library(vgui::editor_kit ALIAS vgui_editor_kit)
set_target_properties(vgui_editor_kit PROPERTIES EXPORT_NAME editor_kit)

target_compile_features(vgui_editor_kit PUBLIC cxx_std_17)

target_include_directories(vgui_editor_kit
    PUBLIC
        $<BUILD_INTERFACE:${VGUI_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${VGUI_SOURCE_DIR}/gui
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${VGUI_SOURCE_DIR}/third_party/ovg/ovg)

target_link_libraries(vgui_editor_kit
    PUBLIC vgui::vgui
    PRIVATE nlohmann_json::nlohmann_json)
