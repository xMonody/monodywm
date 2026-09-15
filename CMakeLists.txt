cmake_minimum_required(VERSION 3.16)
project(monodywm VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# wlroots exposes unstable headers through this define
add_compile_definitions(WLR_USE_UNSTABLE)

find_package(PkgConfig REQUIRED)

pkg_check_modules(WLROOTS REQUIRED IMPORTED_TARGET wlroots-0.20)
pkg_check_modules(WAYLAND_SERVER REQUIRED IMPORTED_TARGET wayland-server)
pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)
pkg_check_modules(XKBCOMMON REQUIRED IMPORTED_TARGET xkbcommon)
pkg_check_modules(CJSON REQUIRED IMPORTED_TARGET libcjson)
pkg_check_modules(PIXMAN REQUIRED IMPORTED_TARGET pixman-1)
pkg_check_modules(LIBINPUT REQUIRED IMPORTED_TARGET libinput)
pkg_check_modules(EGL REQUIRED IMPORTED_TARGET egl)
pkg_check_modules(GLESV2 REQUIRED IMPORTED_TARGET glesv2)

find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)

# ---------------------------------------------------------------------------
# protocol code generation
#
# Every protocol this project speaks lives as an XML description in
# protocol/.  wayland-scanner turns each of them into three artifacts in the
# build tree:
#
#   <name>-protocol.h          (server-side header, wlroots naming:
#                                wlroots' installed headers include these)
#   <name>-client-protocol.h  (client-side header)
#   <name>-protocol.c         (marshalling code, generated like the user
#                              asked: cmake generates .h *and* .c from xml)
#
# The compositor itself does not link any of this code: as a wlroots
# compositor, wlroots implements the protocol globals server-side and
# registers them with the display.  It only needs the generated server
# headers on the include path, because the installed wlroots headers
# (e.g. wlr_layer_shell_v1.h) #include them.  The generated client code
# is what the bundled test clients link against, which makes the tests
# self-contained (no dependency on a wlroots build tree anymore).
# ---------------------------------------------------------------------------

set(PROTOCOLS
	wayland
	xdg-shell
	xdg-decoration-unstable-v1
	xdg-output-unstable-v1
	viewporter
	presentation-time
	linux-dmabuf-v1
	primary-selection-unstable-v1
	wlr-layer-shell-unstable-v1
	wlr-output-management-unstable-v1
	wlr-foreign-toplevel-management-unstable-v1
	wlr-virtual-pointer-unstable-v1
	virtual-keyboard-unstable-v1
	input-method-unstable-v2
	text-input-unstable-v3
	cursor-shape-v1
	tablet-unstable-v2
	xdg-activation-v1
	wp_fractional_scale_v1
	linux-drm-syncobj-v1
	wlr-screencopy-unstable-v1
)

set(PROTOCOL_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/protocol)
file(MAKE_DIRECTORY ${PROTOCOL_GEN_DIR})

# generates the three artifacts for one protocol xml file; appends their
# paths to the given output variables
function(wayland_generate name)
	set(xml ${CMAKE_CURRENT_SOURCE_DIR}/protocol/${name}.xml)
	set(prefix ${PROTOCOL_GEN_DIR}/${name})

	add_custom_command(
		OUTPUT ${prefix}-protocol.h
		COMMAND ${WAYLAND_SCANNER} server-header ${xml} ${prefix}-protocol.h
		DEPENDS ${xml}
	)
	add_custom_command(
		OUTPUT ${prefix}-client-protocol.h
		COMMAND ${WAYLAND_SCANNER} client-header ${xml} ${prefix}-client-protocol.h
		DEPENDS ${xml}
	)
	add_custom_command(
		OUTPUT ${prefix}-protocol.c
		COMMAND ${WAYLAND_SCANNER} private-code ${xml} ${prefix}-protocol.c
		DEPENDS ${xml}
	)

	# collect them for the caller
	set(${name}_SERVER_HEADER ${prefix}-protocol.h PARENT_SCOPE)
	set(${name}_CLIENT_HEADER ${prefix}-client-protocol.h PARENT_SCOPE)
	set(${name}_CODE ${prefix}-protocol.c PARENT_SCOPE)
	set(PROTOCOL_GEN_FILES ${PROTOCOL_GEN_FILES} ${prefix}-protocol.h
		${prefix}-client-protocol.h ${prefix}-protocol.c PARENT_SCOPE)
