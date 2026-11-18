cmake_minimum_required(VERSION 4.5)
project(hyprworks VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(BUILD_SHARED_LIBS "Build shared libraries" ${PROJECT_IS_TOP_LEVEL})
option(HYPRWORKS_BUILD_LUA "Build the Lua authoring library and runtime" ${PROJECT_IS_TOP_LEVEL})
option(HYPRWORKS_BUILD_TOOLKIT "Enable hyprtoolkit in the Lua runtime" ${PROJECT_IS_TOP_LEVEL})

find_package(PkgConfig REQUIRED)
pkg_check_modules(JSONC REQUIRED IMPORTED_TARGET json-c>=0.19)

add_library(hyprworks src/native/hyprland/workspaces.cpp src/native/hyprland/workspace_protocol.cpp src/native/host.cpp)
add_library(hyprworks::hyprworks ALIAS hyprworks)
target_compile_features(hyprworks PUBLIC cxx_std_23)
set_target_properties(hyprworks PROPERTIES CXX_EXTENSIONS OFF VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR})
target_include_directories(hyprworks PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_link_libraries(hyprworks PRIVATE PkgConfig::JSONC)
target_compile_options(hyprworks PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)

option(HYPRWORKS_BUILD_SHORTCUTS "Enable native Hyprland global shortcuts through Wayland" ${PROJECT_IS_TOP_LEVEL})
if(HYPRWORKS_BUILD_SHORTCUTS)
    enable_language(C)
    pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client>=1.26.0)
    pkg_check_modules(WAYLAND_SCANNER REQUIRED wayland-scanner>=1.26.0)
    pkg_get_variable(WAYLAND_SCANNER_EXECUTABLE wayland-scanner wayland_scanner)
    set(SHORTCUT_PROTOCOL "${CMAKE_CURRENT_SOURCE_DIR}/protocols/hyprland-global-shortcuts-v1.xml")
    add_custom_command(OUTPUT hyprland-global-shortcuts-v1-client.h hyprland-global-shortcuts-v1-protocol.c
        COMMAND "${WAYLAND_SCANNER_EXECUTABLE}" client-header "${SHORTCUT_PROTOCOL}" hyprland-global-shortcuts-v1-client.h
        COMMAND "${WAYLAND_SCANNER_EXECUTABLE}" private-code "${SHORTCUT_PROTOCOL}" hyprland-global-shortcuts-v1-protocol.c
        DEPENDS "${SHORTCUT_PROTOCOL}" VERBATIM)
    target_sources(hyprworks PRIVATE src/native/hyprland/shortcuts.cpp "${CMAKE_CURRENT_BINARY_DIR}/hyprland-global-shortcuts-v1-protocol.c")
    target_include_directories(hyprworks PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    target_link_libraries(hyprworks PRIVATE PkgConfig::WAYLAND_CLIENT)
    target_compile_definitions(hyprworks PUBLIC HYPRWORKS_HAS_SHORTCUTS)
endif()

option(HYPRWORKS_BUILD_UPOWER "Enable native UPower observation through sd-bus" ${PROJECT_IS_TOP_LEVEL})
option(HYPRWORKS_BUILD_MPRIS "Enable native MPRIS media control through sd-bus" ${PROJECT_IS_TOP_LEVEL})
option(HYPRWORKS_BUILD_NOTIFICATIONS "Enable native desktop notification serving through sd-bus" ${PROJECT_IS_TOP_LEVEL})
option(HYPRWORKS_BUILD_TRAY "Enable native StatusNotifier and D-Bus menus through sd-bus" ${PROJECT_IS_TOP_LEVEL})
option(HYPRWORKS_BUILD_COMMANDS "Enable named external commands through sd-bus" ${PROJECT_IS_TOP_LEVEL})
if(HYPRWORKS_BUILD_UPOWER OR HYPRWORKS_BUILD_MPRIS OR HYPRWORKS_BUILD_NOTIFICATIONS OR HYPRWORKS_BUILD_TRAY OR HYPRWORKS_BUILD_COMMANDS)
    pkg_check_modules(SYSTEMD REQUIRED IMPORTED_TARGET libsystemd>=262)
    target_link_libraries(hyprworks PRIVATE PkgConfig::SYSTEMD)
endif()
if(HYPRWORKS_BUILD_TRAY)
    target_sources(hyprworks PRIVATE src/native/dbusmenu.cpp)
endif()

option(HYPRWORKS_BUILD_DESKTOP "Enable XDG desktop entries through GIO" ${PROJECT_IS_TOP_LEVEL})
if(HYPRWORKS_BUILD_DESKTOP)
    pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-unix-2.0>=2.90.1)
    target_link_libraries(hyprworks PRIVATE PkgConfig::GIO)
endif()

option(HYPRWORKS_BUILD_AUDIO "Enable native audio through libpulse" ${PROJECT_IS_TOP_LEVEL})
if(HYPRWORKS_BUILD_AUDIO)
    pkg_check_modules(PULSE REQUIRED IMPORTED_TARGET libpulse>=17.0)
    target_link_libraries(hyprworks PRIVATE PkgConfig::PULSE)
endif()

