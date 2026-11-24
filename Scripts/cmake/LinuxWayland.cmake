#  ==============================================================================
#
#  This file is part of the iPlug 2 library. Copyright (C) the iPlug 2 developers.
#
#  See LICENSE.txt for  more info.
#
#  ==============================================================================

include_guard(GLOBAL)

function(iplug_get_wayland_graphics_backend target result)
  get_target_property(libraries ${target} LINK_LIBRARIES)
  set(visited "${target}")
  while(libraries)
    list(GET libraries 0 library)
    list(REMOVE_AT libraries 0)
    if(library MATCHES "^\\$<BUILD_INTERFACE:([^<>]+)>$")
      set(library "${CMAKE_MATCH_1}")
    endif()
    if(NOT TARGET "${library}")
      continue()
    endif()

    get_target_property(aliased_target "${library}" ALIASED_TARGET)
    if(aliased_target)
      set(library "${aliased_target}")
    endif()
    if(library STREQUAL IGRAPHICS_LIB OR library MATCHES "^iPlug2::IGraphics::(NanoVG|Skia)(::|$)")
      set(${result} "${library}" PARENT_SCOPE)
      return()
    endif()

    if(NOT library IN_LIST visited)
      list(APPEND visited "${library}")
      # Follow usage requirements, not private implementation dependencies.
      get_target_property(interface_libraries "${library}" INTERFACE_LINK_LIBRARIES)
      if(interface_libraries)
        list(APPEND libraries ${interface_libraries})
      endif()
    endif()
  endwhile()
  set(${result} "" PARENT_SCOPE)
endfunction()

function(iplug_configure_wayland_client target toplevel graphics_backend)
  if(NOT graphics_backend MATCHES "^iPlug2::IGraphics::(Skia::CPU|Skia::GL3|NanoVG::GL3)$")
    message(FATAL_ERROR
      "Wayland was enabled for ${target}, but the graphics backend '${graphics_backend}' is unsupported. "
      "Select Skia/CPU, Skia/GL3, or NanoVG/GL3 with IGRAPHICS_BACKEND and IGRAPHICS_RENDERER.")
  endif()

  find_package(PkgConfig REQUIRED)
  pkg_check_modules(IPLUG_WAYLAND REQUIRED IMPORTED_TARGET wayland-client xkbcommon)
  target_sources(${target} PRIVATE "${IPLUG2_DIR}/IGraphics/Platforms/IGraphicsLinuxWaylandClient.cpp")
  target_compile_definitions(${target} PRIVATE IPLUG_WAYLAND_CLIENT)
  target_link_libraries(${target} PRIVATE PkgConfig::IPLUG_WAYLAND)
  if(graphics_backend MATCHES "::GL3$")
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
