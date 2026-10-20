include("${IPLUG2_DIR}/iPlug2.cmake")
find_package(iPlug2 REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0)

add_executable(LinuxDialogTest DialogTest.cpp)
target_link_libraries(LinuxDialogTest PRIVATE iPlug2::IGraphics::NanoVG::GL3 PkgConfig::GTK3)
target_compile_definitions(LinuxDialogTest PRIVATE IPLUG_EDITOR=1 IPLUG_DSP=0)

foreach(mode normal no-gtk)
  add_test(NAME linux-dialog-${mode}
    COMMAND "${XVFB_RUN}" -a "$<TARGET_FILE:LinuxDialogTest>" "${mode}")
  set_tests_properties(linux-dialog-${mode} PROPERTIES
    TIMEOUT 60 ENVIRONMENT "LIBGL_ALWAYS_SOFTWARE=1;GSETTINGS_BACKEND=memory")
endforeach()

add_executable(LinuxRuntimeTest RuntimeTest.cpp)
target_link_libraries(LinuxRuntimeTest PRIVATE iPlug2::IGraphics::NanoVG::GL3)
target_compile_definitions(LinuxRuntimeTest PRIVATE IPLUG_EDITOR=1 IPLUG_DSP=0)
set_target_properties(LinuxRuntimeTest PROPERTIES ENABLE_EXPORTS ON)

foreach(mode close clipboard core-timer core-timer-early core-timer-fail)
  add_test(NAME linux-${mode}
    COMMAND "${XVFB_RUN}" -a "$<TARGET_FILE:LinuxRuntimeTest>" "${mode}")
  set_tests_properties(linux-${mode} PROPERTIES TIMEOUT 15
    ENVIRONMENT "LIBGL_ALWAYS_SOFTWARE=1;LD_PRELOAD=$<TARGET_FILE:LinuxStartupHooks>;IPLUG_LINUX_TEST_MODE=${mode}")
endforeach()