endfunction()

set(PROTOCOL_GEN_FILES)
foreach(p ${PROTOCOLS})
	wayland_generate(${p})
endforeach()


# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------

set(COMPOSITOR_SOURCES
	src/main.c
	src/ipc.c
	src/scene.c
	src/ime.c
	src/toplevel.c
	src/rounded.c
	src/border.c
	src/shadow.c
	src/animate.c
	src/place.c
	src/layer.c
	src/output.c
	src/input.c
	src/pointer.c
)

add_library(protocol_stubs STATIC
	${xdg-shell_CODE}
	${xdg-decoration-unstable-v1_CODE}
	${wlr-layer-shell-unstable-v1_CODE}
	${wlr-virtual-pointer-unstable-v1_CODE}
	${virtual-keyboard-unstable-v1_CODE}
	${input-method-unstable-v2_CODE}
	${text-input-unstable-v3_CODE}
	${cursor-shape-v1_CODE}
	${tablet-unstable-v2_CODE}
)
target_include_directories(protocol_stubs PUBLIC ${PROTOCOL_GEN_DIR})
target_link_libraries(protocol_stubs PUBLIC PkgConfig::WAYLAND_CLIENT)


add_executable(monodywm ${COMPOSITOR_SOURCES})
target_include_directories(monodywm PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/src
	${PROTOCOL_GEN_DIR} # wlroots' installed headers #include generated ones
)
target_link_libraries(monodywm PRIVATE
	PkgConfig::WLROOTS
	PkgConfig::WAYLAND_SERVER
	PkgConfig::XKBCOMMON
	PkgConfig::CJSON
	PkgConfig::LIBINPUT
	PkgConfig::PIXMAN
	PkgConfig::EGL
	PkgConfig::GLESV2
	m
)
install(TARGETS monodywm RUNTIME DESTINATION bin)

# bundled test clients (exercise the compositor; need the generated
# client-side protocol code)
add_executable(test-client test-client.c)
target_include_directories(test-client PRIVATE ${PROTOCOL_GEN_DIR})
target_link_libraries(test-client PRIVATE protocol_stubs PkgConfig::WAYLAND_CLIENT)

add_executable(test-interaction test-interaction.c)
target_include_directories(test-interaction PRIVATE ${PROTOCOL_GEN_DIR})
target_link_libraries(test-interaction PRIVATE protocol_stubs PkgConfig::WAYLAND_CLIENT)

# cursor-style test: terminal-like client (SSD + I-beam cursor) driven by a
# virtual pointer; verify cursor decisions via the compositor's WLR_DEBUG log
add_executable(test-cursor test-cursor.c)
target_include_directories(test-cursor PRIVATE ${PROTOCOL_GEN_DIR})
target_link_libraries(test-cursor PRIVATE protocol_stubs PkgConfig::WAYLAND_CLIENT)

# cursor-shape-v1 negotiation test: binds the cursor-shape global, sets a
# shape on the pointer and drives a virtual pointer into a window to check
# the compositor renders it and restores it after an override
add_executable(test-cursor-shape test-cursor-shape.c)
target_include_directories(test-cursor-shape PRIVATE ${PROTOCOL_GEN_DIR})
target_link_libraries(test-cursor-shape PRIVATE protocol_stubs PkgConfig::WAYLAND_CLIENT)

add_executable(test-select-drag test-select-drag.c)
target_include_directories(test-select-drag PRIVATE ${PROTOCOL_GEN_DIR})
target_link_libraries(test-select-drag PRIVATE protocol_stubs PkgConfig::WAYLAND_CLIENT)

# resize-cursor test: edge-resize drag with a client that honors configure
# sizes and re-requests its cursor mid-drag; verifies the resize cursor
# stays put for the whole drag (no flicker)
add_executable(test-resize-cursor test-resize-cursor.c)
target_include_directories(test-resize-cursor PRIVATE ${PROTOCOL_GEN_DIR})
target_link_libraries(test-resize-cursor PRIVATE protocol_stubs PkgConfig::WAYLAND_CLIENT)

