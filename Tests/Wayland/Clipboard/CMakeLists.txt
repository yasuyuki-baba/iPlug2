cmake_minimum_required(VERSION 3.14)
project(WaylandClipboardTest LANGUAGES C CXX)

get_filename_component(IPLUG2_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
include(${IPLUG2_DIR}/iPlug2.cmake)
find_package(iPlug2 REQUIRED)

iplug_add_plugin(WaylandClipboardTest
  FORMATS CLAP
  SOURCES Clipboard.cpp
)
target_include_directories(WaylandClipboardTest-clap PRIVATE
  "${IPLUG2_DIR}/Examples/IPlugEffect"
)
