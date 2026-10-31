set(VGUI_CHART_SOURCES
    chart_api.cpp
    chart_dashboard.cpp
    chart_draw.cpp
    chart_event.cpp
    chart_export.cpp
    chart_layout.cpp
    chart_png.cpp
    chart_util.cpp
    gauge_core.cpp
)

add_library(vgui_chart STATIC ${VGUI_CHART_SOURCES})
add_library(vgui::chart ALIAS vgui_chart)
set_target_properties(vgui_chart PROPERTIES EXPORT_NAME chart)

target_compile_features(vgui_chart PUBLIC cxx_std_17)

target_include_directories(vgui_chart
    PUBLIC
        $<BUILD_INTERFACE:${VGUI_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${VGUI_SOURCE_DIR}/gui
        ${VGUI_SOURCE_DIR}/third_party/ovg/ovg)

target_link_libraries(vgui_chart
    PUBLIC
        vgui::vgui
    PRIVATE
        nlohmann_json::nlohmann_json)

if(MSVC)
    target_compile_options(vgui_chart PRIVATE /utf-8)
endif()
