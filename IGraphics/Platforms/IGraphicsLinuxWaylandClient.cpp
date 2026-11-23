/*
 This file is part of the iPlug 2 library. Copyright (C) the iPlug 2 developers.
 See LICENSE.txt for more info.
*/

#if defined IPLUG_WAYLAND_CLIENT && !defined NO_IGRAPHICS

#include <cerrno>
#include <chrono>
#include <clocale>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <map>
#include <poll.h>
#include <signal.h>
#include <string>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-compose.h>
#ifdef IPLUG_WAYLAND_EGL
#include <mutex>
#include <X11/Xlib.h>
#include <glad/glad.h>
#include <GL/glx.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <wayland-egl.h>
#else
#include "include/core/SkPixmap.h"
#endif
#include "IGraphicsLinux.h"
#ifdef IPLUG_WAYLAND_TOPLEVEL
#include "xdg-shell-client-protocol.h"
#endif

using namespace iplug;
using namespace iplug::igraphics;

namespace
{
constexpr const char* kUTF8Mime = "text/plain;charset=utf-8";
constexpr const char* kTextMime = "text/plain";
constexpr const char* kFilesMime = "text/uri-list";
constexpr size_t kMaxWaylandTransfer = 1024 * 1024;
constexpr auto kWaylandTransferTimeout = std::chrono::milliseconds(500);

bool WaitForWaylandTransfer(int fd, short events, std::chrono::steady_clock::time_point deadline)
{
  pollfd descriptor{fd, events, 0};
  const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
  return remaining > 0 && poll(&descriptor, 1, remaining) > 0;
}

void SendWaylandText(int fd, const char* text, size_t length)
{
  const int flags = fcntl(fd, F_GETFL);
  if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
    return;

  // A clipboard reader may cancel by closing its pipe. Do not deliver
  // SIGPIPE to the DAW, or alter the process-wide signal handler.
  sigset_t sigpipe, previous, pending;
  sigemptyset(&sigpipe);
  sigaddset(&sigpipe, SIGPIPE);
  if (pthread_sigmask(SIG_BLOCK, &sigpipe, &previous) != 0)
    return;
  const bool wasPending = sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE);
  const auto deadline = std::chrono::steady_clock::now() + kWaylandTransferTimeout;
  size_t offset = 0;
  while (offset < length && WaitForWaylandTransfer(fd, POLLOUT, deadline))
  {
    const auto written = write(fd, text + offset, length - offset);
    if (written > 0)
      offset += written;
    else if (written < 0 && errno != EAGAIN && errno != EINTR)
    {
      if (errno == EPIPE && !wasPending)
      {
        timespec timeout{};
        sigtimedwait(&sigpipe, nullptr, &timeout);
      }
      break;
    }
  }
  pthread_sigmask(SIG_SETMASK, &previous, nullptr);
}
}

#ifdef IPLUG_WAYLAND_EGL
namespace
{
struct EGLDisplayUse
{
  int users = 0;
  bool initializedHere = false;
};

std::mutex sEGLDisplayMutex;
std::map<EGLDisplay, EGLDisplayUse> sEGLDisplays;

// Editors may share a wl_display. Keep EGL alive until the last editor closes
// and preserve EGL displays that the host already initialized.
EGLDisplay AcquireEGLDisplay(EGLenum platform, void* pNativeDisplay)
{
  EGLDisplay display = eglGetPlatformDisplay(platform, pNativeDisplay, nullptr);
  if (display == EGL_NO_DISPLAY)
    return EGL_NO_DISPLAY;

  const std::lock_guard<std::mutex> lock(sEGLDisplayMutex);
  auto& use = sEGLDisplays[display];
  if (use.users == 0 && !eglQueryString(display, EGL_VERSION))
  {
    if (!eglInitialize(display, nullptr, nullptr))
    {
      sEGLDisplays.erase(display);
      return EGL_NO_DISPLAY;
    }
    use.initializedHere = true;
  }
  ++use.users;
  return display;
}

void ReleaseEGLDisplay(EGLDisplay display)
{
  const std::lock_guard<std::mutex> lock(sEGLDisplayMutex);
  auto it = sEGLDisplays.find(display);
  if (--it->second.users == 0)
  {
    if (it->second.initializedHere)
      eglTerminate(display);
    sEGLDisplays.erase(it);
  }
}
}
#endif

struct IGraphicsLinux::WaylandClientSurface
{
  wl_event_queue* mQueue = nullptr;
  wl_compositor* mCompositor = nullptr;
  wl_subcompositor* mSubcompositor = nullptr;
  wl_shm* mShm = nullptr;
  wl_buffer* mBuffers[2]{};
  wl_surface* mSurface = nullptr;
  wl_subsurface* mSubsurface = nullptr;
#ifdef IPLUG_WAYLAND_EGL
  EGLDisplay mEGLDisplay = EGL_NO_DISPLAY;
  EGLConfig mEGLConfig{};
  EGLSurface mEGLSurface = EGL_NO_SURFACE;
  EGLContext mEGLContext = EGL_NO_CONTEXT;
  wl_egl_window* mEGLWindow = nullptr;
  int mEGLWidth = 0;
  int mEGLHeight = 0;
  bool mEGLWindowResized = false;
  struct ContextState
  {
    EGLenum api;
    EGLDisplay display;
    EGLContext context;
    EGLSurface draw;
    EGLSurface read;
    Display* glxDisplay;
    GLXContext glxContext;
    GLXDrawable glxDraw;
    GLXDrawable glxRead;
  };
  std::vector<ContextState> mContextStack;

  bool InitializeEGL(wl_display* pDisplay, bool offscreen)
  {
    mEGLDisplay = offscreen ? AcquireEGLDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY)
                            : AcquireEGLDisplay(EGL_PLATFORM_WAYLAND_KHR, pDisplay);
    if (mEGLDisplay == EGL_NO_DISPLAY)
      return false;

    const EGLint configAttrs[] = {
      EGL_SURFACE_TYPE, offscreen ? EGL_PBUFFER_BIT : EGL_WINDOW_BIT,
      EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
      EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
      EGL_STENCIL_SIZE, 8, EGL_NONE
    };
    EGLint count = 0;
    if (!eglChooseConfig(mEGLDisplay, configAttrs, &mEGLConfig, 1, &count) || count == 0)
      return false;

