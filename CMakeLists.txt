cmake_minimum_required(VERSION 3.19)
project(ida_workbench VERSION 1.3.0 LANGUAGES CXX)

# The app version -> generated header (src/version.h in the build dir), shown in
# Settings ▸ About and the window footer. project(VERSION) above is the default, used by
# every local build; CI overrides it with the tag it is releasing, so a downloaded exe
# always reports the version of the release it came in — no file edit per release.
set(IDA_WORKBENCH_VERSION "${PROJECT_VERSION}" CACHE STRING "Version the app reports")
configure_file(src/version.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/version.h" @ONLY)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
# No .ui files: AUTOUIC off so headers named ui_*.h (e.g. ui_util.h) are treated
# as ordinary includes, not as uic-generated output.
set(CMAKE_AUTOUIC OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)   # feeds IntelliSense / clangd

# When building against a static vcpkg triplet, also link the static CRT so the
# final exe has zero DLL dependencies beyond the Windows system libraries.
if (DEFINED VCPKG_TARGET_TRIPLET AND VCPKG_TARGET_TRIPLET MATCHES "static")
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Qt6 with the Widgets (GUI) and Network (TCP port checks) modules.
find_package(Qt6 REQUIRED COMPONENTS Widgets Network)
qt_standard_project_setup()

qt_add_executable(ida-workbench
    src/main.cpp
    src/core/manager.cpp
    src/core/logging.cpp
    src/ui/main_window.cpp
    src/ui/settings_panel.cpp
    src/ui/delegates.cpp
    src/ui/theme.cpp
    src/ui/ui_util.cpp
)

# Headers are included flat ("manager.h", "main_window.h", …); expose the layers.
# The generated/ dir carries the CMake-produced version.h.
target_include_directories(ida-workbench PRIVATE src src/core src/ui "${CMAKE_CURRENT_BINARY_DIR}/generated")

# Compile the stylesheet, fonts, and in-app icon into the binary.
qt_add_resources(ida-workbench "assets"
    PREFIX "/"
    BASE "resources"
    FILES
        resources/styles/material3.qss
        resources/icons/app_nobg.png
        resources/icons/check_light.png
        resources/fonts/symbols.ttf
        resources/fonts/symbols_fill.ttf
        resources/fonts/robotoflex-400.ttf
        resources/fonts/robotoflex-500.ttf
        resources/fonts/robotoflex-600.ttf
        resources/fonts/robotoflex-700.ttf
        resources/fonts/robotomono-400.ttf
        resources/fonts/robotomono-500.ttf
)

# Embed the Python glue so a single copied exe is truly self-contained: the app
# extracts these next to config.json on launch (see ensureHelperScripts) when they
# are not found beside the executable.
qt_add_resources(ida-workbench "scripts"
    PREFIX "/scripts"
    FILES
        start_mcp.py
        analyze_ida.py
        disable_autostart.py
)

# Embed the .ico into the exe itself (taskbar / Explorer icon) via a resource script.
if (WIN32)
    set(APP_ICON_ICO "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/app.ico")
    configure_file(resources/app.rc.in "${CMAKE_CURRENT_BINARY_DIR}/app.rc" @ONLY)
    target_sources(ida-workbench PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/app.rc")
endif()

target_link_libraries(ida-workbench PRIVATE Qt6::Widgets Qt6::Network)
if (WIN32)
    target_link_libraries(ida-workbench PRIVATE dwmapi iphlpapi)
endif()

# GUI subsystem: no console window on Windows (logs go to the in-app panel).
set_target_properties(ida-workbench PROPERTIES WIN32_EXECUTABLE ON)

include(CTest)
if (BUILD_TESTING)
	add_executable(fake-depot tests/fake_depot.cpp)
	target_link_libraries(fake-depot PRIVATE Qt6::Core)
	add_executable(fake-analyzer tests/fake_analyzer.cpp)
	target_link_libraries(fake-analyzer PRIVATE Qt6::Core)
	add_executable(manager-config-tests
        tests/manager_config_tests.cpp
        src/core/manager.cpp
        src/core/logging.cpp
    )
    target_include_directories(manager-config-tests PRIVATE src src/core)
    target_link_libraries(manager-config-tests PRIVATE Qt6::Core Qt6::Network)
	add_test(NAME manager-workspace-revisions COMMAND manager-config-tests $<TARGET_FILE:fake-depot> $<TARGET_FILE:fake-analyzer>)
	add_executable(logging-tests tests/logging_tests.cpp src/core/logging.cpp)
	target_include_directories(logging-tests PRIVATE src src/core)
	target_link_libraries(logging-tests PRIVATE Qt6::Core)
	add_test(NAME shared-log-tail-trimming COMMAND logging-tests)
    if (WIN32)
		target_link_libraries(manager-config-tests PRIVATE iphlpapi)
        get_target_property(QT_CORE_RUNTIME Qt6::Core IMPORTED_LOCATION_RELEASE)
        if (NOT QT_CORE_RUNTIME)
            get_target_property(QT_CORE_RUNTIME Qt6::Core IMPORTED_LOCATION)
        endif()
        get_filename_component(QT_RUNTIME_DIR "${QT_CORE_RUNTIME}" DIRECTORY)
        set_tests_properties(manager-workspace-revisions shared-log-tail-trimming PROPERTIES
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT_RUNTIME_DIR}")
    endif()
endif()
