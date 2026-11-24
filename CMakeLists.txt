cmake_minimum_required(VERSION 4.5)
project(hyprworks VERSION 0.4.0 LANGUAGES CXX)

set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CMakeDependentOption)

option(BUILD_SHARED_LIBS "Build shared libraries" ${PROJECT_IS_TOP_LEVEL})
option(HYPRWORKS_BUILD_LUA "Build the Lua authoring library and runtime" ${PROJECT_IS_TOP_LEVEL})
option(HYPRWORKS_BUILD_GUI "Enable the Wayland/Cairo GUI" ${PROJECT_IS_TOP_LEVEL})
option(HYPRWORKS_BUILD_SHORTCUTS "Enable native Hyprland global shortcuts through Wayland" ${PROJECT_IS_TOP_LEVEL})
option(HYPRWORKS_BUILD_UPOWER "Enable native UPower observation through sd-bus" ${PROJECT_IS_TOP_LEVEL})
option(HYPRWORKS_BUILD_DBUS "Enable native D-Bus calls and services through sd-bus" ${PROJECT_IS_TOP_LEVEL})
option(HYPRWORKS_BUILD_MPRIS "Enable native MPRIS media control through sd-bus" ${PROJECT_IS_TOP_LEVEL})
option(HYPRWORKS_BUILD_NOTIFICATIONS "Enable native desktop notification serving through sd-bus" ${PROJECT_IS_TOP_LEVEL})
option(HYPRWORKS_BUILD_TRAY "Enable native StatusNotifier and D-Bus menus through sd-bus" ${PROJECT_IS_TOP_LEVEL})
option(HYPRWORKS_BUILD_DESKTOP "Enable XDG desktop entries through GIO" ${PROJECT_IS_TOP_LEVEL})
option(HYPRWORKS_BUILD_AUDIO "Enable native audio through libpulse" ${PROJECT_IS_TOP_LEVEL})
option(HYPRWORKS_BUILD_POLKIT "Enable native Polkit authentication through libpolkit-agent" ${PROJECT_IS_TOP_LEVEL})
cmake_dependent_option(HYPRWORKS_BUILD_PORTAL "Enable ScreenCast portal serving and PipeWire streaming"
    ${PROJECT_IS_TOP_LEVEL} "HYPRWORKS_BUILD_GUI;HYPRWORKS_BUILD_LUA;HYPRWORKS_BUILD_DBUS" OFF)
option(HYPRWORKS_BUILD_TESTS "Build core lifecycle and concurrency checks" OFF)

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
set(HYPRWORKS_NATIVE_DEPS json-c>=0.19)
if(HYPRWORKS_BUILD_SHORTCUTS)
    list(APPEND HYPRWORKS_NATIVE_DEPS wayland-client>=1.26.0)
endif()
if(HYPRWORKS_BUILD_DBUS OR HYPRWORKS_BUILD_UPOWER OR HYPRWORKS_BUILD_MPRIS OR HYPRWORKS_BUILD_NOTIFICATIONS OR HYPRWORKS_BUILD_TRAY OR HYPRWORKS_BUILD_POLKIT)
    list(APPEND HYPRWORKS_NATIVE_DEPS libsystemd>=262)
endif()
if(HYPRWORKS_BUILD_DESKTOP)
    list(APPEND HYPRWORKS_NATIVE_DEPS gio-unix-2.0>=2.90.1)
endif()
if(HYPRWORKS_BUILD_AUDIO)
    list(APPEND HYPRWORKS_NATIVE_DEPS libpulse>=17.0)
endif()
if(HYPRWORKS_BUILD_POLKIT)
    list(APPEND HYPRWORKS_NATIVE_DEPS polkit-agent-1>=128)
endif()