    const EGLint contextAttrs[] = {
      EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3,
      EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE
    };
    const auto api = eglQueryAPI();
    if (!eglBindAPI(EGL_OPENGL_API))
      return false;
    mEGLContext = eglCreateContext(mEGLDisplay, mEGLConfig, EGL_NO_CONTEXT, contextAttrs);
    eglBindAPI(api);
    if (mEGLContext == EGL_NO_CONTEXT)
      return false;

    const int width = mGraphics->WindowWidth() * mGraphics->GetScreenScale();
    const int height = mGraphics->WindowHeight() * mGraphics->GetScreenScale();
    if (offscreen)
      return ResizeEGL(width, height);

    mEGLWindow = wl_egl_window_create(mSurface, width, height);
    if (!mEGLWindow)
      return false;
    mEGLSurface = eglCreatePlatformWindowSurface(mEGLDisplay, mEGLConfig, mEGLWindow, nullptr);
    mEGLWidth = width;
    mEGLHeight = height;
    return mEGLSurface != EGL_NO_SURFACE;
  }

  bool ResizeEGL(int width, int height)
  {
    if (width == mEGLWidth && height == mEGLHeight)
      return true;
    if (mEGLWindow)
    {
      wl_egl_window_resize(mEGLWindow, width, height, 0, 0);
      mEGLWindowResized = true;
    }
    else
    {
      if (width <= 0 || height <= 0 || width > 8192 || height > 8192)
        return false;
      const EGLint attrs[] = {EGL_WIDTH, width, EGL_HEIGHT, height, EGL_NONE};
      auto surface = eglCreatePbufferSurface(mEGLDisplay, mEGLConfig, attrs);
      if (surface == EGL_NO_SURFACE)
        return false;
      if (mEGLSurface != EGL_NO_SURFACE)
        eglDestroySurface(mEGLDisplay, mEGLSurface);
      mEGLSurface = surface;
    }
    mEGLWidth = width;
    mEGLHeight = height;
    return true;
  }

  void DestroyEGL()
  {
    if (mEGLSurface != EGL_NO_SURFACE) eglDestroySurface(mEGLDisplay, mEGLSurface);
    if (mEGLContext != EGL_NO_CONTEXT) eglDestroyContext(mEGLDisplay, mEGLContext);
    if (mEGLWindow) wl_egl_window_destroy(mEGLWindow);
    if (mEGLDisplay != EGL_NO_DISPLAY) ReleaseEGLDisplay(mEGLDisplay);
    mEGLSurface = EGL_NO_SURFACE;
    mEGLContext = EGL_NO_CONTEXT;
    mEGLWindow = nullptr;
    mEGLDisplay = EGL_NO_DISPLAY;
    mEGLWidth = mEGLHeight = 0;
  }
#endif

  template <typename ReadPixels>
  bool PresentShmFrame(int width, int height, ReadPixels readPixels);

  bool mViewInitialized = false;
  bool mPresented = false;
#ifdef IPLUG_WAYLAND_TOPLEVEL
  xdg_wm_base* mShell = nullptr;
  xdg_surface* mXdgSurface = nullptr;
  xdg_toplevel* mToplevel = nullptr;
  int mPendingWidth = 0;
  int mPendingHeight = 0;
  bool mConfigured = false;

  bool CreateToplevel(const char* title, const char* appID)
  {
    if (!mShell)
      return false;

    mXdgSurface = xdg_wm_base_get_xdg_surface(mShell, mSurface);
    if (!mXdgSurface)
      return false;
    static const xdg_surface_listener surfaceListener{
      [](void* data, xdg_surface* surface, uint32_t serial) {
        auto& window = *static_cast<WaylandClientSurface*>(data);
        xdg_surface_ack_configure(surface, serial);
        window.mConfigured = true;
        auto& graphics = *window.mGraphics;
        if (!graphics.mCloseRequested)
        {
          if (window.mPendingWidth > 0 && window.mPendingHeight > 0)
            graphics.ResizeFromHost(window.mPendingWidth, window.mPendingHeight);
          graphics.mNeedsRedraw = true;
        }
      }
    };
    xdg_surface_add_listener(mXdgSurface, &surfaceListener, this);

    mToplevel = xdg_surface_get_toplevel(mXdgSurface);
    if (!mToplevel)
      return false;
    static const auto toplevelListener = [] {
      xdg_toplevel_listener listener{};
      listener.configure = [](void* data, xdg_toplevel*, int32_t width, int32_t height, wl_array*) {
        auto& window = *static_cast<WaylandClientSurface*>(data);
        window.mPendingWidth = width;
        window.mPendingHeight = height;
      };
      listener.close = [](void* data, xdg_toplevel*) {
        static_cast<WaylandClientSurface*>(data)->mGraphics->RequestWindowClose();
      };
      return listener;
    }();
    xdg_toplevel_add_listener(mToplevel, &toplevelListener, this);
    xdg_toplevel_set_title(mToplevel, title);
    xdg_toplevel_set_app_id(mToplevel, appID);
    return true;
  }
