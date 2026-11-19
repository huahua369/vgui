# Internal, headless document model for the future UI designer.
add_library(ui_design_model STATIC model.cpp document_io.cpp)
target_include_directories(ui_design_model PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(ui_design_model PUBLIC cxx_std_20)
target_link_libraries(ui_design_model PRIVATE nlohmann_json::nlohmann_json)

# Projection of a design document into an existing VGUI context.
add_library(ui_design_runtime STATIC runtime.cpp)
add_library(vgui::ui_design_runtime ALIAS ui_design_runtime)
target_link_libraries(ui_design_runtime PUBLIC ui_design_model vgui::vgui)

# U3 file-only delivery tooling; an application can verify a package before
# choosing whether to mount it. No VGUI, editor kit, SDL or GPU dependency.
add_library(ui_design_export STATIC delivery.cpp delivery_hash.cpp)
add_library(vgui::ui_design_export ALIAS ui_design_export)
target_link_libraries(ui_design_export PUBLIC ui_design_model PRIVATE nlohmann_json::nlohmann_json)

add_executable(vgui_ui_pack delivery_main.cpp)
target_link_libraries(vgui_ui_pack PRIVATE vgui::ui_design_export)
if(MINGW)
    target_link_options(vgui_ui_pack PRIVATE -municode)
endif()
