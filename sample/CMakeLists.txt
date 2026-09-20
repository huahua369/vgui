 

find_package(EnTT CONFIG REQUIRED)

# ---- 源文件 ----
set(SRC
    main.cpp 
    ../third_party/ovg/ovg/ovg_renderer_sdl3.cpp
) 

include_directories(${CMAKE_CURRENT_SOURCE_DIR})
# ---- 可执行文件 ----
add_executable(vgui ${SRC})
target_link_libraries(vgui PRIVATE libovg libvgui EnTT::EnTT ZLIB::ZLIB)

  