#endif
  wl_seat* mSeat = nullptr;
  wl_pointer* mPointer = nullptr;
  wl_keyboard* mKeyboard = nullptr;
  xkb_context* mXkbContext = nullptr;
  xkb_state* mXkbState = nullptr;
  xkb_compose_state* mCompose = nullptr;
  IGraphicsLinux* mGraphics = nullptr;
  bool mOverSurface = false;
  bool mKeyboardFocused = false;
  bool mLeft = false;
  bool mRight = false;
  wl_data_device_manager* mDataManager = nullptr;
  wl_data_device* mDataDevice = nullptr;
  wl_data_offer* mSelection = nullptr;
  wl_data_offer* mDrop = nullptr;
  wl_data_source* mClipboardSource = nullptr;
  WDL_String mClipboardText;
  struct Offer
  {
    const char* textMime = nullptr;
    bool files = false;
    uint32_t action = WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE;
  };
  std::map<wl_data_offer*, Offer> mOffers;
  float mDropX = 0.f;
  float mDropY = 0.f;
  uint32_t mInputSerial = 0;
  uint32_t mLastClick = 0;
  float mLastClickX = 0.f;
  float mLastClickY = 0.f;
  int mRepeatRate = 0;
  int mRepeatDelay = 0;
  uint32_t mRepeatKey = 0;
  std::chrono::steady_clock::time_point mRepeatAt;
  std::vector<uint32_t> mPressedKeys;

  void ClearOffer(wl_data_offer*& offer)
  {
    if (!offer)
      return;
    mOffers.erase(offer);
    wl_data_offer_destroy(offer);
    offer = nullptr;
  }

  bool ReceiveOffer(wl_data_offer* offer, const char* mime, std::string& contents)
  {
    if (!offer || !mime)
      return false;
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0)
      return false;
    wl_data_offer_receive(offer, mime, fds[1]);
    close(fds[1]);
    const auto deadline = std::chrono::steady_clock::now() + kWaylandTransferTimeout;
    auto* display = mGraphics->mWaylandClientDisplay;
    bool success = false;
    int flushed = wl_display_flush(display);
    while (flushed < 0 && errno == EAGAIN && WaitForWaylandTransfer(wl_display_get_fd(display), POLLOUT, deadline))
      flushed = wl_display_flush(display);
    while (flushed >= 0 && WaitForWaylandTransfer(fds[0], POLLIN, deadline))
    {
      char bytes[4096];
      const auto count = read(fds[0], bytes, sizeof(bytes));
      if (count <= 0 || contents.size() + count > kMaxWaylandTransfer)
      {
        success = count == 0;
        break;
      }
      contents.append(bytes, count);
    }
    close(fds[0]);
    return success;
  }

  void SetDropLocation(wl_fixed_t x, wl_fixed_t y)
  {
    mDropX = wl_fixed_to_double(x) / mGraphics->GetTotalScale();
    mDropY = wl_fixed_to_double(y) / mGraphics->GetTotalScale();
  }

  void CreateDataDevice()
  {
    if (!mDataManager || !mSeat)
      return;
    mDataDevice = wl_data_device_manager_get_data_device(mDataManager, mSeat);
    if (!mDataDevice)
      return;
    static const wl_data_device_listener listener{
      [](void* data, wl_data_device*, wl_data_offer* offer) {
        auto& window = *static_cast<WaylandClientSurface*>(data);
        auto& info = window.mOffers[offer];
        static const wl_data_offer_listener offerListener{
          [](void* data, wl_data_offer*, const char* mime) {
            auto& info = *static_cast<Offer*>(data);
            if (strcmp(mime, kUTF8Mime) == 0)
              info.textMime = kUTF8Mime;
            else if (!info.textMime && strcmp(mime, kTextMime) == 0)
              info.textMime = kTextMime;
            else if (strcmp(mime, kFilesMime) == 0)
              info.files = true;
          },
          [](void*, wl_data_offer*, uint32_t) {},
          [](void* data, wl_data_offer*, uint32_t action) {
            static_cast<Offer*>(data)->action = action;
          }
        };
        wl_data_offer_add_listener(offer, &offerListener, &info);
      },
      [](void* data, wl_data_device*, uint32_t serial, wl_surface* surface, wl_fixed_t x, wl_fixed_t y, wl_data_offer* offer) {
        auto& window = *static_cast<WaylandClientSurface*>(data);
        window.ClearOffer(window.mDrop);
        window.mDrop = offer;
        window.SetDropLocation(x, y);
        auto info = window.mOffers.find(offer);
        if (info == window.mOffers.end())
          return;
        const bool allowsFiles = surface == window.mSurface && info->second.files;
        wl_data_offer_accept(offer, serial, allowsFiles ? kFilesMime : nullptr);
        if (allowsFiles && wl_data_offer_get_version(offer) >= WL_DATA_OFFER_SET_ACTIONS_SINCE_VERSION)
          wl_data_offer_set_actions(offer, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
        if (!allowsFiles)
          window.ClearOffer(window.mDrop);
      },
      [](void* data, wl_data_device*) {
        auto& window = *static_cast<WaylandClientSurface*>(data);
        window.ClearOffer(window.mDrop);
      },
      [](void* data, wl_data_device*, uint32_t, wl_fixed_t x, wl_fixed_t y) {
        static_cast<WaylandClientSurface*>(data)->SetDropLocation(x, y);
      },
      [](void* data, wl_data_device*) {
        auto& window = *static_cast<WaylandClientSurface*>(data);
        std::string contents;
        const auto info = window.mOffers.find(window.mDrop);
        if (!window.mGraphics->mCloseRequested && info != window.mOffers.end() &&
            window.ReceiveOffer(window.mDrop, kFilesMime, contents))
        {
          if (wl_data_offer_get_version(window.mDrop) >= WL_DATA_OFFER_FINISH_SINCE_VERSION &&
              info->second.action == WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY)
            wl_data_offer_finish(window.mDrop);
          window.mGraphics->DispatchLinuxDrop(reinterpret_cast<const unsigned char*>(contents.data()),
                                               contents.size(), window.mDropX, window.mDropY);
        }
        window.ClearOffer(window.mDrop);
      },
      [](void* data, wl_data_device*, wl_data_offer* offer) {
        auto& window = *static_cast<WaylandClientSurface*>(data);
        if (window.mSelection != offer)
          window.ClearOffer(window.mSelection);
        window.mSelection = offer;
      }
    };
    wl_data_device_add_listener(mDataDevice, &listener, this);
  }

  bool SetClipboard(const char* text)
  {
    if (!mDataDevice || !mInputSerial || strlen(text) > kMaxWaylandTransfer)
      return false;
    auto* source = wl_data_device_manager_create_data_source(mDataManager);
    if (!source)
      return false;
    static const wl_data_source_listener listener{
      [](void*, wl_data_source*, const char*) {},
      [](void* data, wl_data_source* source, const char*, int fd) {
        auto& window = *static_cast<WaylandClientSurface*>(data);
        if (source == window.mClipboardSource)
          SendWaylandText(fd, window.mClipboardText.Get(), window.mClipboardText.GetLength());
        close(fd);
      },
      [](void* data, wl_data_source* source) {
        auto& window = *static_cast<WaylandClientSurface*>(data);
        if (source == window.mClipboardSource)
          window.mClipboardSource = nullptr;
        wl_data_source_destroy(source);
      },
      [](void*, wl_data_source*) {},
      [](void*, wl_data_source*) {},
      [](void*, wl_data_source*, uint32_t) {}
    };
    if (mClipboardSource)
      wl_data_source_destroy(mClipboardSource);
    mClipboardText.Set(text);
    mClipboardSource = source;
    wl_data_source_add_listener(source, &listener, this);
    wl_data_source_offer(source, kUTF8Mime);
    wl_data_source_offer(source, kTextMime);
    wl_data_device_set_selection(mDataDevice, source, mInputSerial);
    wl_display_flush(mGraphics->mWaylandClientDisplay);
    return true;
  }

  bool Modifier(const char* name) const
  {
    return mXkbState && xkb_state_mod_name_is_active(mXkbState, name, XKB_STATE_MODS_EFFECTIVE) > 0;
  }

  IMouseMod MouseModifiers(bool left, bool right) const
  {
    return {left, right, Modifier(XKB_MOD_NAME_SHIFT), Modifier(XKB_MOD_NAME_CTRL), Modifier(XKB_MOD_NAME_ALT)};
  }

  void ClearKeyboardFocus(bool releaseKeys = false)
  {
    if (releaseKeys)
      for (const auto key : mPressedKeys)
        DispatchKey(key, false);
    mPressedKeys.clear();
    mKeyboardFocused = false;
    mRepeatKey = 0;
    mInputSerial = 0;
    if (mCompose)
      xkb_compose_state_reset(mCompose);
  }

  void DispatchKey(uint32_t key, bool pressed)
  {
    if (!mXkbState || !mKeyboardFocused || mGraphics->mCloseRequested)
      return;
    auto symbol = xkb_state_key_get_one_sym(mXkbState, key);
    const bool control = Modifier(XKB_MOD_NAME_CTRL);
    const bool alt = Modifier(XKB_MOD_NAME_ALT);
    const bool shift = Modifier(XKB_MOD_NAME_SHIFT);
    char text[64]{};
    int length = xkb_state_key_get_utf8(mXkbState, key, text, sizeof(text));
    if (pressed && mCompose && !control && !alt)
    {
      xkb_compose_state_feed(mCompose, symbol);
      switch (xkb_compose_state_get_status(mCompose))
      {
        case XKB_COMPOSE_COMPOSING:
          return;
        case XKB_COMPOSE_COMPOSED:
          symbol = xkb_compose_state_get_one_sym(mCompose);
          length = xkb_compose_state_get_utf8(mCompose, text, sizeof(text));
          xkb_compose_state_reset(mCompose);
          break;
        case XKB_COMPOSE_CANCELLED:
          xkb_compose_state_reset(mCompose);
          return;
        default:
          break;
      }
    }
    if (length >= 0 && length < static_cast<int>(sizeof(text)))
      mGraphics->DispatchLinuxKey(pressed, symbol, text, length, shift, control, alt);
  }

  void RepeatKey()
  {
    if (mRepeatKey && std::chrono::steady_clock::now() >= mRepeatAt)
    {
      mRepeatAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(1, 1000 / mRepeatRate));
      DispatchKey(mRepeatKey, true);
    }
  }

  static const wl_keyboard_listener& KeyboardListener()
  {
    static const wl_keyboard_listener listener{
      [](void* data, wl_keyboard*, uint32_t format, int fd, uint32_t size) {
        auto& window = *static_cast<WaylandClientSurface*>(data);
        if (format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 && size && window.mXkbContext)
        {
          auto* map = static_cast<const char*>(mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0));
          if (map != MAP_FAILED)
          {
            if (auto* keymap = xkb_keymap_new_from_buffer(window.mXkbContext, map, size - (map[size - 1] == '\0'),
                                                        XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS))
            {
              xkb_state_unref(window.mXkbState);
              window.mXkbState = xkb_state_new(keymap);
              xkb_keymap_unref(keymap);
            }
            munmap(const_cast<char*>(map), size);
          }
        }
        close(fd);
        window.mRepeatKey = 0;
      },
      [](void* data, wl_keyboard*, uint32_t serial, wl_surface* surface, wl_array*) {
        auto& window = *static_cast<WaylandClientSurface*>(data);
        window.ClearKeyboardFocus(true);
        window.mKeyboardFocused = surface == window.mSurface;
        if (window.mKeyboardFocused)
          window.mInputSerial = serial;
      },
      [](void* data, wl_keyboard*, uint32_t, wl_surface*) {
        static_cast<WaylandClientSurface*>(data)->ClearKeyboardFocus(true);
      },
      [](void* data, wl_keyboard*, uint32_t serial, uint32_t, uint32_t key, uint32_t state) {
        auto& window = *static_cast<WaylandClientSurface*>(data);
        if (!window.mKeyboardFocused)
          return;
        const uint32_t keycode = key + 8; // Evdev keycodes are offset by eight in XKB.
        const bool pressed = state == WL_KEYBOARD_KEY_STATE_PRESSED;
        window.mInputSerial = serial;
        if (pressed)
        {
          window.mPressedKeys.push_back(keycode);
          window.mRepeatKey = 0;
          if (window.mXkbState && window.mRepeatRate > 0 && !window.Modifier(XKB_MOD_NAME_CTRL) &&
              !window.Modifier(XKB_MOD_NAME_ALT) &&
              xkb_keymap_key_repeats(xkb_state_get_keymap(window.mXkbState), keycode))
          {
            window.mRepeatKey = keycode;
            window.mRepeatAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(window.mRepeatDelay);
          }
        }
        else
        {
          auto& keys = window.mPressedKeys;
          keys.erase(std::remove(keys.begin(), keys.end(), keycode), keys.end());
          if (window.mRepeatKey == keycode)
            window.mRepeatKey = 0;
        }
        window.DispatchKey(keycode, pressed);
      },
      [](void* data, wl_keyboard*, uint32_t, uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group) {
        auto& window = *static_cast<WaylandClientSurface*>(data);
        if (window.mXkbState)
          xkb_state_update_mask(window.mXkbState, depressed, latched, locked, 0, 0, group);
      },
      [](void* data, wl_keyboard*, int32_t rate, int32_t delay) {
        auto& window = *static_cast<WaylandClientSurface*>(data);
        window.mRepeatRate = rate;
        window.mRepeatDelay = std::max(0, delay);
        window.mRepeatKey = 0;
      }
    };
    return listener;
  }

  void ReleaseKeyboard()
  {
    if (wl_keyboard_get_version(mKeyboard) >= WL_KEYBOARD_RELEASE_SINCE_VERSION)
      wl_keyboard_release(mKeyboard);
    else
      wl_keyboard_destroy(mKeyboard);
    mKeyboard = nullptr;
    ClearKeyboardFocus();
  }

  static void SeatCapabilities(void* pData, wl_seat* pSeat, uint32_t capabilities)
  {
    auto& window = *static_cast<WaylandClientSurface*>(pData);
    if (capabilities & WL_SEAT_CAPABILITY_POINTER)
    {
      if (!window.mPointer)
      {
        window.mPointer = wl_seat_get_pointer(pSeat);
        if (window.mPointer)
          wl_pointer_add_listener(window.mPointer, &PointerListener(), &window);
      }
    }
    else if (window.mPointer)
    {
      window.ReleasePointer();
      window.mPointer = nullptr;
      window.mOverSurface = window.mLeft = window.mRight = false;
      window.mGraphics->ReleaseMouseCapture();
    }
    if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && !window.mKeyboard)
    {
      window.mKeyboard = wl_seat_get_keyboard(pSeat);
      if (window.mKeyboard)
        wl_keyboard_add_listener(window.mKeyboard, &KeyboardListener(), &window);
    }
    else if (!(capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && window.mKeyboard)
    {
      window.ClearKeyboardFocus(true);
      window.ReleaseKeyboard();
    }
  }

  void ReleasePointer()
  {
    if (wl_pointer_get_version(mPointer) >= WL_POINTER_RELEASE_SINCE_VERSION)
      wl_pointer_release(mPointer);
    else
      wl_pointer_destroy(mPointer);
  }

  static const wl_pointer_listener& PointerListener()
  {
    static const auto listener = [] {
      wl_pointer_listener pointer{};
      pointer.enter = [](void* pData, wl_pointer*, uint32_t, wl_surface* pSurface, wl_fixed_t x, wl_fixed_t y) {
        auto& window = *static_cast<WaylandClientSurface*>(pData);
        window.mOverSurface = pSurface == window.mSurface;
        if (window.mOverSurface)
          window.Move(x, y);
      };
      pointer.leave = [](void* pData, wl_pointer*, uint32_t, wl_surface* pSurface) {
        auto& window = *static_cast<WaylandClientSurface*>(pData);
        if (pSurface == window.mSurface)
        {
          window.mOverSurface = window.mLeft = window.mRight = false;
          window.mGraphics->ReleaseMouseCapture();
          window.mGraphics->OnMouseOut();
        }
      };
      pointer.motion = [](void* pData, wl_pointer*, uint32_t, wl_fixed_t x, wl_fixed_t y) {
        auto& window = *static_cast<WaylandClientSurface*>(pData);
        if (window.mOverSurface)
          window.Move(x, y);
      };
      pointer.button = [](void* pData, wl_pointer*, uint32_t serial, uint32_t time, uint32_t button, uint32_t state) {
        auto& window = *static_cast<WaylandClientSurface*>(pData);
        if (!window.mOverSurface || (button != BTN_LEFT && button != BTN_RIGHT) ||
            window.mGraphics->mCloseRequested)
          return;

        const bool pressed = state == WL_POINTER_BUTTON_STATE_PRESSED;
        auto& graphics = *window.mGraphics;
        window.mInputSerial = serial;
        const auto mod = window.MouseModifiers(button == BTN_LEFT, button == BTN_RIGHT);
        (button == BTN_LEFT ? window.mLeft : window.mRight) = pressed;
        const std::vector<IMouseInfo> points{{graphics.mCursorX, graphics.mCursorY, 0.f, 0.f, mod}};
        if (pressed)
        {
          const auto x = graphics.mCursorX;
          const auto y = graphics.mCursorY;
          const float slop = 5.f / graphics.GetTotalScale();
          if (button == BTN_LEFT && window.mLastClick && time - window.mLastClick < 250 &&
              std::abs(x - window.mLastClickX) < slop && std::abs(y - window.mLastClickY) < slop)
          {
            window.mLastClick = 0;
            graphics.OnMouseDblClick(x, y, mod);
          }
          else
          {
            window.mLastClick = button == BTN_LEFT ? time : 0;
            window.mLastClickX = x;
            window.mLastClickY = y;
            graphics.OnMouseDown(points);
          }
        }
        else
          graphics.OnMouseUp(points);
      };
      pointer.axis = [](void* pData, wl_pointer*, uint32_t, uint32_t axis, wl_fixed_t value) {
        auto& window = *static_cast<WaylandClientSurface*>(pData);
        auto& graphics = *window.mGraphics;
        if (window.mOverSurface && !graphics.mCloseRequested && axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
          graphics.OnMouseWheel(graphics.mCursorX, graphics.mCursorY,
                                window.MouseModifiers(false, false),
                                -wl_fixed_to_double(value) / 10.f);
      };
      pointer.frame = [](void*, wl_pointer*) {};
      pointer.axis_source = [](void*, wl_pointer*, uint32_t) {};
      pointer.axis_stop = [](void*, wl_pointer*, uint32_t, uint32_t) {};
      pointer.axis_discrete = [](void*, wl_pointer*, uint32_t, int32_t) {};
      return pointer;
    }();
    return listener;
  }

  void Move(wl_fixed_t x, wl_fixed_t y)
  {
    auto& graphics = *mGraphics;
    if (graphics.mCloseRequested)
      return;

    const float nextX = wl_fixed_to_double(x) / graphics.GetTotalScale();
    const float nextY = wl_fixed_to_double(y) / graphics.GetTotalScale();
    const float dx = nextX - graphics.mCursorX;
    const float dy = nextY - graphics.mCursorY;
    graphics.mCursorX = nextX;
    graphics.mCursorY = nextY;
    const auto mod = MouseModifiers(mLeft, mRight);
    if (mLeft || mRight)
      graphics.OnMouseDrag({{nextX, nextY, dx, dy, mod}});
    else
      graphics.OnMouseOver(nextX, nextY, mod);
  }

  ~WaylandClientSurface()
  {
#ifdef IPLUG_WAYLAND_EGL
    DestroyEGL();
#endif
    if (mPointer) ReleasePointer();
    if (mKeyboard) ReleaseKeyboard();
    if (mClipboardSource) wl_data_source_destroy(mClipboardSource);
    for (const auto& entry : mOffers)
      wl_data_offer_destroy(entry.first);
    if (mDataDevice)
    {
      if (wl_data_device_get_version(mDataDevice) >= WL_DATA_DEVICE_RELEASE_SINCE_VERSION)
        wl_data_device_release(mDataDevice);
      else
        wl_data_device_destroy(mDataDevice);
    }
    if (mDataManager) wl_data_device_manager_destroy(mDataManager);
    xkb_compose_state_unref(mCompose);
    xkb_state_unref(mXkbState);
    xkb_context_unref(mXkbContext);
    if (mSeat)
    {
      if (wl_seat_get_version(mSeat) >= WL_SEAT_RELEASE_SINCE_VERSION)
        wl_seat_release(mSeat);
      else
        wl_seat_destroy(mSeat);
    }
#ifdef IPLUG_WAYLAND_TOPLEVEL
    if (mToplevel) xdg_toplevel_destroy(mToplevel);
    if (mXdgSurface) xdg_surface_destroy(mXdgSurface);
    if (mShell) xdg_wm_base_destroy(mShell);
#endif
    if (mSubsurface) wl_subsurface_destroy(mSubsurface);
    if (mSurface) wl_surface_destroy(mSurface);
    for (auto* pBuffer : mBuffers)
      if (pBuffer) wl_buffer_destroy(pBuffer);
    if (mShm) wl_shm_destroy(mShm);
    if (mSubcompositor) wl_subcompositor_destroy(mSubcompositor);
    if (mCompositor) wl_compositor_destroy(mCompositor);
    if (mQueue) wl_event_queue_destroy(mQueue);
  }
};

