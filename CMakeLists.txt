# CMakeList.txt: vgui 的 CMake 项目，在此处包括源代码并定义
# 项目特定的逻辑。
#
cmake_minimum_required(VERSION 3.16)

# 如果支持，请为 MSVC 编译器启用热重载。
if(POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<IF:$<AND:$<C_COMPILER_ID:MSVC>,$<CXX_COMPILER_ID:MSVC>>,$<$<CONFIG:Debug,RelWithDebInfo>:EditAndContinue>,$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>>")
endif()

project(vgui VERSION 0.3.0 LANGUAGES C CXX)

set(VGUI_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(VGUI_IS_TOP_LEVEL ON)
else()
    set(VGUI_IS_TOP_LEVEL OFF)
endif()

option(VGUI_BUILD_SAMPLES "Build vgui samples and tests" ${VGUI_IS_TOP_LEVEL})
option(VGUI_BUILD_EDITOR_KIT "Build vgui_editor_kit" ON)
option(VGUI_BUILD_CHART "Build the optional vgui_chart module" ON)
option(VGUI_INSTALL "Install vgui headers, libraries and CMake package files" ${VGUI_IS_TOP_LEVEL})

# E03: 截图回归测试通过 CTest 注册，CI 可直接运行 ctest -R vgui_screenshots。
if(VGUI_IS_TOP_LEVEL)
    enable_testing()
endif()

if(MSVC)
    add_compile_options(/utf-8)
    add_compile_options(/std:c++latest)
else()
    add_compile_options(-finput-charset=UTF-8)
endif()

IF(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(platform_i)
    set(platform_dir x64)
ELSE()
    set(platform_i 32)
    set(platform_dir x86)
ENDIF()
if(CMAKE_BUILD_TYPE MATCHES "Debug")
    set(platform_dir "${platform_dir}d")
endif()

# 设置 vgui 自身示例/测试的输出目录。作为子目录被引入时不覆盖消费者的
# 输出目录设置。
if(VGUI_IS_TOP_LEVEL)
    set(EXECUTABLE_OUTPUT_PATH ${PROJECT_SOURCE_DIR}/out/bin/${platform_dir})
    set(LIBRARY_OUTPUT_PATH ${EXECUTABLE_OUTPUT_PATH})
    LINK_DIRECTORIES(${EXECUTABLE_OUTPUT_PATH})
endif()

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# ---- 查找 lib ----
find_package(SDL3 REQUIRED)
find_package(harfbuzz CONFIG REQUIRED)
find_package(Fontconfig REQUIRED)
find_package(PkgConfig REQUIRED)
find_package(glm CONFIG REQUIRED)
if(VGUI_BUILD_EDITOR_KIT OR VGUI_BUILD_SAMPLES OR VGUI_BUILD_CHART)
    find_package(nlohmann_json CONFIG REQUIRED)
endif()
if(NOT TARGET PkgConfig::Clipper2)
    pkg_check_modules(Clipper2 REQUIRED IMPORTED_TARGET Clipper2)
endif()
if(NOT TARGET PkgConfig::Clipper2Z)
    pkg_check_modules(Clipper2Z REQUIRED IMPORTED_TARGET Clipper2Z)
endif()

# 包含子项目。
add_subdirectory(third_party/ovg)

# libovg 由 third_party/ovg 定义；这里补齐供 vgui 构建树和安装包使用的
# 公开 include 目录与导出名。Stage B 起 vgui 的 SDL3 后端公共头会直接
# 暴露 OVG 类型，因此 OVG 公共头也随 vgui 一起安装。
add_library(vgui::ovg ALIAS libovg)
set_target_properties(libovg PROPERTIES EXPORT_NAME ovg)
target_include_directories(libovg PUBLIC
    $<BUILD_INTERFACE:${VGUI_SOURCE_DIR}/third_party/ovg/ovg>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

add_subdirectory(backends)

# 公共头包含 UTF-8 中文注释；把 /utf-8 传给消费者，避免 MSVC 在非 UTF-8
# 代码页下按本地编码解析头文件。C 和 C++ 目标都接受该选项。
if(MSVC)
    target_compile_options(libvgui INTERFACE /utf-8)
    target_compile_options(libovg INTERFACE /utf-8)
endif()

if(VGUI_BUILD_CHART)
    add_subdirectory(gui/chart)
endif()
if(VGUI_BUILD_EDITOR_KIT)
    add_subdirectory(gui/editor)
endif()
if(VGUI_BUILD_SAMPLES)
    add_subdirectory(sample)
endif()

if(VGUI_INSTALL)
    install(TARGETS libvgui libovg
        EXPORT vguiTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

    if(VGUI_BUILD_EDITOR_KIT)
        install(TARGETS vgui_editor_kit
            EXPORT vguiTargets
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    endif()

    if(VGUI_BUILD_CHART)
        install(TARGETS vgui_chart
            EXPORT vguiTargets
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    endif()

    install(FILES include/vgui/vgui.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/vgui)

    # Stage B: SDL3 后端公共头（app_mgr / gui_viewport / 渲染器 / 事件类型）。
    install(DIRECTORY include/vgui/backend
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/vgui)

    if(VGUI_BUILD_EDITOR_KIT)
        install(DIRECTORY include/vgui/editor
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/vgui)
    endif()

    if(VGUI_BUILD_CHART)
        install(DIRECTORY include/vgui/chart
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/vgui)
    endif()

    # vgui 的 SDL3 后端公共头暴露 OVG 类型；安装 OVG 的公共 C/C++ 头。
    # ovg_renderer_sdl3.h 不安装：它定义了与 vgui vg_fbo_t 冲突的同名结构。
    install(FILES
        third_party/ovg/ovg/ovg_c.h
        third_party/ovg/ovg/ovg.h
        third_party/ovg/ovg/ovg_fonts.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

    # 旧 include 路径的兼容 shim。editor shim 只在 editor kit 实际构建时安装。
    install(FILES include/vgui.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    if(VGUI_BUILD_EDITOR_KIT)
        install(FILES include/vgui_editor_kit.h
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    endif()

    install(FILES LICENSE.txt
        DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/vgui)

    install(EXPORT vguiTargets
        FILE vguiTargets.cmake
        NAMESPACE vgui::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/vgui)

    configure_package_config_file(
        ${VGUI_SOURCE_DIR}/cmake/vguiConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/vguiConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/vgui)

    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/vguiConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion)

    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/vguiConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/vguiConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/vgui)
endif()
