cmake_minimum_required(VERSION 3.27)

project(zoomparent
    DESCRIPTION "yabai-style zoom-parent for Hyprland's dwindle layout"
    VERSION 0.1
)

set(CMAKE_CXX_STANDARD 26)

add_library(zoomparent SHARED main.cpp)

find_package(PkgConfig REQUIRED)
pkg_check_modules(deps REQUIRED IMPORTED_TARGET
    hyprland
    libdrm
    libinput
    libudev
    pixman-1
    wayland-server
    xkbcommon
)
target_link_libraries(zoomparent PRIVATE rt PkgConfig::deps)

install(TARGETS zoomparent)