void* IGraphicsLinux::OpenWaylandClientWindow()
{
  if (mWaylandClient)
    return mWaylandClient->mSurface;
  auto window = std::make_unique<WaylandClientSurface>();
  window->mGraphics = this;
  window->mXkbContext = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  if (window->mXkbContext)
  {
    const char* locale = setlocale(LC_CTYPE, nullptr);
    for (const char* variable : {"LC_ALL", "LC_CTYPE", "LANG"})
    {
      const char* value = getenv(variable);
      if (value && *value)
      {
        locale = value;
        break;
      }
    }
    if (auto* table = xkb_compose_table_new_from_locale(window->mXkbContext, locale ? locale : "C", XKB_COMPOSE_COMPILE_NO_FLAGS))
    {
      window->mCompose = xkb_compose_state_new(table, XKB_COMPOSE_STATE_NO_FLAGS);
      xkb_compose_table_unref(table);
    }
  }
  window->mQueue = wl_display_create_queue(mWaylandClientDisplay);
  if (!window->mQueue)
    return nullptr;

  auto* pRegistry = wl_display_get_registry(mWaylandClientDisplay);
  if (!pRegistry)
    return nullptr;

  wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(pRegistry), window->mQueue);
  static const wl_registry_listener registryListener{
    [](void* pData, wl_registry* pRegistry, uint32_t name, const char* interface, uint32_t version) {
      auto& window = *static_cast<WaylandClientSurface*>(pData);
      if (!window.mCompositor && strcmp(interface, wl_compositor_interface.name) == 0)
        window.mCompositor = static_cast<wl_compositor*>(
          wl_registry_bind(pRegistry, name, &wl_compositor_interface, std::min(version, 4u)));
      else if (!window.mSubcompositor && strcmp(interface, wl_subcompositor_interface.name) == 0)
        window.mSubcompositor = static_cast<wl_subcompositor*>(
          wl_registry_bind(pRegistry, name, &wl_subcompositor_interface, 1));
      else if (!window.mShm && strcmp(interface, wl_shm_interface.name) == 0)
        window.mShm = static_cast<wl_shm*>(wl_registry_bind(pRegistry, name, &wl_shm_interface, 1));
      else if (!window.mDataManager && strcmp(interface, wl_data_device_manager_interface.name) == 0)
        window.mDataManager = static_cast<wl_data_device_manager*>(
          wl_registry_bind(pRegistry, name, &wl_data_device_manager_interface, std::min(version, 3u)));
#ifdef IPLUG_WAYLAND_TOPLEVEL
      else if (!window.mShell && strcmp(interface, xdg_wm_base_interface.name) == 0)
      {
        window.mShell = static_cast<xdg_wm_base*>(wl_registry_bind(pRegistry, name, &xdg_wm_base_interface, 1));
        static const xdg_wm_base_listener shellListener{
          [](void*, xdg_wm_base* shell, uint32_t serial) { xdg_wm_base_pong(shell, serial); }
        };
        if (window.mShell)
          xdg_wm_base_add_listener(window.mShell, &shellListener, &window);
      }
#endif
      else if (!window.mSeat && strcmp(interface, wl_seat_interface.name) == 0)
      {
        window.mSeat = static_cast<wl_seat*>(wl_registry_bind(pRegistry, name, &wl_seat_interface, std::min(version, 7u)));
        static const wl_seat_listener seatListener{WaylandClientSurface::SeatCapabilities, [](void*, wl_seat*, const char*) {}};
        if (window.mSeat)
          wl_seat_add_listener(window.mSeat, &seatListener, &window);
      }
    },
    [](void*, wl_registry*, uint32_t) {}
  };
  wl_registry_add_listener(pRegistry, &registryListener, window.get());
  const int result = wl_display_roundtrip_queue(mWaylandClientDisplay, window->mQueue);
  wl_registry_destroy(pRegistry);
  if (result < 0 || !window->mCompositor)
    return nullptr;
  window->CreateDataDevice();
