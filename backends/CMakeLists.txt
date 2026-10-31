option(VGUI_CORE_ONLY "Build libvgui without the SDL3 backend (engine runtime / headless embedding)" OFF)

# Core: the C widget tree, layout, input dispatch and OVG recording.  No SDL,
# no OS windowing -- hosts drive vgui_update/vgui_draw and consume
# ovg_draw_data_t with their own renderer.  mapView (OS mmap reader) and anim
# are OS-dependent but SDL-free, so they stay in the core list.
set(OVGUI_SOURCES
    ../gui/anim.cpp
    ../gui/vgui.cpp
    mapView.cpp
)

if(NOT VGUI_CORE_ONLY)
    # SDL3 backend: window/input/IME app layer, vgpu 2D GPU renderer and the
    # Windows desktop integration used by it.
    list(APPEND OVGUI_SOURCES
        ../gui/win_core.cpp
        vgui_sdl3.cpp
        vgui_renderer_sdl3.cpp
    )
endif()

add_library(libvgui STATIC ${OVGUI_SOURCES})
add_library(vgui::vgui ALIAS libvgui)
set_target_properties(libvgui PROPERTIES EXPORT_NAME vgui)

target_compile_features(libvgui PUBLIC cxx_std_17)

target_include_directories(libvgui
    PUBLIC
        $<BUILD_INTERFACE:${VGUI_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${VGUI_SOURCE_DIR}/gui
        ${VGUI_SOURCE_DIR}/backends
        ${VGUI_SOURCE_DIR}/third_party/ovg/ovg)

# Text measurement and font metrics now live in libovg (ovg_fonts.cpp), so
# libvgui no longer needs a direct HarfBuzz dependency.  libovg is part of the
# link interface because vgui_sdl3.h / vgui_renderer_sdl3.h expose OVG types.
target_link_libraries(libvgui
    PUBLIC
        glm::glm-header-only
        libovg
    PRIVATE
        PkgConfig::Clipper2
        PkgConfig::Clipper2Z)

if(NOT VGUI_CORE_ONLY)
    target_link_libraries(libvgui PUBLIC SDL3::SDL3)
endif()
