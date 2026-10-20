 


set(OVGUI_SOURCES 
    ../gui/anim.cpp
    ../gui/win_core.cpp
    ../gui/vgui.cpp
    mapView.cpp
    vgui_sdl3.cpp
)
 

add_library(libvgui STATIC ${OVGUI_SOURCES})
# harfbuzz 用于 gui/vgui.cpp 里的真实文本测量（与 ovg 渲染一致）
target_link_libraries(libvgui PUBLIC SDL3::SDL3 glm::glm-header-only PkgConfig::Clipper2 PkgConfig::Clipper2Z harfbuzz::harfbuzz)