#ifndef IPLUG_WAYLAND_EGL
  if (!window->mShm)
    return nullptr;
#endif

  window->mSurface = wl_compositor_create_surface(window->mCompositor);
  if (!window->mSurface)
    return nullptr;
  if (mWaylandClientParent)
  {
    if (!window->mSubcompositor)
      return nullptr;
    window->mSubsurface = wl_subcompositor_get_subsurface(window->mSubcompositor, window->mSurface, mWaylandClientParent);
    if (!window->mSubsurface)
      return nullptr;

    // The host owns the parent. Our commits must not depend on a parent commit
    // for every editor frame.
    wl_subsurface_set_desync(window->mSubsurface);
  }
  else
  {
#ifdef IPLUG_WAYLAND_TOPLEVEL
    if (!window->CreateToplevel(mWaylandClientTitle.Get(), GetBundleID()))
      return nullptr;
#else
    return nullptr;
#endif
  }
#ifdef IPLUG_WAYLAND_EGL
  if (!window->InitializeEGL(mWaylandClientDisplay, false))
  {
    window->DestroyEGL();
    // A host's Wayland proxy may only support forwarding shared-memory buffers.
    if (!mWaylandClientParent || !window->mShm || !window->InitializeEGL(mWaylandClientDisplay, true))
    {
      fprintf(stderr, "Could not create the Wayland EGL surface or OpenGL 3.3 context\n");
      return nullptr;
    }
  }
