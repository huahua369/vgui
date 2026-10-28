 


set(OVGUI_SOURCES 
    ../gui/anim.cpp
    ../gui/win_core.cpp
    ../gui/vgui.cpp
    mapView.cpp
    vgui_sdl3.cpp
    vgui_renderer_sdl3.cpp
)
 

add_library(libvgui STATIC ${OVGUI_SOURCES})
# Text measurement and font metrics now live in libovg (ovg_fonts.cpp), so
# libvgui no longer needs a direct HarfBuzz dependency.
target_link_libraries(libvgui PUBLIC SDL3::SDL3 glm::glm-header-only PkgConfig::Clipper2 PkgConfig::Clipper2Z)
