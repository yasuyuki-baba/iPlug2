# Native Wayland smoke tests

## Standalone with Skia/CPU

On Linux, build `IPlugEffect` with GTK 3 and the pinned Skia libraries.
The [Linux build documentation](../../Documentation/cmake.md#linux) lists the
build dependencies and Skia setup.

```sh
cmake -S Examples/IPlugEffect -B build/wayland -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DIGRAPHICS_BACKEND=SKIA -DIGRAPHICS_RENDERER=CPU \
  -DIPLUG_EXPERIMENTAL_WAYLAND=ON -DIPLUG_DEPLOY_PLUGINS=OFF
cmake --build build/wayland --target IPlugEffect-app
```

In a Wayland desktop session, run `build/wayland/out/IPlugEffect --wayland`.
The standalone app uses audio and MIDI devices from its existing settings.
Add `--no-io` to run without devices. The experimental window supports pointer
interaction and resizing. Menus and preferences, keyboard/IME, clipboard and
drag-and-drop are outside the scope of this window implementation.

With Weston, a C compiler and the GTK 3 development package installed, run:

```sh
bash Tests/Wayland/smoke.sh build/wayland/out/IPlugEffect
IPLUG_WAYLAND_TEST_SCALE=2 bash Tests/Wayland/smoke.sh build/wayland/out/IPlugEffect
```

The test starts headless Weston with Pixman and a private Wayland socket.
It clears `DISPLAY`, checks GTK is using a Wayland display and verifies
that the client attaches a buffer and receives a buffer release. A test-only
Cairo observer checks pixels in the first frame, injects a GTK scroll signal
and checks the knob redraw, then resizes from 600×600 to 720×720 logical pixels
and closes the window. The 2× run checks the corresponding physical frame
sizes. The test exercises GTK input dispatch through the editor and samples
the frame passed to GTK. Physical input and compositor screenshot capture
are separate checks.

## Experimental VST3 embedding

Build against the VST3 SDK 3.8 Wayland interfaces. Also install the
`wayland-client` development package (`libwayland-dev` on Ubuntu).

```sh
cmake -S Examples/IPlugEffect -B build/wayland-vst3 -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DIGRAPHICS_BACKEND=SKIA -DIGRAPHICS_RENDERER=CPU \
  -DIPLUG_VST3_WAYLAND=ON -DIPLUG_DEPLOY_PLUGINS=OFF
cmake --build build/wayland-vst3 --target IPlugEffect-vst3
bash Tests/Wayland/vst3-smoke.sh \
  "$(find build/wayland-vst3/out/IPlugEffect.vst3 -name IPlugEffect.so -print -quit)" \
  Dependencies/IPlug/VST3_SDK
```

The smoke test borrows a Wayland connection from a minimal VST3 frame,
checks that the editor attaches and submits a buffer, then
removes it and verifies that the connection and timer have been returned.
It repeats this sequence three times and checks for protocol errors.
The test uses an unmapped parent on headless Weston.

For a visible test, start the Steinberg SDK's `editorhost` built with
`SMTG_ENABLE_WAYLAND_SUPPORT=ON` and pass it the `IPlugEffect.vst3` bundle.
The SDK example needs a compositor seat. In OrbStack, Weston with an active
RDP client provides one. This configuration was used to verify initial
display and dragging a knob with XWayland disabled. The prototype has been
checked at scale 1. DAW compatibility, other
scales, dialogs and text input remain open.

## Experimental CLAP floating editor

Install the CLAP SDK and helpers, the `wayland-client` development package,
Wayland protocols and `wayland-scanner`. Build and test the Skia/CPU example:

```sh
cmake -S Examples/IPlugEffect -B build/wayland-clap -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DIGRAPHICS_BACKEND=SKIA -DIGRAPHICS_RENDERER=CPU \
  -DIPLUG_CLAP_WAYLAND=ON -DIPLUG_DEPLOY_PLUGINS=OFF
cmake --build build/wayland-clap --target IPlugEffect-clap
bash Tests/Wayland/clap-smoke.sh build/wayland-clap/out/IPlugEffect.clap \
  Dependencies/IPlug/CLAP_SDK
```

A CLAP host can request `wayland` with `is_floating=true`. The plug-in
creates an xdg-shell toplevel and dispatches its events on host timer ticks
or main-thread callbacks. It also advertises the existing embedded X11
editor. To test without a DAW, the smoke test loads the actual module in a
small CLAP host, checks repeated show/hide and destruction, and verifies
the host's close notification when the Wayland connection is lost.

The prototype has been checked at scale 1. Decorations depend on the
compositor, and transient parenting is unavailable. DAW compatibility,
HiDPI, dialogs and text input remain open.

## NanoVG/GL3 and Skia/GL3 with EGL

Install the EGL 1.5 and Wayland EGL development packages (on Ubuntu,
`libegl-dev` and `libwayland-dev`), Wayland protocols and `wayland-scanner`.
Use the CLAP and VST3 SDKs above when building those formats.
For Skia/GL3, use `-DIGRAPHICS_BACKEND=SKIA` with `-DIGRAPHICS_RENDERER=GL3`
and build the pinned Skia libraries as described in the Linux build
documentation.

```sh
cmake -S Examples/IPlugEffect -B build/wayland-egl -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DIGRAPHICS_BACKEND=NANOVG -DIGRAPHICS_RENDERER=GL3 \
  -DIPLUG_EXPERIMENTAL_WAYLAND=ON -DIPLUG_CLAP_WAYLAND=ON -DIPLUG_VST3_WAYLAND=ON \
  -DIPLUG_DEPLOY_PLUGINS=OFF
cmake --build build/wayland-egl --target IPlugEffect-app IPlugEffect-clap IPlugEffect-vst3
LIBGL_ALWAYS_SOFTWARE=1 bash Tests/Wayland/smoke.sh build/wayland-egl/out/IPlugEffect --egl
```

The standalone EGL smoke test checks the xdg-shell configure handshake,
submission of a native Wayland buffer and shutdown after the compositor
disconnects. The CLAP and VST3 lifecycle scripts above also accept the
EGL-built modules. With Mesa and a headless Pixman Weston, set
`LIBGL_ALWAYS_SOFTWARE=1` for those runs as well.

On a VST3 host connection where window EGL cannot initialize, the editor
tries offscreen EGL via the Mesa surfaceless platform and submits frames
through `wl_shm`. This path reads each frame back from OpenGL.
