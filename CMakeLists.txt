cmake_minimum_required(VERSION 3.20)
project(HackerLandWM VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
# Source files — AUTORCC processes resources/resources.qrc automatically
set(CMAKE_AUTOUIC ON)

# Find Qt6
find_package(Qt6 REQUIRED COMPONENTS
    Core
    Gui
    Widgets
    WaylandCompositor
    WaylandClient
    OpenGL
    OpenGLWidgets
    DBus
    Concurrent
)

# Find system packages
find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND_SERVER REQUIRED wayland-server)
pkg_check_modules(WAYLAND_CLIENT REQUIRED wayland-client)
pkg_check_modules(XKB REQUIRED xkbcommon)
pkg_check_modules(PIXMAN REQUIRED pixman-1)

# Optional: wlroots backend
pkg_check_modules(WLROOTS wlroots)

# Source files  (AUTORCC processes resources/resources.qrc automatically)
set(SOURCES
    src/main.cpp
    src/compositor/WMCompositor.cpp
    src/compositor/WMCompositor.h
    src/compositor/WMOutput.cpp
    src/compositor/WMOutput.h
    src/compositor/WMSurface.cpp
    src/compositor/WMSurface.h
    src/compositor/WMXdgShell.cpp
    src/compositor/WMXdgShell.h
    src/compositor/WMLayerShell.cpp
    src/compositor/WMLayerShell.h
    src/core/TilingEngine.cpp
    src/core/TilingEngine.h
    src/core/Workspace.cpp
    src/core/Workspace.h
    src/core/Window.cpp
    src/core/Window.h
    src/core/InputHandler.cpp
    src/core/InputHandler.h
    src/core/Config.cpp
    src/core/Config.h
    src/core/AnimationEngine.cpp
    src/core/AnimationEngine.h
    src/ui/BarWidget.cpp
    src/ui/BarWidget.h
    src/ui/NotificationOverlay.cpp
    src/ui/NotificationOverlay.h
    src/ui/WorkspaceSwitcher.cpp
    src/ui/WorkspaceSwitcher.h
    src/ui/GlassWidget.cpp
    src/ui/GlassWidget.h
    src/ui/AppLauncher.cpp
    src/ui/AppLauncher.h
    src/ui/RenderEngine.cpp
    src/ui/RenderEngine.h
    resources/resources.qrc
)

add_executable(hackerlandwm ${SOURCES})

target_include_directories(hackerlandwm PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${WAYLAND_SERVER_INCLUDE_DIRS}
    ${XKB_INCLUDE_DIRS}
    ${PIXMAN_INCLUDE_DIRS}
)

target_link_libraries(hackerlandwm PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::WaylandCompositor
    Qt6::WaylandClient
    Qt6::OpenGL
    Qt6::OpenGLWidgets
    Qt6::DBus
    Qt6::Concurrent
    ${WAYLAND_SERVER_LIBRARIES}
    ${XKB_LIBRARIES}
    ${PIXMAN_LIBRARIES}
)

target_compile_options(hackerlandwm PRIVATE
    -Wall -Wextra
    -O2
    -march=native
)

# Install targets
install(TARGETS hackerlandwm DESTINATION /usr/bin)
install(FILES misc/hackerlandwm.desktop DESTINATION /usr/share/wayland-sessions/)
install(FILES misc/hackerlandwm.conf DESTINATION /etc/hackerlandwm/)
install(DIRECTORY misc/themes/ DESTINATION /usr/share/hackerlandwm/themes/)