#endif
  mWaylandClient = window.release();

  DispatchScope dispatch(mDispatchDepth);
  {
#ifdef IPLUG_WAYLAND_EGL
    ScopedGLContext context{this};
    if (eglGetCurrentContext() != mWaylandClient->mEGLContext ||
        !gladLoadGLLoader([](const char* name) { return reinterpret_cast<void*>(eglGetProcAddress(name)); }) ||
        !GLAD_GL_VERSION_3_3)
      return nullptr;
    if (mWaylandClient->mEGLWindow)
      eglSwapInterval(mWaylandClient->mEGLDisplay, 0);
#endif
    OnViewInitialized(nullptr);
    mWaylandClient->mViewInitialized = true;
#ifdef IPLUG_WAYLAND_EGL
    if (!GetDrawContext())
    {
      fprintf(stderr, "Could not initialize the Wayland OpenGL renderer\n");
      return nullptr;
    }
#endif
    GetDelegate()->LayoutUI(this);
    AttachPopupMenuControl();
#ifdef IPLUG_WAYLAND_EGL
    DrawResize();
#endif
  }
  SetAllControlsDirty();
  mNeedsRedraw = true;
  GetDelegate()->OnUIOpen();
  if (mCloseRequested)
    return nullptr;

#ifdef IPLUG_WAYLAND_TOPLEVEL
  if (mWaylandClient->mToplevel)
  {
    // A new xdg-surface must make an empty commit and receive configure
    // before attaching its first buffer.
    wl_surface_commit(mWaylandClient->mSurface);
    if (wl_display_flush(mWaylandClientDisplay) < 0 && errno != EAGAIN)
      return nullptr;
    return mWaylandClient->mSurface;
  }
