cmake_minimum_required(VERSION 3.22)

project(GVFG_BootCycleTest VERSION 1.3.5 LANGUAGES CXX)

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
    source/flow_monitor.cpp
    source/flow_monitor.h
    source/flow_items_dialog.cpp
    source/flow_items_dialog.h
    source/app_scanner.cpp
    source/app_scanner.h
    source/startup_manager.cpp
    source/startup_manager.h
    source/relay_controller.cpp
    source/relay_agent.cpp
    source/relay_controller.h
    source/BootCycleTest.cpp
    source/boot_flow_runner.cpp
    source/boot_flow_runner.h
    source/flow_model.cpp
    source/flow_model.h
    source/flow_runner.cpp
    source/flow_runner.h
    source/flow_dialog.cpp
    source/flow_dialog.h
    source/flow_dialog.ui
    source/region_dialog.ui
    source/flow_samples.cpp
    source/flow_samples.h
    source/offline_capture.cpp
    source/offline_capture.h
    source/window_capture.cpp
    source/window_capture.h
    source/video_reference.cpp
    source/video_reference.h
    source/image_checks.cpp
    source/image_checks.h
    source/easy_plan.cpp
    source/easy_plan.h
    source/easy_builder.cpp
    source/easy_builder.h
    source/easy_builder.ui
    source/ocr_reader.cpp
    source/ocr_reader.h
    source/text_check.cpp
    source/text_check.h
    source/text_check.ui
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
    setupapi advapi32
    d3d11 dxgi windowsapp mfplat mfreadwrite mfuuid propsys uiautomationcore ole32 oleaut32 shell32 user32 uuid dwmapi
)
set_target_properties(GVFG_BootCycleTest PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin"
)

qt_add_executable(GVFG_RelayAgent WIN32
    source/relay_agent_main.cpp
    source/relay_agent.cpp
    source/relay_controller.cpp
)
target_compile_definitions(GVFG_RelayAgent PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(GVFG_RelayAgent PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/W4> $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_link_libraries(GVFG_RelayAgent PRIVATE Qt6::Widgets Qt6::Network setupapi advapi32)
set_target_properties(GVFG_RelayAgent PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin")

# Vendor resources belong outside disposable build/output directories.
# Make Qt Creator's manually built executable usable with OCR as well.
add_custom_command(TARGET GVFG_BootCycleTest POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ocr"
        "$<TARGET_FILE_DIR:GVFG_BootCycleTest>/ocr"
    VERBATIM
)

# Keep .ui edits visible even with localized compiler dependency diagnostics.
foreach(view mainwindow flow_dialog easy_builder text_check)
    set_property(SOURCE source/${view}.cpp APPEND PROPERTY OBJECT_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/source/${view}.ui")
endforeach()
set_property(SOURCE source/flow_dialog.cpp APPEND PROPERTY OBJECT_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/source/region_dialog.ui")

# Qt Creator can invoke localized MSVC with an encoding different from Ninja's
# cached /showIncludes prefix. Keep project-header dependencies explicit so an
# ABI-changing header can never leave an old runner object linked into a new UI.
file(GLOB project_headers CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/source/*.h")
get_target_property(project_sources GVFG_BootCycleTest SOURCES)
foreach(source IN LISTS project_sources)
    if(source MATCHES "\\.cpp$" AND NOT source MATCHES "^\\$<")
        set_property(SOURCE "${source}" APPEND PROPERTY OBJECT_DEPENDS ${project_headers})
    endif()
endforeach()
set_property(SOURCE "${CMAKE_CURRENT_BINARY_DIR}/GVFG_BootCycleTest_autogen/mocs_compilation.cpp"
    APPEND PROPERTY OBJECT_DEPENDS ${project_headers})

# Install only deployable files, never local settings, markers or test records.
include(GNUInstallDirs)
install(TARGETS GVFG_BootCycleTest GVFG_RelayAgent RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
qt_generate_deploy_app_script(
    TARGET GVFG_BootCycleTest
    OUTPUT_SCRIPT deploy_script
    NO_TRANSLATIONS
)
install(SCRIPT ${deploy_script})
if(MSVC)
    # windeployqt ships the installer; also place the CRT beside both EXEs.
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION "${CMAKE_INSTALL_BINDIR}")
    include(InstallRequiredSystemLibraries)
    foreach(runtime IN ITEMS msvcp140.dll msvcp140_1.dll vcruntime140.dll vcruntime140_1.dll)
        if(NOT "${MSVC_CRT_DIR}/${runtime}" IN_LIST CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
            install(CODE "message(FATAL_ERROR \"Missing x64 MSVC redistributable DLL: ${runtime}\")")
        endif()
    endforeach()
endif()
install(FILES examples/sample_capture_card_test.json DESTINATION examples)
install(DIRECTORY DESTINATION dut)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/dut/" DESTINATION dut OPTIONAL)
install(FILES test_media/capture_card_test_1080p60.mp4 DESTINATION test_media)
install(FILES DISTRIBUTION.md DESTINATION . RENAME README.md)
install(FILES third_party/ch340/CH341SER.EXE DESTINATION drivers/CH340)
install(CODE "if(NOT EXISTS \"${CMAKE_CURRENT_SOURCE_DIR}/third_party/ocr/tesseract.exe\")\n message(FATAL_ERROR \"Missing portable OCR runtime: third_party/ocr/tesseract.exe\")\nendif()")
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ocr" DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/distribution/licenses" DESTINATION .)

set(CPACK_GENERATOR ZIP)
set(CPACK_PACKAGE_NAME GVFG_BootCycleTest)
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_FILE_NAME "GVFG_BootCycleTest-${PROJECT_VERSION}-Windows-x64")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
include(CPack)
