set(VGUI_EDITOR_KIT_SOURCES
    asset_browser.cpp
    command_stack.cpp
    editor_context.cpp
    editor_layout.cpp
    editor_shell.cpp
    outliner_view.cpp
    property_grid.cpp
    viewport_view.cpp
)

add_library(vgui_editor_kit STATIC ${VGUI_EDITOR_KIT_SOURCES})

target_include_directories(vgui_editor_kit
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/gui
)

target_link_libraries(vgui_editor_kit PUBLIC libvgui PRIVATE nlohmann_json::nlohmann_json)
