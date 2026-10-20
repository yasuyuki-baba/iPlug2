cmake_minimum_required(VERSION 3.14)

if(NOT IS_ABSOLUTE "${TEST_BINARY_DIR}" OR
   NOT EXISTS "${IPLUG2_DIR}/Tests/Linux/Deployment/CMakeLists.txt")
  message(FATAL_ERROR "Pass an absolute TEST_BINARY_DIR and a valid IPLUG2_DIR.")
endif()

execute_process(COMMAND "${CMAKE_COMMAND}"
  -S "${IPLUG2_DIR}/Tests/Linux/Deployment" -B "${TEST_BINARY_DIR}"
  "-DIPLUG2_DIR=${IPLUG2_DIR}" RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "Deployment fixture configuration failed")
endif()

foreach(target Direct Deferred High-clap)
  file(REMOVE "${TEST_BINARY_DIR}/deploy/${target}.clap")
endforeach()
file(REMOVE "${TEST_BINARY_DIR}/deploy/High.clap"
  "${TEST_BINARY_DIR}/deploy/resources/txt/late.txt"
  "${TEST_BINARY_DIR}/out/resources/txt/late.txt"
  "${TEST_BINARY_DIR}/deploy/reaper/reaper_Reaper.so"
  "${TEST_BINARY_DIR}/deploy/reaper/Reaper/index.html")
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${TEST_BINARY_DIR}"
  --clean-first --target Direct Deferred High-clap Reaper RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "Deployment fixture build failed")
endif()
foreach(name Direct Deferred High)
  if(NOT EXISTS "${TEST_BINARY_DIR}/deploy/${name}.clap")
    message(FATAL_ERROR "${name}: configured target was not automatically deployed")
  endif()
endforeach()
file(READ "${TEST_BINARY_DIR}/deploy/resources/txt/late.txt" deployed)
if(NOT deployed STREQUAL "deployment resource\n")
  message(FATAL_ERROR "Resources added after configuration were not deployed")
endif()
if(NOT EXISTS "${TEST_BINARY_DIR}/out/reaper_Reaper.so" OR
   NOT EXISTS "${TEST_BINARY_DIR}/deploy/reaper/reaper_Reaper.so")
  message(FATAL_ERROR "Linux REAPER extension output and deployment names do not match")
endif()
file(READ "${TEST_BINARY_DIR}/deploy/reaper/Reaper/index.html" deployed_web)
if(NOT deployed_web STREQUAL "<p>REAPER deployment test</p>\n")
  message(FATAL_ERROR "REAPER extension web resources were not deployed")
endif()