# mask-guard test: (obsolete) the custom rounded-mask pass was removed;
# kept as a plain client commit smoke test
add_executable(test-mask-guard test-mask-guard.c)
target_include_directories(test-mask-guard PRIVATE ${PROTOCOL_GEN_DIR})
target_link_libraries(test-mask-guard PRIVATE protocol_stubs PkgConfig::WAYLAND_CLIENT)

# subsurface restack test: place_below/place_above carry no damage and are
# applied on the parent commit; the FBO cache must detect the order change
add_executable(test-restack test-restack.c)
target_include_directories(test-restack PRIVATE ${PROTOCOL_GEN_DIR})
target_link_libraries(test-restack PRIVATE protocol_stubs PkgConfig::WAYLAND_CLIENT)

# border test: two toplevels + focus switches
add_executable(test-bar-clamp test-bar-clamp.c)
target_include_directories(test-bar-clamp PRIVATE ${PROTOCOL_GEN_DIR})
target_link_libraries(test-bar-clamp PRIVATE protocol_stubs PkgConfig::WAYLAND_CLIENT)

add_executable(test-border test-border.c)
target_include_directories(test-border PRIVATE ${PROTOCOL_GEN_DIR})
target_link_libraries(test-border PRIVATE protocol_stubs PkgConfig::WAYLAND_CLIENT)

# quit test: virtual keyboard presses Shift+Alt+q; verifies the compositor
# shuts down cleanly (no wlroots teardown assertions)
add_executable(test-quit test-quit.c)
target_include_directories(test-quit PRIVATE ${PROTOCOL_GEN_DIR})
target_link_libraries(test-quit PRIVATE protocol_stubs PkgConfig::WAYLAND_CLIENT PkgConfig::XKBCOMMON)

# input-method relay test: app (text-input-v3) <-> IM (input-method-v2)
add_executable(test-ime-relay test-ime-relay.c)
target_include_directories(test-ime-relay PRIVATE ${PROTOCOL_GEN_DIR})
target_link_libraries(test-ime-relay PRIVATE protocol_stubs PkgConfig::WAYLAND_CLIENT PkgConfig::XKBCOMMON)

# grab-shortcut test: while the IM holds the keyboard grab (cursor in a
# text field, fcitx5 composing), a global shortcut (Ctrl+Alt+P -> rofi)
# must still fire and the consumed key must not reach the IM grab
add_executable(test-grab-shortcut test-grab-shortcut.c)
target_include_directories(test-grab-shortcut PRIVATE ${PROTOCOL_GEN_DIR})
target_link_libraries(test-grab-shortcut PRIVATE protocol_stubs PkgConfig::WAYLAND_CLIENT PkgConfig::XKBCOMMON)

# fcitx5 end-to-end: app + virtual keyboard driver (needs a real fcitx5)
add_executable(test-ime-app test-ime-app.c)
target_include_directories(test-ime-app PRIVATE ${PROTOCOL_GEN_DIR})
target_link_libraries(test-ime-app PRIVATE protocol_stubs PkgConfig::WAYLAND_CLIENT PkgConfig::XKBCOMMON)

# force every generated artifact to exist before the compilers run; the
# executables only reference some of them directly, so a plain dependency
# on the target is enough for the rest
add_custom_target(protocols DEPENDS ${PROTOCOL_GEN_FILES})
add_dependencies(monodywm protocols)
add_dependencies(protocol_stubs protocols)
add_dependencies(test-client protocols)
add_dependencies(test-interaction protocols)
add_dependencies(test-cursor protocols)
add_dependencies(test-cursor-shape protocols)
add_dependencies(test-select-drag protocols)
add_dependencies(test-bar-clamp protocols)
add_dependencies(test-mask-guard protocols)
add_dependencies(test-restack protocols)
add_dependencies(test-border protocols)
add_dependencies(test-quit protocols)
add_dependencies(test-ime-relay protocols)
add_dependencies(test-ime-app protocols)