#endif

  // Submit the initial child frame before the host maps its parent.
  DrawWaylandClientWindow();
  if (mCloseRequested || !mWaylandClient->mPresented ||
      wl_display_roundtrip_queue(mWaylandClientDisplay, mWaylandClient->mQueue) < 0)
    return nullptr;
  return mWaylandClient->mSurface;
}

#ifdef IPLUG_WAYLAND_TOPLEVEL
bool IGraphicsLinux::ConnectWaylandClient(const char* title)
{
  if (mWaylandClientDisplay || WindowIsOpen())
    return false;
  auto* pDisplay = wl_display_connect(nullptr);
  if (!pDisplay)
    return false;

  mWaylandClientTitle.Set(title);
  SetWaylandClientParent(pDisplay, nullptr, wl_display_disconnect);
  return true;
}

void IGraphicsLinux::SetWaylandClientTitle(const char* title)
{
  mWaylandClientTitle.Set(title);
  if (mWaylandClient && mWaylandClient->mToplevel)
  {
    xdg_toplevel_set_title(mWaylandClient->mToplevel, title);
    wl_surface_commit(mWaylandClient->mSurface);
  }
}
#endif

void* IGraphicsLinux::GetWaylandClientWindow() const
{
  return mWaylandClient->mSurface;
}

bool IGraphicsLinux::ReadWaylandClientClipboard(WDL_String& text)
{
  auto& window = *mWaylandClient;
  if (window.mClipboardSource)
  {
    text.Set(window.mClipboardText.Get());
    return true;
  }
  const auto offer = window.mOffers.find(window.mSelection);
  if (offer == window.mOffers.end())
    return false;
  std::string contents;
  if (!window.ReceiveOffer(offer->first, offer->second.textMime, contents))
    return false;
  text.Set(contents.c_str());
  return true;
}

bool IGraphicsLinux::WriteWaylandClientClipboard(const char* text)
{
  return mWaylandClient->SetClipboard(text ? text : "");
}

void IGraphicsLinux::CloseWaylandClientWindow()
{
  if (mWaylandClient)
  {
    if (mWaylandClient->mViewInitialized)
    {
#ifdef IPLUG_WAYLAND_EGL
      ScopedGLContext context{this};
#endif
      OnViewDestroyed();
    }
    delete mWaylandClient;
    mWaylandClient = nullptr;
    wl_display_flush(mWaylandClientDisplay);
  }
  auto* pDisplay = mWaylandClientDisplay;
  mWaylandClientDisplay = nullptr;
  mWaylandClientParent = nullptr;
  auto closeConnection = std::move(mCloseWaylandClientConnection);
  if (pDisplay && closeConnection)
    closeConnection(pDisplay);
}

