
cmake_minimum_required(VERSION 3.16)

project(slime-wallpaper C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)

pkg_check_modules(WAYLAND REQUIRED wayland-client wayland-protocols)

find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)

execute_process(
COMMAND pkg-config --variable=pkgdatadir wayland-protocols
OUTPUT_VARIABLE WAYLAND_PROTOCOLS
OUTPUT_STRIP_TRAILING_WHITESPACE
)

set(WLR_PROTOCOLS /usr/share/wlr-protocols)

set(GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)

file(MAKE_DIRECTORY ${GENERATED_DIR})

set(XDG_XML
${WAYLAND_PROTOCOLS}/stable/xdg-shell/xdg-shell.xml
)

set(VIEWPORTER_XML
${WAYLAND_PROTOCOLS}/stable/viewporter/viewporter.xml
)

set(WLR_LAYER_SHELL_XML
${WLR_PROTOCOLS}/unstable/wlr-layer-shell-unstable-v1.xml
)

add_custom_command(
OUTPUT
${GENERATED_DIR}/xdg-shell-client-protocol.h
${GENERATED_DIR}/xdg-shell-protocol.c
COMMAND ${WAYLAND_SCANNER} client-header
${XDG_XML}
${GENERATED_DIR}/xdg-shell-client-protocol.h
COMMAND ${WAYLAND_SCANNER} private-code
${XDG_XML}
${GENERATED_DIR}/xdg-shell-protocol.c
)

add_custom_command(
OUTPUT
${GENERATED_DIR}/viewporter-client-protocol.h
${GENERATED_DIR}/viewporter-protocol.c
COMMAND ${WAYLAND_SCANNER} client-header
${VIEWPORTER_XML}
${GENERATED_DIR}/viewporter-client-protocol.h
COMMAND ${WAYLAND_SCANNER} private-code
${VIEWPORTER_XML}
${GENERATED_DIR}/viewporter-protocol.c
)

add_custom_command(
OUTPUT
${GENERATED_DIR}/wlr-layer-shell-unstable-v1-client-protocol.h
${GENERATED_DIR}/wlr-layer-shell-unstable-v1-protocol.c
COMMAND ${WAYLAND_SCANNER} client-header
${WLR_LAYER_SHELL_XML}
${GENERATED_DIR}/wlr-layer-shell-unstable-v1-client-protocol.h
COMMAND ${WAYLAND_SCANNER} private-code
${WLR_LAYER_SHELL_XML}
${GENERATED_DIR}/wlr-layer-shell-unstable-v1-protocol.c
)

add_executable(slime-wallpaper
slime_wallpaper.c
${GENERATED_DIR}/xdg-shell-protocol.c
${GENERATED_DIR}/viewporter-protocol.c
${GENERATED_DIR}/wlr-layer-shell-unstable-v1-protocol.c
)

target_compile_options(slime-wallpaper PRIVATE
-O3
-march=native
-Wall
)

target_include_directories(slime-wallpaper PRIVATE
${GENERATED_DIR}
${WAYLAND_INCLUDE_DIRS}
)

target_link_libraries(slime-wallpaper PRIVATE
${WAYLAND_LIBRARIES}
m
)