if(HYPRWORKS_BUILD_LUA)
    pkg_check_modules(LUA REQUIRED IMPORTED_TARGET lua>=5.5.1)
    add_library(hyprworks-lua
        src/lua/invocation.cpp src/lua/run.cpp src/lua/host.cpp src/lua/hyprland/workspaces.cpp)
    add_library(hyprworks::lua ALIAS hyprworks-lua)
    target_link_libraries(hyprworks-lua PUBLIC hyprworks::hyprworks PRIVATE PkgConfig::LUA)
    set_target_properties(hyprworks-lua PROPERTIES
        CXX_EXTENSIONS OFF EXPORT_NAME lua VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR})
    target_compile_options(hyprworks-lua PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)

    if(HYPRWORKS_BUILD_SHORTCUTS)
        target_sources(hyprworks-lua PRIVATE src/lua/hyprland/shortcuts.cpp)
    endif()

    if(HYPRWORKS_BUILD_TOOLKIT)
        target_sources(hyprworks-lua PRIVATE src/lua/gui/backend.cpp src/lua/gui/surfaces.cpp src/lua/gui/content.cpp)
        pkg_check_modules(HYPRTOOLKIT REQUIRED IMPORTED_TARGET hyprtoolkit>=0.6.0)
        pkg_check_modules(XKBCOMMON REQUIRED IMPORTED_TARGET xkbcommon>=1.13.2)
        target_link_libraries(hyprworks-lua PRIVATE PkgConfig::HYPRTOOLKIT PkgConfig::XKBCOMMON)
        target_compile_definitions(hyprworks-lua PRIVATE HYPRWORKS_HAS_TOOLKIT)
    endif()

    add_executable(hyprworks-cli src/runner.cpp)
    target_link_libraries(hyprworks-cli PRIVATE hyprworks::lua)
    target_compile_options(hyprworks-cli PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
    target_compile_definitions(hyprworks-cli PRIVATE
        HYPRWORKS_VERSION="${PROJECT_VERSION}"
        HYPRWORKS_DEFAULT_TOOLKIT=$<BOOL:${HYPRWORKS_BUILD_TOOLKIT}>)
    file(RELATIVE_PATH HYPRWORKS_LIBRARY_FROM_BINARY "${CMAKE_INSTALL_FULL_BINDIR}" "${CMAKE_INSTALL_FULL_LIBDIR}")
    set_target_properties(hyprworks-cli PROPERTIES
        OUTPUT_NAME hyprworks
        INSTALL_RPATH "$ORIGIN/${HYPRWORKS_LIBRARY_FROM_BINARY}")
    set_target_properties(hyprworks-lua PROPERTIES INSTALL_RPATH "$ORIGIN")
    install(TARGETS hyprworks-lua EXPORT hyprworks-lua-targets
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
    install(TARGETS hyprworks-cli RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
    install(EXPORT hyprworks-lua-targets NAMESPACE hyprworks:: DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/hyprworks")
    if(HYPRWORKS_BUILD_TOOLKIT)
        set_property(TARGET hyprworks-cli PROPERTY INSTALL_RPATH "$ORIGIN/${HYPRWORKS_LIBRARY_FROM_BINARY}/hyprworks;$ORIGIN/${HYPRWORKS_LIBRARY_FROM_BINARY}")
        set_property(TARGET hyprworks-lua PROPERTY INSTALL_RPATH "$ORIGIN/hyprworks;$ORIGIN")
        set(HYPRWORKS_GUI_LIBDIR "${CMAKE_INSTALL_LIBDIR}/hyprworks")
        if(NOT IS_ABSOLUTE "${HYPRWORKS_GUI_LIBDIR}")
            set(HYPRWORKS_GUI_LIBDIR "$<INSTALL_PREFIX>/${HYPRWORKS_GUI_LIBDIR}")
        endif()
        target_link_options(hyprworks-lua INTERFACE "$<INSTALL_INTERFACE:LINKER:-rpath-link,${HYPRWORKS_GUI_LIBDIR}>")
        set(HYPRWORKS_GUI_LIBRARIES ${HYPRTOOLKIT_LINK_LIBRARIES})
        list(FILTER HYPRWORKS_GUI_LIBRARIES INCLUDE REGEX "/libhypr(toolkit|graphics)\\.so$")
        foreach(library IN LISTS HYPRWORKS_GUI_LIBRARIES)
            install(CODE "
                file(INSTALL DESTINATION \"${HYPRWORKS_GUI_LIBDIR}\"
                    TYPE SHARED_LIBRARY FOLLOW_SYMLINK_CHAIN FILES \"${library}\")
            ")
        endforeach()
        install(FILES patches/hyprgraphics-LICENSE patches/hyprtoolkit-LICENSE
            DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/hyprworks")
        configure_file(cmake/hyprworks.service.in hyprworks.service @ONLY)
        install(FILES "${CMAKE_CURRENT_BINARY_DIR}/hyprworks.service"
            DESTINATION "${CMAKE_INSTALL_LIBDIR}/systemd/user")
    endif()
endif()

foreach(feature IN ITEMS AUDIO COMMANDS DESKTOP MPRIS NOTIFICATIONS TRAY UPOWER)
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
install(FILES README.md DESTINATION "${CMAKE_INSTALL_DOCDIR}")
install(EXPORT hyprworks-targets NAMESPACE hyprworks:: DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/hyprworks")
configure_package_config_file(cmake/hyprworks-config.cmake.in "${CMAKE_CURRENT_BINARY_DIR}/hyprworks-config.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/hyprworks")
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/hyprworks-config-version.cmake"
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMinorVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/hyprworks-config.cmake" "${CMAKE_CURRENT_BINARY_DIR}/hyprworks-config-version.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/hyprworks")
