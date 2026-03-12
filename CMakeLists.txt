cmake_minimum_required(VERSION 3.20)
project(HackerLandWM VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# ── Qt6 ───────────────────────────────────────────────────────────────────────
find_package(Qt6 REQUIRED COMPONENTS
    Core Gui Widgets
    WaylandCompositor WaylandClient
    OpenGL OpenGLWidgets
    DBus Concurrent
)
find_package(Qt6 OPTIONAL_COMPONENTS Svg)

# ── System packages ───────────────────────────────────────────────────────────
find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND_SERVER REQUIRED wayland-server)
pkg_check_modules(WAYLAND_CLIENT REQUIRED wayland-client)
pkg_check_modules(XKB            REQUIRED xkbcommon)
pkg_check_modules(PIXMAN         REQUIRED pixman-1)
pkg_check_modules(LIBINPUT       REQUIRED libinput)
pkg_check_modules(UDEV           REQUIRED libudev)

# Optional
pkg_check_modules(XWAYLAND       xwayland)
pkg_check_modules(PIPEWIRE       libpipewire-0.3)
pkg_check_modules(WLROOTS        wlroots)

# ── Architecture detection: support x86 / x86_64 / i686 ──────────────────────
include(CheckCXXCompilerFlag)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64")
    set(ARCH_FLAGS "-march=x86-64" "-mtune=generic" "-msse4.2")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "i[3-6]86")
    set(ARCH_FLAGS "-march=i686" "-mtune=generic" "-msse2")
else()
    set(ARCH_FLAGS "")
endif()

# ── Sources ───────────────────────────────────────────────────────────────────
set(SOURCES
    src/main.cpp

    # Compositor
    src/compositor/WMCompositor.cpp  src/compositor/WMCompositor.h
    src/compositor/WMOutput.cpp      src/compositor/WMOutput.h
    src/compositor/WMSurface.cpp     src/compositor/WMSurface.h
    src/compositor/WMXdgShell.cpp    src/compositor/WMXdgShell.h
    src/compositor/WMLayerShell.cpp  src/compositor/WMLayerShell.h
    src/compositor/WindowRenderState.h
    src/compositor/IPCServer.cpp     src/compositor/IPCServer.h
    src/compositor/LockScreen.cpp    src/compositor/LockScreen.h
    src/compositor/MultiMonitor.cpp  src/compositor/MultiMonitor.h

    # Core
    src/core/TilingEngine.cpp    src/core/TilingEngine.h
    src/core/Workspace.cpp       src/core/Workspace.h
    src/core/Window.cpp          src/core/Window.h
    src/core/InputHandler.cpp    src/core/InputHandler.h
    src/core/Config.cpp          src/core/Config.h
    src/core/AnimationEngine.cpp src/core/AnimationEngine.h
    src/core/GamepadHandler.cpp  src/core/GamepadHandler.h
    src/core/WindowRules.cpp     src/core/WindowRules.h

    # UI
    src/ui/BarWidget.cpp          src/ui/BarWidget.h
    src/ui/NotificationOverlay.cpp src/ui/NotificationOverlay.h
    src/ui/WorkspaceSwitcher.cpp  src/ui/WorkspaceSwitcher.h
    src/ui/GlassWidget.cpp        src/ui/GlassWidget.h
    src/ui/AppLauncher.cpp        src/ui/AppLauncher.h
    src/ui/RenderEngine.cpp       src/ui/RenderEngine.h
    src/ui/GLBlurRenderer.cpp     src/ui/GLBlurRenderer.h

    resources/resources.qrc
)

# XWayland support
if(XWAYLAND_FOUND)
    list(APPEND SOURCES
        src/compositor/XWaylandManager.cpp
        src/compositor/XWaylandManager.h)
    add_definitions(-DHAVE_XWAYLAND)
endif()

# PipeWire screencasting
if(PIPEWIRE_FOUND)
    list(APPEND SOURCES
        src/compositor/ScreencastManager.cpp
        src/compositor/ScreencastManager.h)
    add_definitions(-DHAVE_PIPEWIRE)
endif()

add_executable(hackerlandwm ${SOURCES})

target_include_directories(hackerlandwm PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${WAYLAND_SERVER_INCLUDE_DIRS}
    ${XKB_INCLUDE_DIRS}
    ${PIXMAN_INCLUDE_DIRS}
    ${LIBINPUT_INCLUDE_DIRS}
    ${UDEV_INCLUDE_DIRS}
)

target_link_libraries(hackerlandwm PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets
    Qt6::WaylandCompositor Qt6::WaylandClient
    Qt6::OpenGL Qt6::OpenGLWidgets
    Qt6::DBus Qt6::Concurrent
    $<$<TARGET_EXISTS:Qt6::Svg>:Qt6::Svg>
    ${WAYLAND_SERVER_LIBRARIES}
    ${XKB_LIBRARIES}
    ${PIXMAN_LIBRARIES}
    ${LIBINPUT_LIBRARIES}
    ${UDEV_LIBRARIES}
)

if(XWAYLAND_FOUND)
    target_link_libraries(hackerlandwm PRIVATE ${XWAYLAND_LIBRARIES})
    target_include_directories(hackerlandwm PRIVATE ${XWAYLAND_INCLUDE_DIRS})
endif()
if(PIPEWIRE_FOUND)
    target_link_libraries(hackerlandwm PRIVATE ${PIPEWIRE_LIBRARIES})
    target_include_directories(hackerlandwm PRIVATE ${PIPEWIRE_INCLUDE_DIRS})
endif()

target_compile_options(hackerlandwm PRIVATE
    -Wall -Wextra -O2
    ${ARCH_FLAGS}
)

# Install
install(TARGETS hackerlandwm DESTINATION /usr/bin)
install(FILES misc/hackerlandwm.desktop DESTINATION /usr/share/wayland-sessions/)
install(DIRECTORY misc/themes/ DESTINATION /usr/share/hackerlandwm/themes/)

# ── hackerlandwm-msg IPC client ───────────────────────────────────────────────
add_executable(hackerlandwm-msg src/hackerlandwm-msg.cpp)
target_link_libraries(hackerlandwm-msg PRIVATE Qt6::Core Qt6::Network)
install(TARGETS hackerlandwm-msg DESTINATION /usr/bin)
