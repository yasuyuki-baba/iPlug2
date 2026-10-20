cmake_minimum_required(VERSION 3.14)
project(LinuxDeploymentRegression LANGUAGES C CXX)

include("${IPLUG2_DIR}/iPlug2.cmake")
add_library(iPlug2::CLAP INTERFACE IMPORTED)
find_package(iPlug2 REQUIRED)
set(IPLUG2_CLAP_SUPPORTED TRUE)
set(IPLUG_DEPLOY_PLUGINS ON)
set(IPLUG_DEPLOY_METHOD COPY)
set(IPLUG_CLAP_DEPLOY_PATH "${CMAKE_BINARY_DIR}/deploy" CACHE PATH "" FORCE)

add_library(Direct MODULE Probe.cpp)
iplug_configure_target(Direct CLAP Direct)

add_library(Deferred MODULE Probe.cpp)
iplug_configure_target(Deferred CLAP Deferred DEFER_DEPLOYMENT)
_iplug_add_resources(Deferred "${CMAKE_CURRENT_SOURCE_DIR}/late.txt")
iplug_deploy_target(Deferred CLAP Deferred)

_iplug_create_desktop_targets(High "CLAP" "${CMAKE_CURRENT_SOURCE_DIR}/Probe.cpp"
  "" "${CMAKE_CURRENT_SOURCE_DIR}/late.txt" "" "")

add_library(iPlug2::ReaperExt INTERFACE IMPORTED)
include("${IPLUG2_CMAKE_DIR}/ReaperExt.cmake")
set(IPLUG_REAPEREXT_DEPLOY_PATH "${CMAKE_BINARY_DIR}/deploy/reaper" CACHE PATH "" FORCE)
add_library(Reaper MODULE Probe.cpp)
iplug_configure_reaperext(Reaper Reaper
  WEB_RESOURCES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/web")
