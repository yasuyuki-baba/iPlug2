#  ==============================================================================
#
#  This file is part of the iPlug 2 library. Copyright (C) the iPlug 2 developers.
#
#  See LICENSE.txt for  more info.
#
#  ==============================================================================

include_guard(GLOBAL)

function(iplug_configure_wayland_client target toplevel)
  if(NOT IGRAPHICS_LIB MATCHES "^iPlug2::IGraphics::(Skia::CPU|Skia::GL3|NanoVG::GL3)$")
    return()
  endif()

  find_package(PkgConfig REQUIRED)
  pkg_check_modules(IPLUG_WAYLAND REQUIRED IMPORTED_TARGET wayland-client xkbcommon)
  target_sources(${target} PRIVATE "${IPLUG2_DIR}/IGraphics/Platforms/IGraphicsLinuxWaylandClient.cpp")
  target_compile_definitions(${target} PRIVATE IPLUG_WAYLAND_CLIENT)
  target_link_libraries(${target} PRIVATE PkgConfig::IPLUG_WAYLAND)
  if(IGRAPHICS_LIB MATCHES "::GL3$")
    pkg_check_modules(IPLUG_WAYLAND_EGL REQUIRED IMPORTED_TARGET egl>=1.5 wayland-egl)
    target_compile_definitions(${target} PRIVATE IPLUG_WAYLAND_EGL)
    target_link_libraries(${target} PRIVATE PkgConfig::IPLUG_WAYLAND_EGL)
  endif()

  if(toplevel)
    target_compile_definitions(${target} PRIVATE IPLUG_WAYLAND_TOPLEVEL)
    pkg_check_modules(IPLUG_WAYLAND_PROTOCOLS REQUIRED wayland-protocols)
    pkg_get_variable(protocol_dir wayland-protocols pkgdatadir)
    find_program(IPLUG_WAYLAND_SCANNER wayland-scanner)
    set(protocol "${protocol_dir}/stable/xdg-shell/xdg-shell.xml")
    if(NOT IPLUG_WAYLAND_SCANNER OR NOT EXISTS "${protocol}")
      message(FATAL_ERROR "Wayland toplevels require wayland-scanner and the stable xdg-shell protocol")
    endif()
    set(generated "${CMAKE_CURRENT_BINARY_DIR}/${target}-wayland")
    set(header "${generated}/xdg-shell-client-protocol.h")
    set(source "${generated}/xdg-shell-protocol.c")
    add_custom_command(OUTPUT "${header}" "${source}"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${generated}"
      COMMAND "${IPLUG_WAYLAND_SCANNER}" client-header "${protocol}" "${header}"
      COMMAND "${IPLUG_WAYLAND_SCANNER}" private-code "${protocol}" "${source}"
      DEPENDS "${protocol}"
      VERBATIM)
    target_sources(${target} PRIVATE "${header}" "${source}")
    target_include_directories(${target} PRIVATE "${generated}")
  endif()
endfunction()