add_library(hyprworks src/native/hyprland/workspaces.cpp src/native/hyprland/protocol.cpp src/native/host.cpp)
add_library(hyprworks::hyprworks ALIAS hyprworks)
target_compile_features(hyprworks PUBLIC cxx_std_23)
set_target_properties(hyprworks PROPERTIES CXX_EXTENSIONS OFF VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR})
target_include_directories(hyprworks PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
foreach(dependency IN LISTS HYPRWORKS_NATIVE_DEPS)
    string(REGEX REPLACE "[<>=].*" "" module "${dependency}")
    pkg_check_modules(HYPRWORKS_${module} REQUIRED IMPORTED_TARGET "${dependency}")
    target_link_libraries(hyprworks PRIVATE PkgConfig::HYPRWORKS_${module})
endforeach()
target_compile_options(hyprworks PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
target_link_libraries(hyprworks PRIVATE Threads::Threads)

if(HYPRWORKS_BUILD_SHORTCUTS OR (HYPRWORKS_BUILD_LUA AND HYPRWORKS_BUILD_GUI))
    enable_language(C)
    pkg_check_modules(WAYLAND_SCANNER REQUIRED wayland-scanner>=1.26.0)
    pkg_get_variable(WAYLAND_SCANNER_EXECUTABLE wayland-scanner wayland_scanner)
endif()
if(HYPRWORKS_BUILD_SHORTCUTS)
    set(SHORTCUT_PROTOCOL "${CMAKE_CURRENT_SOURCE_DIR}/protocols/hyprland-global-shortcuts-v1.xml")
    add_custom_command(OUTPUT hyprland-global-shortcuts-v1-client.h hyprland-global-shortcuts-v1-protocol.c
        COMMAND "${WAYLAND_SCANNER_EXECUTABLE}" client-header "${SHORTCUT_PROTOCOL}" hyprland-global-shortcuts-v1-client.h
        COMMAND "${WAYLAND_SCANNER_EXECUTABLE}" private-code "${SHORTCUT_PROTOCOL}" hyprland-global-shortcuts-v1-protocol.c
        DEPENDS "${SHORTCUT_PROTOCOL}" VERBATIM)
    target_sources(hyprworks PRIVATE src/native/hyprland/shortcuts.cpp "${CMAKE_CURRENT_BINARY_DIR}/hyprland-global-shortcuts-v1-protocol.c")
    target_include_directories(hyprworks PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    target_compile_definitions(hyprworks PUBLIC HYPRWORKS_HAS_SHORTCUTS)
endif()

if(HYPRWORKS_BUILD_TRAY)
    target_sources(hyprworks PRIVATE src/native/dbusmenu.cpp)
endif()

if(HYPRWORKS_BUILD_LUA)
    set(HYPRWORKS_LUA_DEPS lua>=5.5.1)
    if(HYPRWORKS_BUILD_GUI)
        list(APPEND HYPRWORKS_LUA_DEPS wayland-client>=1.26.0 xkbcommon>=1.13.2 cairo pangocairo libpng librsvg-2.0)
    endif()
    if(HYPRWORKS_BUILD_PORTAL)
        list(APPEND HYPRWORKS_LUA_DEPS libpipewire-0.3>=1.4)
    endif()
    add_library(hyprworks-lua
        src/runtime/invocation.cpp src/lua/host.cpp src/lua/hyprland/workspaces.cpp)
    add_library(hyprworks::lua ALIAS hyprworks-lua)
    target_link_libraries(hyprworks-lua PUBLIC hyprworks::hyprworks)
    foreach(dependency IN LISTS HYPRWORKS_LUA_DEPS)
        string(REGEX REPLACE "[<>=].*" "" module "${dependency}")
        pkg_check_modules(HYPRWORKS_${module} REQUIRED IMPORTED_TARGET "${dependency}")
        list(APPEND HYPRWORKS_LUA_LIBRARIES PkgConfig::HYPRWORKS_${module})
    endforeach()
    target_link_libraries(hyprworks-lua PRIVATE ${HYPRWORKS_LUA_LIBRARIES})
    set_target_properties(hyprworks-lua PROPERTIES
        CXX_EXTENSIONS OFF EXPORT_NAME lua VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR})
    target_compile_options(hyprworks-lua PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
    if(HYPRWORKS_BUILD_PORTAL)
        target_sources(hyprworks-lua PRIVATE src/gui/portal.cpp)
        target_compile_definitions(hyprworks-lua PUBLIC HYPRWORKS_HAS_PORTAL)
        install(FILES share/hyprworks.portal DESTINATION "${CMAKE_INSTALL_DATADIR}/xdg-desktop-portal/portals")
        if(HYPRWORKS_BUILD_TESTS)
            add_executable(hyprworks-pipewire-capture tests/pipewire_capture.cpp)
            target_link_libraries(hyprworks-pipewire-capture PRIVATE hyprworks PkgConfig::HYPRWORKS_libpipewire-0.3)
            target_compile_options(hyprworks-pipewire-capture PRIVATE -UNDEBUG -Wall -Wextra -Wpedantic)
        endif()
    endif()
    if(HYPRWORKS_BUILD_TESTS)
        enable_testing()
        add_executable(hyprworks-invocation-core tests/invocation_core.cpp)
        target_link_libraries(hyprworks-invocation-core PRIVATE hyprworks-lua PkgConfig::HYPRWORKS_lua Threads::Threads)
        target_compile_definitions(hyprworks-invocation-core PRIVATE $<$<BOOL:${HYPRWORKS_BUILD_GUI}>:HYPRWORKS_HAS_GUI>)
        target_compile_options(hyprworks-invocation-core PRIVATE -UNDEBUG -Wall -Wextra -Wpedantic)
        add_test(NAME invocation-core COMMAND hyprworks-invocation-core)
        set_tests_properties(invocation-core PROPERTIES TIMEOUT 15)
        add_test(NAME host-io COMMAND $<TARGET_FILE:hyprworks-cli> --headless --no-watch --entry host.lua)
        set_tests_properties(host-io PROPERTIES TIMEOUT 15 ENVIRONMENT "HYPRWORKS_CONFIG_DIR=${CMAKE_CURRENT_SOURCE_DIR}/tests")
        if(HYPRWORKS_BUILD_POLKIT OR HYPRWORKS_BUILD_DBUS)
            find_program(DBUS_RUN_SESSION_EXECUTABLE dbus-run-session REQUIRED)
        endif()
        if(HYPRWORKS_BUILD_DBUS)
            add_executable(hyprworks-dbus-core tests/dbus_core.cpp)
            target_link_libraries(hyprworks-dbus-core PRIVATE hyprworks)
            target_compile_options(hyprworks-dbus-core PRIVATE -UNDEBUG -Wall -Wextra -Wpedantic)
            add_test(NAME dbus-core COMMAND "${DBUS_RUN_SESSION_EXECUTABLE}" -- $<TARGET_FILE:hyprworks-dbus-core>)
            set_tests_properties(dbus-core PROPERTIES TIMEOUT 15)
            add_test(NAME dbus-service COMMAND "${DBUS_RUN_SESSION_EXECUTABLE}" -- $<TARGET_FILE:hyprworks-cli> --headless --no-watch --entry dbus_service.lua)
            set_tests_properties(dbus-service PROPERTIES TIMEOUT 15 ENVIRONMENT "HYPRWORKS_CONFIG_DIR=${CMAKE_CURRENT_SOURCE_DIR}/tests")
            add_test(NAME job-reconnect COMMAND "${DBUS_RUN_SESSION_EXECUTABLE}" -- sh "${CMAKE_CURRENT_SOURCE_DIR}/tests/job.sh"
                $<TARGET_FILE:hyprworks-cli>)
            set_tests_properties(job-reconnect PROPERTIES TIMEOUT 15 ENVIRONMENT "HYPRWORKS_CONFIG_DIR=${CMAKE_CURRENT_SOURCE_DIR}/tests")
        endif()
        if(HYPRWORKS_BUILD_POLKIT)
            add_executable(hyprworks-polkit-core tests/polkit_core.cpp src/native/polkit.cpp)
            target_link_libraries(hyprworks-polkit-core PRIVATE hyprworks-lua
                PkgConfig::HYPRWORKS_lua PkgConfig::HYPRWORKS_polkit-agent-1 PkgConfig::HYPRWORKS_libsystemd Threads::Threads)
            target_compile_definitions(hyprworks-polkit-core PRIVATE $<$<BOOL:${HYPRWORKS_BUILD_GUI}>:HYPRWORKS_HAS_GUI>)
            target_compile_options(hyprworks-polkit-core PRIVATE -UNDEBUG -Wall -Wextra -Wpedantic)
            target_link_options(hyprworks-polkit-core PRIVATE
                -Wl,--wrap=polkit_agent_session_initiate -Wl,--wrap=polkit_agent_session_response -Wl,--wrap=polkit_agent_session_cancel)
            add_test(NAME polkit-core COMMAND "${DBUS_RUN_SESSION_EXECUTABLE}" -- $<TARGET_FILE:hyprworks-polkit-core>)
            set_tests_properties(polkit-core PROPERTIES TIMEOUT 15)
        endif()
    endif()

    if(HYPRWORKS_BUILD_SHORTCUTS)
        target_sources(hyprworks-lua PRIVATE src/lua/hyprland/shortcuts.cpp)
    endif()

    if(HYPRWORKS_BUILD_GUI)
        pkg_check_modules(WAYLAND_PROTOCOLS REQUIRED wayland-protocols>=1.45)
        pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
        set(GUI_PROTOCOLS
            "${WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml"
            "${WAYLAND_PROTOCOLS_DIR}/unstable/xdg-output/xdg-output-unstable-v1.xml"
            "${WAYLAND_PROTOCOLS_DIR}/stable/viewporter/viewporter.xml"
            "${WAYLAND_PROTOCOLS_DIR}/staging/fractional-scale/fractional-scale-v1.xml"
            "${WAYLAND_PROTOCOLS_DIR}/staging/cursor-shape/cursor-shape-v1.xml"
            "${WAYLAND_PROTOCOLS_DIR}/unstable/tablet/tablet-unstable-v2.xml"
            "${WAYLAND_PROTOCOLS_DIR}/unstable/text-input/text-input-unstable-v3.xml"
            "${WAYLAND_PROTOCOLS_DIR}/staging/ext-foreign-toplevel-list/ext-foreign-toplevel-list-v1.xml"
            "${WAYLAND_PROTOCOLS_DIR}/staging/ext-image-capture-source/ext-image-capture-source-v1.xml"
            "${WAYLAND_PROTOCOLS_DIR}/staging/ext-image-copy-capture/ext-image-copy-capture-v1.xml"
            "${WAYLAND_PROTOCOLS_DIR}/staging/ext-workspace/ext-workspace-v1.xml"
            "${CMAKE_CURRENT_SOURCE_DIR}/protocols/hyprland-workspace-image-capture-source-v1.xml"
            "${CMAKE_CURRENT_SOURCE_DIR}/protocols/wlr-layer-shell-unstable-v1.xml"
            "${CMAKE_CURRENT_SOURCE_DIR}/protocols/wlr-foreign-toplevel-management-unstable-v1.xml"
            "${CMAKE_CURRENT_SOURCE_DIR}/protocols/hyprland-focus-grab-v1.xml")
        foreach(protocol IN LISTS GUI_PROTOCOLS)
            get_filename_component(name "${protocol}" NAME_WE)
            add_custom_command(OUTPUT "${name}-client.h" "${name}-protocol.c"
                COMMAND "${WAYLAND_SCANNER_EXECUTABLE}" client-header "${protocol}" "${name}-client.h"
                COMMAND "${WAYLAND_SCANNER_EXECUTABLE}" private-code "${protocol}" "${name}-protocol.c"
                DEPENDS "${protocol}" VERBATIM)
            list(APPEND GUI_PROTOCOL_SOURCES "${CMAKE_CURRENT_BINARY_DIR}/${name}-protocol.c")
        endforeach()
        target_link_libraries(hyprworks-lua PRIVATE Threads::Threads PkgConfig::HYPRWORKS_json-c)
        target_include_directories(hyprworks-lua PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
        target_sources(hyprworks-lua PRIVATE ${GUI_PROTOCOL_SOURCES}
            src/gui/gui.cpp src/gui/surface.cpp src/gui/input.cpp src/gui/render.cpp
            src/gui/images.cpp src/gui/icons.cpp src/gui/windows.cpp)
        target_compile_definitions(hyprworks-lua PRIVATE HYPRWORKS_HAS_GUI)
        if(HYPRWORKS_BUILD_TESTS)
            add_executable(hyprworks-gui-core tests/gui_core.cpp)
            target_link_libraries(hyprworks-gui-core PRIVATE hyprworks-lua ${HYPRWORKS_LUA_LIBRARIES})
            target_include_directories(hyprworks-gui-core PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
            target_compile_definitions(hyprworks-gui-core PRIVATE HYPRWORKS_HAS_GUI)
            target_compile_options(hyprworks-gui-core PRIVATE -UNDEBUG -Wall -Wextra -Wpedantic)
            add_test(NAME gui-core COMMAND hyprworks-gui-core)
            add_test(NAME image-worker COMMAND hyprworks-gui-core --images)
            set_tests_properties(gui-core image-worker PROPERTIES TIMEOUT 15)
        endif()
    endif()

    add_executable(hyprworks-cli src/runner.cpp)
    target_link_libraries(hyprworks-cli PRIVATE hyprworks::lua)
    target_compile_options(hyprworks-cli PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
    target_compile_definitions(hyprworks-cli PRIVATE
        HYPRWORKS_VERSION="${PROJECT_VERSION}"
        HYPRWORKS_DEFAULT_GUI=$<BOOL:${HYPRWORKS_BUILD_GUI}>)
    file(RELATIVE_PATH HYPRWORKS_LIBRARY_FROM_BINARY "${CMAKE_INSTALL_FULL_BINDIR}" "${CMAKE_INSTALL_FULL_LIBDIR}")
    set_target_properties(hyprworks-cli PROPERTIES
        OUTPUT_NAME hyprworks
        INSTALL_RPATH "$ORIGIN/${HYPRWORKS_LIBRARY_FROM_BINARY}")
    set_target_properties(hyprworks-lua PROPERTIES INSTALL_RPATH "$ORIGIN")
    install(TARGETS hyprworks-lua EXPORT hyprworks-lua-targets
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
    install(TARGETS hyprworks-cli RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
    install(PROGRAMS cmake/60-hyprworks-environment
        DESTINATION "lib/systemd/user-environment-generators")
    install(EXPORT hyprworks-lua-targets NAMESPACE hyprworks:: DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/hyprworks")
    if(HYPRWORKS_BUILD_GUI)
        configure_file(cmake/hyprworks.service.in hyprworks.service @ONLY)
        install(FILES "${CMAKE_CURRENT_BINARY_DIR}/hyprworks.service"
            DESTINATION "${CMAKE_INSTALL_LIBDIR}/systemd/user")
        if(HYPRWORKS_BUILD_POLKIT)
            configure_file(cmake/hyprworks-polkit.service.in hyprworks-polkit.service @ONLY)
            install(FILES "${CMAKE_CURRENT_BINARY_DIR}/hyprworks-polkit.service"
                DESTINATION "${CMAKE_INSTALL_LIBDIR}/systemd/user")
        endif()
    endif()
endif()

foreach(feature IN ITEMS AUDIO DBUS DESKTOP MPRIS NOTIFICATIONS POLKIT TRAY UPOWER)
    if(HYPRWORKS_BUILD_${feature})
        string(TOLOWER "${feature}" source)
        target_sources(hyprworks PRIVATE "src/native/${source}.cpp")
        target_compile_definitions(hyprworks PUBLIC HYPRWORKS_HAS_${feature})
        if(HYPRWORKS_BUILD_LUA)
            target_sources(hyprworks-lua PRIVATE "src/lua/${source}.cpp")
        endif()
    endif()
endforeach()

install(TARGETS hyprworks EXPORT hyprworks-targets
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(DIRECTORY include/hyprworks DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(FILES LICENSE DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/hyprworks")
install(DIRECTORY protocols/ DESTINATION "${CMAKE_INSTALL_DATADIR}/hyprworks/protocols")
install(FILES README.md ARCHITECTURE.md DESTINATION "${CMAKE_INSTALL_DOCDIR}")
install(EXPORT hyprworks-targets NAMESPACE hyprworks:: DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/hyprworks")
configure_package_config_file(cmake/hyprworksConfig.cmake.in "${CMAKE_CURRENT_BINARY_DIR}/hyprworksConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/hyprworks")
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/hyprworksConfigVersion.cmake"
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMinorVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/hyprworksConfig.cmake" "${CMAKE_CURRENT_BINARY_DIR}/hyprworksConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/hyprworks")