void IGraphicsLinux::DrawWaylandClientWindow()
{
  auto* pDisplay = mWaylandClientDisplay;
  auto* pQueue = mWaylandClient->mQueue;
  while (wl_display_prepare_read_queue(pDisplay, pQueue) != 0)
  {
    if (wl_display_dispatch_queue_pending(pDisplay, pQueue) < 0)
    {
      RequestWindowClose();
      return;
    }
  }

  const int flushed = wl_display_flush(pDisplay);
  if (flushed < 0 && errno != EAGAIN)
  {
    wl_display_cancel_read(pDisplay);
    RequestWindowClose();
    return;
  }

  pollfd fd{wl_display_get_fd(pDisplay), POLLIN, 0};
  if (poll(&fd, 1, 0) > 0 && (fd.revents & (POLLIN | POLLERR | POLLHUP)))
  {
    if (wl_display_read_events(pDisplay) < 0)
    {
      RequestWindowClose();
      return;
    }
  }
  else
    wl_display_cancel_read(pDisplay);

  if (wl_display_dispatch_queue_pending(pDisplay, pQueue) < 0)
  {
    RequestWindowClose();
    return;
  }

  mWaylandClient->RepeatKey();
  if (mCloseRequested)
    return;
#ifdef IPLUG_WAYLAND_TOPLEVEL
  if (mWaylandClient->mToplevel && !mWaylandClient->mConfigured)
    return;
#endif

  IRECTList dirty;
  if (IsDirty(dirty))
    mNeedsRedraw = true;

  if (!mNeedsRedraw)
    return;
  auto& window = *mWaylandClient;
#ifdef IPLUG_WAYLAND_EGL
  if (!window.mEGLWindow && window.mBuffers[0] && window.mBuffers[1])
    return;
  if (!window.ResizeEGL(WindowWidth() * GetScreenScale(), WindowHeight() * GetScreenScale()))
  {
    RequestWindowClose();
    return;
  }
  ScopedGLContext context{this};
  if (eglGetCurrentContext() != window.mEGLContext || !GetDrawContext())
  {
    RequestWindowClose();
    return;
  }
#else
  if (window.mBuffers[0] && window.mBuffers[1])
    return;
#endif
  dirty.Clear();
  dirty.Add(GetBounds());
  SetAllControlsClean();
  mNeedsRedraw = false;
  Draw(dirty);
#ifdef IPLUG_WAYLAND_EGL
  const bool presented = window.mEGLWindow ? eglSwapBuffers(window.mEGLDisplay, window.mEGLSurface)
    : window.PresentShmFrame(window.mEGLWidth, window.mEGLHeight, [&window](void* pData, int stride) {
      glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
      glReadPixels(0, 0, window.mEGLWidth, window.mEGLHeight, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, pData);
      auto* pPixels = static_cast<unsigned char*>(pData);
      for (int y = 0; y < window.mEGLHeight / 2; ++y)
      {
        auto* pRow = pPixels + y * stride;
        std::swap_ranges(pRow, pRow + stride, pPixels + (window.mEGLHeight - 1 - y) * stride);
      }
      return true;
    });
  if (presented)
  {
    window.mPresented = true;
    if (window.mEGLWindowResized)
    {
      // EGL may already have acquired a buffer at the previous size. Repaint
      // once after presenting it so the newly sized buffer gets drawn.
      int width = 0, height = 0;
      wl_egl_window_get_attached_size(window.mEGLWindow, &width, &height);
      mNeedsRedraw |= width != window.mEGLWidth || height != window.mEGLHeight;
      window.mEGLWindowResized = false;
    }
  }
  else
  {
    fprintf(stderr, "Could not present a Wayland EGL frame\n");
    RequestWindowClose();
  }
#endif
}

#ifdef IPLUG_WAYLAND_EGL
void IGraphicsLinux::ActivateWaylandClientGL()
{
  auto& window = *mWaylandClient;
  auto& state = window.mContextStack.emplace_back(WaylandClientSurface::ContextState{
    eglQueryAPI(), eglGetCurrentDisplay(), eglGetCurrentContext(),
    eglGetCurrentSurface(EGL_DRAW), eglGetCurrentSurface(EGL_READ),
    glXGetCurrentDisplay(), glXGetCurrentContext(), glXGetCurrentDrawable(), glXGetCurrentReadDrawable()
  });
  if (state.context == window.mEGLContext)
    return;
  if (state.glxContext)
    glXMakeContextCurrent(state.glxDisplay, 0, 0, nullptr);
  if (!eglBindAPI(EGL_OPENGL_API) ||
      !eglMakeCurrent(window.mEGLDisplay, window.mEGLSurface, window.mEGLSurface, window.mEGLContext))
    fprintf(stderr, "Could not activate the Wayland EGL context\n");
}

void IGraphicsLinux::DeactivateWaylandClientGL()
{
  auto& window = *mWaylandClient;
  const auto state = window.mContextStack.back();
  window.mContextStack.pop_back();
  if (state.context == window.mEGLContext)
    return;
  if (state.context != EGL_NO_CONTEXT)
  {
    if (!eglBindAPI(state.api) || !eglMakeCurrent(state.display, state.draw, state.read, state.context))
      fprintf(stderr, "Could not restore the previous EGL context\n");
    return;
  }
  if (eglGetCurrentContext() == window.mEGLContext)
    eglMakeCurrent(window.mEGLDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  eglBindAPI(state.api);
  if (state.glxContext && !glXMakeContextCurrent(state.glxDisplay, state.glxDraw, state.glxRead, state.glxContext))
    fprintf(stderr, "Could not restore the previous GLX context\n");
}
#endif

template <typename ReadPixels>
bool IGraphicsLinux::WaylandClientSurface::PresentShmFrame(int width, int height, ReadPixels readPixels)
{
  auto& pBuffer = mBuffers[mBuffers[0] ? 1 : 0];
  if (pBuffer || width <= 0 || height <= 0 || width > 8192 || height > 8192)
    return false;

  const int stride = width * 4;
  const int size = stride * height;
  const int fd = memfd_create("iplug2-wayland-editor", MFD_CLOEXEC);
  if (fd < 0)
    return false;
  void* pData = MAP_FAILED;
  if (ftruncate(fd, size) == 0)
    pData = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

  if (pData != MAP_FAILED)
  {
    if (readPixels(pData, stride))
    {
      if (auto* pPool = wl_shm_create_pool(mShm, fd, size))
      {
        pBuffer = wl_shm_pool_create_buffer(pPool, 0, width, height, stride, WL_SHM_FORMAT_ARGB8888);
        wl_shm_pool_destroy(pPool);
      }
    }
    munmap(pData, size);
  }
  close(fd);
  if (!pBuffer)
    return false;

  static const wl_buffer_listener bufferListener{
    [](void* pSlot, wl_buffer* pReleased) {
      wl_buffer_destroy(pReleased);
      *static_cast<wl_buffer**>(pSlot) = nullptr;
    }
  };
  wl_buffer_add_listener(pBuffer, &bufferListener, &pBuffer);
  wl_surface_attach(mSurface, pBuffer, 0, 0);
  wl_surface_damage(mSurface, 0, 0, width, height);
  wl_surface_commit(mSurface);
  wl_display_flush(mGraphics->mWaylandClientDisplay);
  return true;
}

#ifndef IPLUG_WAYLAND_EGL
void IGraphicsLinux::PresentWaylandClientFrame(const SkPixmap& pixels)
{
  auto& window = *mWaylandClient;
  if (window.PresentShmFrame(pixels.width(), pixels.height(), [&pixels](void* pData, int stride) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    const auto info = SkImageInfo::Make(pixels.width(), pixels.height(), kBGRA_8888_SkColorType, kPremul_SkAlphaType);
    return pixels.readPixels(info, pData, stride);
#else
    auto* pPixel = static_cast<SkPMColor*>(pData);
    for (int y = 0; y < pixels.height(); ++y)
      for (int x = 0; x < pixels.width(); ++x)
        *pPixel++ = SkPreMultiplyColor(pixels.getColor(x, y));
    return true;
#endif
  }))
    window.mPresented = true;
}
#endif

#endif
