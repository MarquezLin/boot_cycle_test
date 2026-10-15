cmake_minimum_required(VERSION 3.22)

project(GVFG_BootCycleTest VERSION 1.0.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "GVFG_BootCycleTest requires Windows.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets Network)

# Repair CMake's known Big5/UTF-8 mis-decoding of Traditional Chinese cl
# diagnostics. This only normalizes cached text; it never runs a compiler.
if(MSVC AND CMAKE_GENERATOR MATCHES "Ninja" AND CMAKE_CL_SHOWINCLUDES_PREFIX MATCHES "^瘜冽")
    set(CMAKE_CL_SHOWINCLUDES_PREFIX "注意: 包含檔案: ")
endif()

qt_add_executable(GVFG_BootCycleTest WIN32
    source/main.cpp
    source/mainwindow.cpp
    source/mainwindow.h
    source/mainwindow.ui
    source/startup_manager.cpp
    source/startup_manager.h
    source/BootCycleTest.cpp
)

target_compile_definitions(GVFG_BootCycleTest PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
)
target_compile_options(GVFG_BootCycleTest PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
    $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
)
target_link_libraries(GVFG_BootCycleTest PRIVATE
    Qt6::Widgets
    Qt6::Network
    uiautomationcore ole32 oleaut32 shell32 user32 uuid
)
set_target_properties(GVFG_BootCycleTest PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin"
)

# Install only deployable files, never local settings, markers or test records.
include(GNUInstallDirs)
install(TARGETS GVFG_BootCycleTest RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
qt_generate_deploy_app_script(
    TARGET GVFG_BootCycleTest
    OUTPUT_SCRIPT deploy_script
    NO_TRANSLATIONS
)
install(SCRIPT ${deploy_script})
install(FILES DISTRIBUTION.md DESTINATION . RENAME README.md)

set(CPACK_GENERATOR ZIP)
set(CPACK_PACKAGE_NAME GVFG_BootCycleTest)
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_FILE_NAME "GVFG_BootCycleTest-${PROJECT_VERSION}-Windows-x64")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
include(CPack)

