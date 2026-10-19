cmake_minimum_required(VERSION 3.24)
project(sonic_pi_callback_spike VERSION 0.1.8 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# This is the deliberately small, device-free build: the host supplies audio
# buffers, so Clockwork must not compile its own device layer.
set(CLOCKWORK_DEVICE OFF CACHE BOOL "" FORCE)
set(CLOCKWORK_RUST ON CACHE BOOL "" FORCE)
set(CLOCKWORK_MIDI OFF CACHE BOOL "" FORCE)
set(CLOCKWORK_GAMEPAD OFF CACHE BOOL "" FORCE)
set(CLOCKWORK_OSC OFF CACHE BOOL "" FORCE)
set(CLOCKWORK_PLUGINS OFF CACHE BOOL "" FORCE)
set(CLOCKWORK_STANDALONE OFF CACHE BOOL "" FORCE)
set(CLOCKWORK_LINK OFF CACHE BOOL "" FORCE)
set(CLOCKWORK_LINK_AUDIO OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SMTG_ENABLE_VST3_HOSTING_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SMTG_ENABLE_VSTGUI_SUPPORT ON CACHE BOOL "" FORCE)
set(SMTG_ENABLE_WAYLAND_SUPPORT OFF CACHE BOOL "" FORCE)
set(SMTG_ADD_VST3_UTILITIES ON CACHE BOOL "" FORCE)
set(SMTG_CREATE_PLUGIN_LINK OFF CACHE BOOL "" FORCE)

# Nix keeps X11 development headers and libraries in separate store paths,
# which CMake's FindX11 does not discover from pkg-config automatically.
if(LINUX)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SONIC_PI_VSTGUI_XCB REQUIRED
        xcb xcb-util xcb-cursor xcb-keysyms xcb-xkb xkbcommon xkbcommon-x11
        glib-2.0 cairo pangocairo pangoft2 fontconfig freetype2)
    execute_process(
        COMMAND "${PKG_CONFIG_EXECUTABLE}" --variable=includedir x11
        OUTPUT_VARIABLE SONIC_PI_X11_INCLUDE_DIR
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    execute_process(
        COMMAND "${PKG_CONFIG_EXECUTABLE}" --variable=libdir x11
        OUTPUT_VARIABLE SONIC_PI_X11_LIBRARY_DIR
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(EXISTS "${SONIC_PI_X11_INCLUDE_DIR}/X11/Xlib.h")
        set(X11_X11_INCLUDE_PATH "${SONIC_PI_X11_INCLUDE_DIR}"
            CACHE PATH "X11 include directory" FORCE)
    endif()
    if(EXISTS "${SONIC_PI_X11_LIBRARY_DIR}/libX11.so")
        set(X11_X11_LIB "${SONIC_PI_X11_LIBRARY_DIR}/libX11.so"
            CACHE FILEPATH "X11 library" FORCE)
    endif()

    execute_process(
        COMMAND "${PKG_CONFIG_EXECUTABLE}" --variable=includedir freetype2
        OUTPUT_VARIABLE SONIC_PI_FREETYPE_INCLUDE_DIR
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    execute_process(
        COMMAND "${PKG_CONFIG_EXECUTABLE}" --variable=libdir freetype2
        OUTPUT_VARIABLE SONIC_PI_FREETYPE_LIBRARY_DIR
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(EXISTS "${SONIC_PI_FREETYPE_INCLUDE_DIR}/ft2build.h")
        set(FREETYPE_INCLUDE_DIR_ft2build "${SONIC_PI_FREETYPE_INCLUDE_DIR}"
            CACHE PATH "Freetype include directory" FORCE)
        set(FREETYPE_INCLUDE_DIR_freetype2 "${SONIC_PI_FREETYPE_INCLUDE_DIR}/freetype2"
            CACHE PATH "Freetype headers directory" FORCE)
    endif()
    if(EXISTS "${SONIC_PI_FREETYPE_LIBRARY_DIR}/libfreetype.so")
        set(FREETYPE_LIBRARY "${SONIC_PI_FREETYPE_LIBRARY_DIR}/libfreetype.so"
            CACHE FILEPATH "Freetype library" FORCE)
    endif()

    execute_process(
        COMMAND "${PKG_CONFIG_EXECUTABLE}" --variable=includedir cairo
        OUTPUT_VARIABLE SONIC_PI_CAIRO_INCLUDE_ROOT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    execute_process(
        COMMAND "${PKG_CONFIG_EXECUTABLE}" --variable=includedir icu-uc
        OUTPUT_VARIABLE SONIC_PI_ICU_INCLUDE_ROOT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    execute_process(
        COMMAND "${PKG_CONFIG_EXECUTABLE}" --variable=includedir xcb
        OUTPUT_VARIABLE SONIC_PI_XCB_INCLUDE_ROOT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )

    # FindWayland.cmake searches /usr/share only, while Replit's Nix packages
    # keep protocol XML in an immutable /nix/store path.
    file(GLOB SONIC_PI_WAYLAND_PROTOCOL_DIRS
        "/nix/store/*-wayland-protocols-*/share/wayland-protocols")
    foreach(protocol_dir IN LISTS SONIC_PI_WAYLAND_PROTOCOL_DIRS)
        if(EXISTS "${protocol_dir}/stable/xdg-shell/xdg-shell.xml")
            set(WAYLAND_PROTOCOLS_BASEDIR "${protocol_dir}"
                CACHE PATH "Wayland protocol XML directory" FORCE)
            break()
        endif()
    endforeach()

    file(GLOB SONIC_PI_XORGPROTO_INCLUDE_DIRS
        "/nix/store/*-xorgproto-*/include")
    foreach(include_dir IN LISTS SONIC_PI_XORGPROTO_INCLUDE_DIRS)
        if(EXISTS "${include_dir}/X11/X.h")
            set(SONIC_PI_XORGPROTO_INCLUDE_ROOT "${include_dir}")
            break()
        endif()
    endforeach()
endif()

include(FetchContent)
FetchContent_Declare(supersonic
    GIT_REPOSITORY https://github.com/samaaron/supersonic.git
    GIT_TAG v0.91.0
    GIT_SUBMODULES clockwork
    GIT_SUBMODULES_RECURSE TRUE
)
FetchContent_MakeAvailable(supersonic)

# Use Sonic Pi's actual Ruby language runtime and built-in library. It is
# launched as a helper process so Ruby evaluation never runs on the host's
# real-time audio thread.
FetchContent_Declare(sonic_pi_runtime
    GIT_REPOSITORY https://github.com/sonic-pi-net/sonic-pi.git
    GIT_TAG v5.0.0
    GIT_SHALLOW TRUE
    GIT_SUBMODULES ""
)
FetchContent_GetProperties(sonic_pi_runtime)
if(NOT sonic_pi_runtime_POPULATED)
    FetchContent_Populate(sonic_pi_runtime)
endif()

FetchContent_Declare(vst3sdk
    GIT_REPOSITORY https://github.com/steinbergmedia/vst3sdk.git
    GIT_TAG v3.8.1_build_84
    GIT_SUBMODULES base cmake pluginterfaces public.sdk vstgui4
    GIT_SUBMODULES_RECURSE TRUE
)
cmake_policy(PUSH)
if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
endif()
FetchContent_GetProperties(vst3sdk)
if(NOT vst3sdk_POPULATED)
    FetchContent_Populate(vst3sdk)
endif()
cmake_policy(POP)

# The SDK helper enables VSTGUI's optional standalone application, which needs
# gtkmm. This plug-in only uses the VST editor bindings, so skip that extra app.
set(_vstgui_support_module
    "${vst3sdk_SOURCE_DIR}/cmake/modules/SMTG_VstGuiSupport.cmake")
file(READ "${_vstgui_support_module}" _vstgui_support_contents)
string(FIND "${_vstgui_support_contents}" "set(VSTGUI_STANDALONE ON)"
       _vstgui_standalone_line)
if(NOT _vstgui_standalone_line LESS 0)
    string(REPLACE "set(VSTGUI_STANDALONE ON)" "set(VSTGUI_STANDALONE OFF)"
           _vstgui_support_contents "${_vstgui_support_contents}")
else()
    string(FIND "${_vstgui_support_contents}" "set(VSTGUI_STANDALONE OFF)"
           _vstgui_standalone_off_line)
    if(_vstgui_standalone_off_line LESS 0)
        message(FATAL_ERROR "Could not find the VSTGUI standalone setting in the SDK")
    endif()
endif()
file(WRITE "${_vstgui_support_module}" "${_vstgui_support_contents}")

add_subdirectory("${vst3sdk_SOURCE_DIR}" "${vst3sdk_BINARY_DIR}")
if(TARGET vstgui)
    target_include_directories(vstgui PRIVATE
        ${SONIC_PI_VSTGUI_XCB_INCLUDE_DIRS})
    target_link_directories(vstgui PUBLIC
        ${SONIC_PI_VSTGUI_XCB_LIBRARY_DIRS})
    foreach(include_root IN ITEMS
            "${SONIC_PI_CAIRO_INCLUDE_ROOT}"
            "${SONIC_PI_ICU_INCLUDE_ROOT}"
            "${SONIC_PI_XCB_INCLUDE_ROOT}"
            "${SONIC_PI_XORGPROTO_INCLUDE_ROOT}")
        if(include_root AND IS_DIRECTORY "${include_root}")
            target_include_directories(vstgui PRIVATE "${include_root}")
        endif()
    endforeach()
endif()

add_executable(sonic_pi_render_probe src/render_probe.cpp)
target_link_libraries(sonic_pi_render_probe PRIVATE clockwork_bundle)
target_compile_definitions(sonic_pi_render_probe PRIVATE
    SONIC_PI_BEEP_SYNTHDEF="${supersonic_SOURCE_DIR}/packages/supersonic-scsynth-synthdefs/synthdefs/sonic-pi-beep.scsyndef"
)

add_executable(sonic_pi_live_probe src/live_runtime_probe.cpp)
target_link_libraries(sonic_pi_live_probe PRIVATE
    clockwork_bundle
    sonic_pi_live_runtime
)

add_library(sonic_pi_live_runtime STATIC
    src/vst3/clockwork_tcp_osc_bridge.cpp
    src/vst3/sonic_pi_live_runtime.cpp
)
target_include_directories(sonic_pi_live_runtime PUBLIC src/vst3)
target_compile_definitions(sonic_pi_live_runtime PRIVATE
    SONIC_PI_ROOT_DIRECTORY="${sonic_pi_runtime_SOURCE_DIR}"
)
target_link_libraries(sonic_pi_live_runtime PRIVATE clockwork_bundle)
if(WIN32)
    target_link_libraries(sonic_pi_live_runtime PUBLIC ws2_32)
endif()

smtg_add_vst3plugin(sonic_pi_vst3
    src/vst3/plugin_entry.cpp
    src/vst3/sonic_pi_editor.cpp
    src/vst3/sonic_pi_processor.cpp
)
if(APPLE)
    target_sources(sonic_pi_vst3 PRIVATE
        "${vst3sdk_SOURCE_DIR}/public.sdk/source/main/macmain.cpp")
elseif(WIN32)
    target_sources(sonic_pi_vst3 PRIVATE
        "${vst3sdk_SOURCE_DIR}/public.sdk/source/main/dllmain.cpp")
else()
    target_sources(sonic_pi_vst3 PRIVATE
        "${vst3sdk_SOURCE_DIR}/public.sdk/source/main/linuxmain.cpp")
endif()
target_include_directories(sonic_pi_vst3 PRIVATE
    "${SMTG_VSTGUI_SOURCE_DIR}"
)
target_compile_features(sonic_pi_vst3 PRIVATE cxx_std_20)
target_link_libraries(sonic_pi_vst3 PRIVATE
    sdk
    vstgui_support
    clockwork_bundle
    sonic_pi_live_runtime
)
smtg_target_add_plugin_resources(sonic_pi_vst3
    RESOURCES
        resource/sonic_pi.uidesc
)

