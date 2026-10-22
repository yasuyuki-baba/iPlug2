#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/Xresource.h>
#include <atomic>
#include <chrono>
#include <clocale>
#include <cstdio>
#include <cstring>
#include <future>
#include <string>
#include <thread>
#include <sys/wait.h>
#include <unistd.h>
#include <dlfcn.h>
#include <glad/glad.h>
#include <GL/glx.h>

#include "IGraphicsLinux.h"
#include "IControls.h"
#include "IPlugTimer.h"

using namespace iplug;
using namespace iplug::igraphics;
using Clock = std::chrono::steady_clock;

static int sFailures = 0;
static bool sFailGLXActivation = false;
static bool sDisableInputMethod = false;
static unsigned sInputMethodAttempts = 0;
static unsigned sInputContextsDestroyed = 0;
static bool sWatchErrorHandler = false;
static unsigned sErrorHandlerChanges = 0;
static const char* sInputCommit = nullptr;
static int sInputCommitStatus = XLookupChars;
static unsigned sInputLookups = 0;

extern "C" XIM XOpenIM(Display* display, XrmDatabase database, char* name, char* className)
{
  ++sInputMethodAttempts;
  if (sDisableInputMethod)
    return nullptr;
  static auto open = reinterpret_cast<decltype(&XOpenIM)>(dlsym(RTLD_NEXT, "XOpenIM"));
  if (!open)
    abort();
  return open(display, database, name, className);
}

extern "C" void XDestroyIC(XIC context)
{
  ++sInputContextsDestroyed;
  static auto destroy = reinterpret_cast<decltype(&XDestroyIC)>(dlsym(RTLD_NEXT, "XDestroyIC"));
  if (!destroy)
    abort();
  destroy(context);
}

extern "C" int Xutf8LookupString(XIC context, XKeyPressedEvent* event, char* buffer,
                                 int capacity, KeySym* symbol, int* status)
{
  ++sInputLookups;
  if (sInputCommit)
  {
    const int length = static_cast<int>(strlen(sInputCommit));
    *symbol = sInputCommitStatus == XLookupChars ? XK_a : XK_Return;
    *status = length > capacity ? XBufferOverflow : sInputCommitStatus;
    if (length <= capacity)
      memcpy(buffer, sInputCommit, length);
    return length;
  }
  static auto lookup = reinterpret_cast<decltype(&Xutf8LookupString)>(
    dlsym(RTLD_NEXT, "Xutf8LookupString"));
  if (!lookup)
    abort();
  return lookup(context, event, buffer, capacity, symbol, status);
}

extern "C" XErrorHandler XSetErrorHandler(XErrorHandler handler)
{
  if (sWatchErrorHandler)
    ++sErrorHandlerChanges;
  static auto set = reinterpret_cast<decltype(&XSetErrorHandler)>(
    dlsym(RTLD_NEXT, "XSetErrorHandler"));
  if (!set)
    abort();
  return set(handler);
}

extern "C" int glXMakeCurrent(Display* display, GLXDrawable drawable, GLXContext context)
{
  if (sFailGLXActivation)
  {
    sFailGLXActivation = false;
    return 0;
  }
  static auto makeCurrent = reinterpret_cast<int (*)(Display*, GLXDrawable, GLXContext)>(
    dlsym(RTLD_NEXT, "glXMakeCurrent"));
  if (!makeCurrent)
  {
    fprintf(stderr, "Could not resolve glXMakeCurrent\n");
    abort();
  }
  return makeCurrent(display, drawable, context);
}

static void Check(bool condition, const char* message)
{
  if (!condition)
  {
    fprintf(stderr, "FAIL: %s\n", message);
    ++sFailures;
  }
}

class TestDelegate final : public IGEditorDelegate
{
public:
  explicit TestDelegate(bool hostDriven) : IGEditorDelegate(0)
  {
    mMakeGraphicsFunc = [this, hostDriven]() {
      auto* graphics = new IGraphicsLinux(*this, 320, 240, 60, 1.f);
      graphics->SetHostDriven(hostDriven);
      return graphics;
    };
    mLayoutFunc = [](IGraphics* graphics) {
      graphics->AttachPanelBackground(COLOR_GRAY);
    };
  }

  void BeginInformHostOfParamChangeFromUI(int) override
  {
    Check(false, "unexpected parameter gesture");
  }
  void EndInformHostOfParamChangeFromUI(int) override
  {
    Check(false, "unexpected parameter gesture");
  }
  void OnUIClose() override
  {
    Check(GetUI() != nullptr, "graphics remain available during OnUIClose");
    Check(GetUI() && GetUI()->GetBackgroundControl(), "controls remain alive during OnUIClose");
    Check(std::this_thread::get_id() == mMainThread, "OnUIClose runs on the owning main thread");
    if (mReentrantClose)
      CloseWindow();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ++mCloses;
  }

  unsigned mCloses = 0;
  bool mReentrantClose = false;

private:
  const std::thread::id mMainThread = std::this_thread::get_id();
};

struct GLXState
{
  Display* display = glXGetCurrentDisplay();
  GLXContext context = glXGetCurrentContext();
  GLXDrawable draw = glXGetCurrentDrawable();
  GLXDrawable read = glXGetCurrentReadDrawable();

  void CheckCurrent(const char* message) const
  {
    Check(display == glXGetCurrentDisplay() && context == glXGetCurrentContext() &&
          draw == glXGetCurrentDrawable() && read == glXGetCurrentReadDrawable(), message);
  }
};

static void TestGLXContext()
{
  XInitThreads();
  Display* display = XOpenDisplay(nullptr);
  Check(display != nullptr, "open separate host GLX display");
  if (!display)
    return;
  const int attributes[] = {GLX_DRAWABLE_TYPE, GLX_PBUFFER_BIT,
                             GLX_RENDER_TYPE, GLX_RGBA_BIT, 0};
  int count = 0;
  GLXFBConfig* configs = glXChooseFBConfig(display, DefaultScreen(display), attributes, &count);
  Check(configs && count > 0, "choose host GLX framebuffer configuration");
  if (!configs || count == 0)
  {
    if (configs) XFree(configs);
    XCloseDisplay(display);
    return;
  }
  GLXContext context = glXCreateNewContext(display, configs[0], GLX_RGBA_TYPE, nullptr, 1);
  const int size[] = {GLX_PBUFFER_WIDTH, 16, GLX_PBUFFER_HEIGHT, 16, 0};
  const GLXPbuffer draw = glXCreatePbuffer(display, configs[0], size);
  const GLXPbuffer read = glXCreatePbuffer(display, configs[0], size);
  XFree(configs);
  Check(context && draw && read && draw != read, "create host context with distinct draw/read buffers");
  if (context && draw && read)
  {
    for (bool hostCurrent : {false, true})
    {
      Check(glXMakeContextCurrent(display, hostCurrent ? draw : 0, hostCurrent ? read : 0,
                                  hostCurrent ? context : nullptr), "set initial host GLX state");
      const GLXState host;
      TestDelegate first(true), second(true);
      Check(first.OpenWindow(nullptr) != nullptr, "open first editor with host GLX state");
      host.CheckCurrent("editor initialization restores host GLX state");
      Check(second.OpenWindow(nullptr) != nullptr, "open second editor with host GLX state");
      host.CheckCurrent("second editor initialization restores host GLX state");
      if (first.GetUI() && second.GetUI())
      {
        auto& graphics = *static_cast<IGraphicsLinux*>(first.GetUI());
        auto& other = *static_cast<IGraphicsLinux*>(second.GetUI());
        graphics.Resize(360, 260, 1.f);
        host.CheckCurrent("resize restores host GLX state");
        graphics.OnDisplayTimer();
        host.CheckCurrent("render restores host GLX state");
        {
          IGraphics::ScopedGLContext outer {&graphics};
          const GLXState firstState;
          Check(firstState.context && firstState.context != context, "activate first editor context");
          graphics.DrawResize();
          firstState.CheckCurrent("nested framebuffer resize preserves editor context");
          {
            IGraphics::ScopedGLContext middle {&other};
            const GLXState secondState;
            Check(secondState.context && secondState.context != firstState.context,
                  "activate different editor context");
            {
              IGraphics::ScopedGLContext inner {&graphics};
              firstState.CheckCurrent("reenter first editor context");
            }
            secondState.CheckCurrent("nested editor scope restores second editor");
          }
          firstState.CheckCurrent("second editor scope restores first editor");
        }
        host.CheckCurrent("nested GLX scopes restore host state");
        first.CloseWindow();
        host.CheckCurrent("editor close restores host GLX state");
        Check(first.OpenWindow(nullptr) != nullptr, "reopen first editor");
        host.CheckCurrent("editor reopening restores host GLX state");
      }
      second.CloseWindow();
      first.CloseWindow();
      host.CheckCurrent("all editor teardown preserves host GLX state");

      TestDelegate failed(true);
      sFailGLXActivation = true;
      Check(failed.OpenWindow(nullptr) == nullptr, "GLX activation failure rejects editor");
      Check(!sFailGLXActivation, "GLX activation failure hook was exercised");
      Check(!failed.GetUI(), "failed editor is cleaned up");
      host.CheckCurrent("failed editor initialization preserves host GLX state");
    }
  }
  glXMakeContextCurrent(display, 0, 0, nullptr);
  if (context) glXDestroyContext(display, context);
  if (draw) glXDestroyPbuffer(display, draw);
  if (read) glXDestroyPbuffer(display, read);
  XCloseDisplay(display);
}

static void TestClose()
{
  XInitThreads();
  Display* hostDisplay = XOpenDisplay(nullptr);
  if (!hostDisplay)
  {
    Check(false, "open host display for close regression");
    return;
  }
  const Window parent = XCreateSimpleWindow(hostDisplay, DefaultRootWindow(hostDisplay),
                                             0, 0, 320, 240, 0, 0, 0);
  XMapWindow(hostDisplay, parent);
  XSync(hostDisplay, 0);
  TestDelegate delegate(false);
  for (unsigned i = 0; i < 10; ++i)
  {
    if (!delegate.OpenWindow(reinterpret_cast<void*>(static_cast<uintptr_t>(parent))))
    {
      Check(false, "open editor for close regression");
      break;
    }
    delegate.CloseWindow();
    Check(delegate.GetUI() == nullptr, "editor destroyed after close");
    Check(delegate.mCloses == i + 1, "close callback called once per editor");
  }
  XDestroyWindow(hostDisplay, parent);
  XCloseDisplay(hostDisplay);
}

static void TestExternalClipboard(IGraphics& graphics, const char* text)
{
  std::atomic<bool> stop {false};
  std::promise<bool> ready;
  auto future = ready.get_future();
  std::thread owner([&]() {
    Display* display = XOpenDisplay(nullptr);
    if (!display)
    {
      ready.set_value(false);
      return;
    }
    const Window window = XCreateSimpleWindow(display, DefaultRootWindow(display),
                                               0, 0, 1, 1, 0, 0, 0);
    const Atom clipboard = XInternAtom(display, "CLIPBOARD", 0);
    const Atom utf8 = XInternAtom(display, "UTF8_STRING", 0);
    XSetSelectionOwner(display, clipboard, window, 0);
    ready.set_value(XGetSelectionOwner(display, clipboard) == window);
    while (!stop)
    {
      while (XPending(display))
      {
        XEvent event;
        XNextEvent(display, &event);
        if (event.type != SelectionRequest)
          continue;
        const auto& request = event.xselectionrequest;
        XEvent response {};
        response.xselection.type = SelectionNotify;
        response.xselection.requestor = request.requestor;
        response.xselection.selection = request.selection;
        response.xselection.target = request.target;
        response.xselection.time = request.time;
        if (request.target == utf8)
        {
          response.xselection.property = request.property ? request.property : request.target;
          XChangeProperty(display, request.requestor, response.xselection.property, utf8, 8,
                          PropModeReplace, reinterpret_cast<const unsigned char*>(text),
                          static_cast<int>(strlen(text)));
        }
        XSendEvent(display, request.requestor, 0, 0, &response);
        XFlush(display);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    XDestroyWindow(display, window);
    XCloseDisplay(display);
  });

  const bool ownsSelection = future.get();
  Check(ownsSelection, "external clipboard owner initialized");
  if (ownsSelection)
  {
    WDL_String result("old contents");
    Check(graphics.GetTextFromClipboard(result), "read external clipboard");
    Check(strcmp(result.Get(), text) == 0, "external clipboard text is complete");
  }
  stop = true;
  owner.join();
}

static void TestClipboard()
{
  TestDelegate delegate(true);
  if (!delegate.OpenWindow(nullptr))
  {
    Check(false, "open host-driven clipboard editor");
    return;
  }
  auto& graphics = *static_cast<IGraphicsLinux*>(delegate.GetUI());
  for (const char* text : {"clipboard text", "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e", ""})
  {
    Check(graphics.SetTextInClipboard(text), "own clipboard selection");
    WDL_String result("old contents");
    const auto start = Clock::now();
    Check(graphics.GetTextFromClipboard(result), "read self-owned clipboard");
    Check(Clock::now() - start < std::chrono::milliseconds(100),
          "self-owned clipboard does not wait for event dispatch");
    Check(strcmp(result.Get(), text) == 0, "self-owned clipboard preserves UTF-8 and empty text");
  }
  TestExternalClipboard(graphics, "external UTF-8 \xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e");
  TestExternalClipboard(graphics, "");
  Check(graphics.SetTextInClipboard("regained"), "regain clipboard ownership");
  graphics.ProcessX11Events();
  WDL_String result;
  Check(graphics.GetTextFromClipboard(result) && strcmp(result.Get(), "regained") == 0,
        "stale SelectionClear does not erase newly owned text");
  delegate.CloseWindow();
}

static void TestNativeClose(bool hostDriven)
{
  TestDelegate delegate(hostDriven);
  delegate.mReentrantClose = true;
  if (!delegate.OpenWindow(nullptr))
  {
    Check(false, "open native close editor");
    return;
  }
  auto* graphics = static_cast<IGraphicsLinux*>(delegate.GetUI());
  Display* sender = XOpenDisplay(nullptr);
  if (!sender)
  {
    Check(false, "open native close sender");
    return;
  }
  XEvent event {};
  event.xclient.type = ClientMessage;
  event.xclient.window = static_cast<Window>(reinterpret_cast<uintptr_t>(graphics->GetWindow()));
  event.xclient.message_type = XInternAtom(sender, "WM_PROTOCOLS", 0);
  event.xclient.format = 32;
  event.xclient.data.l[0] = XInternAtom(sender, "WM_DELETE_WINDOW", 0);
  XSendEvent(sender, event.xclient.window, 0, 0, &event);
  XSync(sender, 0);
  bool requested = false;
  const auto deadline = Clock::now() + std::chrono::seconds(2);
  while (!requested && Clock::now() < deadline)
  {
    if (hostDriven)
      graphics->OnDisplayTimer();
    {
      std::lock_guard<std::recursive_mutex> lock(delegate.GfxMutex());
      requested = graphics->WindowCloseRequested();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  Check(requested, "WM_DELETE_WINDOW records a deferred close request");
  {
    std::lock_guard<std::recursive_mutex> lock(delegate.GfxMutex());
    Check(delegate.mCloses == 0, "graphics dispatch does not run OnUIClose");
    Check(graphics->WindowIsOpen() && graphics->GetBackgroundControl(),
          "native window and controls survive until owner dispatch");
  }
  Check(delegate.CloseWindowIfRequested(), "owner completes native close outside graphics dispatch");
  Check(delegate.mCloses == 1, "native close invokes OnUIClose once");
  Check(delegate.GetUI() == nullptr, "owner joins the graphics worker and destroys the editor");
  delegate.CloseWindow();
  Check(delegate.mCloses == 1, "later host teardown does not repeat OnUIClose");
  Check(!delegate.CloseWindowIfRequested(), "native close request cannot be processed twice");
  if (delegate.OpenWindow(nullptr))
  {
    delegate.CloseWindow();
    Check(delegate.mCloses == 2, "reopening starts a fresh close lifecycle");
  }
  else
    Check(false, "reopen after deferred native close");
  XCloseDisplay(sender);
}

static void TestClipboardReceiver(const char* expected)
{
  TestDelegate receiver(true);
  if (!receiver.OpenWindow(nullptr))
  {
    Check(false, "open external clipboard receiver");
    return;
  }
  WDL_String text;
  Check(receiver.GetUI()->GetTextFromClipboard(text), "serve clipboard while owner UI thread waits");
  Check(strcmp(text.Get(), expected) == 0, "external receiver gets complete clipboard text");
}

static void TestClipboardInstances()
{
  TestDelegate owner(true), receiver(true);
  if (!owner.OpenWindow(nullptr) || !receiver.OpenWindow(nullptr))
  {
    Check(false, "open two host-driven clipboard editors");
    return;
  }
  for (const char* text : {"between instances", "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e", ""})
  {
    Check(owner.GetUI()->SetTextInClipboard(text), "set cross-instance clipboard");
    WDL_String result;
    Check(receiver.GetUI()->GetTextFromClipboard(result), "paste without pumping the host event loop");
    Check(strcmp(result.Get(), text) == 0, "cross-instance clipboard preserves UTF-8 and empty text");

    const pid_t child = fork();
    if (child == 0)
    {
      execl("/proc/self/exe", "LinuxRuntimeTest", "clipboard-receiver", text, nullptr);
      _exit(127);
    }
    Check(child > 0, "start independent clipboard client");
    if (child > 0)
    {
      int status = 0;
      Check(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "independent client can paste while the owner main thread is blocked");
    }
  }
  Check(owner.GetUI()->SetTextInClipboard("after close"), "set clipboard before closing owner editor");
  owner.CloseWindow();
  WDL_String text;
  Check(receiver.GetUI()->GetTextFromClipboard(text) && strcmp(text.Get(), "after close") == 0,
        "clipboard remains available while its module is loaded");
}

static void TestClipboardFailure()
{
  TestDelegate delegate(true);
  if (!delegate.OpenWindow(nullptr))
  {
    Check(false, "open clipboard failure editor");
    return;
  }
  for (const char* mode : {"clipboard-display-fail", "clipboard-connection-fail", "clipboard-thread-fail"})
  {
    Check(setenv("IPLUG_LINUX_TEST_MODE", mode, 1) == 0, "configure clipboard fault injection");
    Check(!delegate.GetUI()->SetTextInClipboard("not owned"), "clipboard startup failure is reported");
    Check(unsetenv("IPLUG_LINUX_TEST_MODE") == 0, "clear clipboard fault injection");
  }
  Check(delegate.GetUI()->SetTextInClipboard("retry"), "clipboard startup can retry after failures");
  WDL_String text;
  Check(delegate.GetUI()->GetTextFromClipboard(text) && strcmp(text.Get(), "retry") == 0,
        "retried clipboard service owns the expected text");
}

static std::atomic<unsigned> sHostXErrors {0};

static int HostXErrorHandler(Display*, XErrorEvent*)
{
  ++sHostXErrors;
  return 0;
}

class KeyProbe final : public IControl
{
public:
  KeyProbe() : IControl(IRECT(50, 40, 110, 90)) {}
  void Draw(IGraphics&) override {}
  bool OnKeyDown(float x, float y, const IKeyPress& key) override
  {
    mText += key.utf8;
    mKeys.push_back(key.VK);
    mX = x;
    mY = y;
    return true;
  }
  bool OnKeyUp(float, float, const IKeyPress& key) override
  {
    mReleasedText += key.utf8;
    ++mReleases;
    return true;
  }
  std::string mText;
  std::string mReleasedText;
  std::vector<int> mKeys;
  unsigned mReleases = 0;
  float mX = 0.f, mY = 0.f;
};

static void CheckText(const std::string& actual, const char* expected, const char* message)
{
  Check(actual == expected, message);
  if (actual != expected)
  {
    fprintf(stderr, "Actual UTF-8 bytes:");
    for (unsigned char byte : actual)
      fprintf(stderr, " %02x", byte);
    fprintf(stderr, "\n");
  }
}

static void TestScaledKeys()
{
  TestDelegate delegate(true);
  if (!delegate.OpenWindow(nullptr))
  {
    Check(false, "open editor for scaled host keys");
    return;
  }
  auto& graphics = *static_cast<IGraphicsLinux*>(delegate.GetUI());
  auto* probe = new KeyProbe();
  graphics.AttachControl(probe);
  Display* host = XOpenDisplay(nullptr);
  Check(host != nullptr, "open pointer test connection");
  if (!host)
    return;
  for (const auto scales : {std::pair<float, float>{1.f, 1.f}, {2.f, 1.f},
                            {1.f, 1.5f}, {1.5f, 1.25f}})
  {
    graphics.Resize(320, 240, scales.second);
    graphics.SetScreenScale(scales.first);
    const float scale = graphics.GetTotalScale();
    const Window window = reinterpret_cast<uintptr_t>(graphics.GetWindow());
    XWarpPointer(host, 0, window, 0, 0, 0, 0,
                 static_cast<int>(80 * scale), static_cast<int>(64 * scale));
    XSync(host, 0);
    float x = 0.f, y = 0.f;
    graphics.GetMouseLocation(x, y);
    Check(std::fabs(x - 80.f) < 0.01f && std::fabs(y - 64.f) < 0.01f,
          "pointer coordinates include both screen and drawing scales");
    Check(delegate.OnKeyDown(IKeyPress("a", kVK_A)), "host key down reaches the hovered control");
    Check(delegate.OnKeyUp(IKeyPress("a", kVK_A)), "host key up reaches the hovered control");
    Check(std::fabs(probe->mX - 80.f) < 0.01f && std::fabs(probe->mY - 64.f) < 0.01f,
          "host key handler receives logical coordinates");
  }
  Check(probe->mText == "aaaa" && probe->mReleases == 4, "one key pair delivered at each scale");
  XCloseDisplay(host);
}

static void SendKey(Display* host, IGraphicsLinux& graphics, unsigned keycode, int type = KeyPress)
{
  XEvent event {};
  event.xkey.type = type;
  event.xkey.display = host;
  event.xkey.window = reinterpret_cast<uintptr_t>(graphics.GetWindow());
  event.xkey.root = DefaultRootWindow(host);
  event.xkey.keycode = keycode;
  event.xkey.same_screen = 1;
  Check(XSendEvent(host, event.xkey.window, 0,
                  type == KeyPress ? KeyPressMask : KeyReleaseMask, &event),
        "send keyboard event");
  XSync(host, 0);
  graphics.ProcessX11Events();
}

static void TestKeyboard(bool withoutIM, bool utf8Locale)
{
  Check(std::setlocale(LC_CTYPE, utf8Locale ? "C.UTF-8" : "C") != nullptr, "set the host locale");
  const std::string locale = std::setlocale(LC_CTYPE, nullptr);
  Check(XSetLocaleModifiers("@im=none") != nullptr, "use the local XIM Compose implementation");
  sDisableInputMethod = withoutIM;
  TestDelegate delegate(true);
  Display* host = XOpenDisplay(nullptr);
  Check(host != nullptr, "open keyboard test connection");
  if (!host)
    return;
  int minimum = 0, maximum = 0, symbolsPerKey = 0;
  XDisplayKeycodes(host, &minimum, &maximum);
  const int firstKey = maximum - 7;
  KeySym* previous = XGetKeyboardMapping(host, firstKey, 8, &symbolsPerKey);
  Check(previous != nullptr, "save keyboard mapping");
  if (!previous)
  {
    XCloseDisplay(host);
    return;
  }
  KeySym symbols[] = {XK_a, XK_eacute, XK_Multi_key, XK_apostrophe, XK_e,
                      0x01003042, XK_Return, XK_Left};
  XChangeKeyboardMapping(host, firstKey, 1, symbols, 8);
  XSync(host, 0);
  const unsigned destroyedBefore = sInputContextsDestroyed;
  for (unsigned iteration = 0; iteration < 2; ++iteration)
  {
    if (!delegate.OpenWindow(nullptr))
    {
      Check(false, "open keyboard editor");
      break;
    }
    Check(locale == std::setlocale(LC_CTYPE, nullptr), "opening an editor preserves the host locale");
    auto& graphics = *static_cast<IGraphicsLinux*>(delegate.GetUI());
    auto* probe = new KeyProbe();
    graphics.AttachControl(probe);
    const Window window = reinterpret_cast<uintptr_t>(graphics.GetWindow());
    XSetInputFocus(host, window, RevertToParent, 0);
    XEvent motion {};
    motion.xmotion.type = MotionNotify;
    motion.xmotion.display = host;
    motion.xmotion.window = window;
    motion.xmotion.x = 80;
    motion.xmotion.y = 64;
    motion.xmotion.same_screen = 1;
    Check(XSendEvent(host, window, 0, PointerMotionMask, &motion), "position keyboard target");
    XSync(host, 0);
    graphics.ProcessX11Events();
    SendKey(host, graphics, firstKey);
    SendKey(host, graphics, firstKey, KeyRelease);
    SendKey(host, graphics, firstKey + 1);
    SendKey(host, graphics, firstKey + 1, KeyRelease);
    SendKey(host, graphics, firstKey + 5);
    CheckText(probe->mText, "a\xc3\xa9\xe3\x81\x82", "ASCII, Latin-1 and Unicode keysyms produce UTF-8");
    CheckText(probe->mReleasedText, "a\xc3\xa9", "key releases also contain valid UTF-8");
    if (!withoutIM)
    {
      Check(sInputLookups > 0, "key presses use the input context");
      probe->mText.clear();
      SendKey(host, graphics, firstKey + 2);
      SendKey(host, graphics, firstKey + 3);
      Check(probe->mText.empty(), "Compose prefixes do not insert text");
      SendKey(host, graphics, firstKey + 4);
      Check(probe->mText == "\xc3\xa9", "Compose apostrophe plus e commits an accented character");

      std::string phrase;
      for (unsigned i = 0; i < 30; ++i)
        phrase += "\xe6\x97\xa5";
      phrase += "\xf0\x9f\x8e\xb5";
      probe->mText.clear();
      probe->mKeys.clear();
      const unsigned lookups = sInputLookups;
      sInputCommit = phrase.c_str();
      sInputCommitStatus = iteration ? XLookupBoth : XLookupChars;
      SendKey(host, graphics, firstKey);
      sInputCommit = nullptr;
      Check(sInputLookups == lookups + 2, "retry an IME commit after XBufferOverflow");
      Check(probe->mText == phrase && probe->mKeys.size() == 31,
            "deliver an entire Japanese phrase and a four-byte codepoint without truncation");
      for (int key : probe->mKeys)
        Check(key == kVK_NONE, "IME phrases do not carry unspecified or candidate-acceptance keys");
    }
    probe->mKeys.clear();
    SendKey(host, graphics, firstKey + 7);
    Check(probe->mKeys.size() == 1 && probe->mKeys[0] == kVK_LEFT,
          "non-text navigation keys retain virtual key codes");
    delegate.CloseWindow();
    Check(locale == std::setlocale(LC_CTYPE, nullptr), "closing an editor preserves the host locale");
    Check(sInputContextsDestroyed == destroyedBefore + (withoutIM ? 0 : iteration + 1),
          "close destroys each input context exactly once before reopening");
  }
  Check(sInputMethodAttempts == 2, "each reopened editor initializes its own input method");
  if (withoutIM)
    Check(sInputLookups == 0, "unavailable IM uses the explicit UTF-8 fallback");
  XChangeKeyboardMapping(host, firstKey, symbolsPerKey, previous, 8);
  XFree(previous);
  XCloseDisplay(host);
}

static void TestParentValidation()
{
  XInitThreads();
  Display* host = XOpenDisplay(nullptr);
  Check(host != nullptr, "open parent validation host");
  if (!host)
    return;
  const Window parent = XCreateSimpleWindow(host, DefaultRootWindow(host), 0, 0, 400, 300, 0, 0, 0);
  const Window destroyed = XCreateSimpleWindow(host, DefaultRootWindow(host), 0, 0, 1, 1, 0, 0, 0);
  XMapWindow(host, parent);
  XDestroyWindow(host, destroyed);
  XSync(host, 0);
  XErrorHandler previous = XSetErrorHandler(HostXErrorHandler);
  sWatchErrorHandler = true;
  for (const uintptr_t value : {static_cast<uintptr_t>(parent), static_cast<uintptr_t>(destroyed),
                                static_cast<uintptr_t>(UINT32_MAX) + 1})
  {
    TestDelegate delegate(true);
    void* editor = delegate.OpenWindow(reinterpret_cast<void*>(value));
    Check(editor != nullptr, "valid parent embeds and invalid parent safely falls back");
    if (editor)
    {
      Window root = 0, actualParent = 0, *children = nullptr;
      unsigned count = 0;
      Check(XQueryTree(host, reinterpret_cast<uintptr_t>(editor), &root, &actualParent, &children, &count),
            "query editor parent");
      Check(actualParent == (value == parent ? parent : DefaultRootWindow(host)),
            "only a valid 32-bit X11 parent is used");
      if (children)
        XFree(children);
    }
    XDestroyWindow(host, destroyed);
    XSync(host, 0);
    delegate.CloseWindow();
  }
  sWatchErrorHandler = false;
  Check(sErrorHandlerChanges == 0, "editor creation never changes the process-wide X11 error handler");
  Check(sHostXErrors == 3, "only unrelated host errors reach the host handler");
  Check(XSetErrorHandler(previous) == HostXErrorHandler, "parent validation preserves the host handler");
  XDestroyWindow(host, parent);
  XCloseDisplay(host);
}

static void TestDisappearingClipboardRequestor(bool hostHandler)
{
  TestDelegate owner(true);
  if (!owner.OpenWindow(nullptr))
  {
    Check(false, "open clipboard owner for cancelled transfers");
    return;
  }
  const char* text = "surviving clipboard \xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e";
  Check(owner.GetUI()->SetTextInClipboard(text), "own clipboard before cancelled transfers");
  Display* client = XOpenDisplay(nullptr);
  if (!client)
  {
    Check(false, "open clipboard requestor connection");
    owner.CloseWindow();
    return;
  }
  const Atom clipboard = XInternAtom(client, "CLIPBOARD", 0);
  const Atom utf8 = XInternAtom(client, "UTF8_STRING", 0);
  const Atom targets = XInternAtom(client, "TARGETS", 0);
  const Atom unsupported = XInternAtom(client, "_IPLUG_UNSUPPORTED_SELECTION", 0);
  const Atom property = XInternAtom(client, "_IPLUG_CLIPBOARD_PROBE", 0);
  XErrorHandler previousHandler = nullptr;
  if (hostHandler)
    previousHandler = XSetErrorHandler(HostXErrorHandler);

  Window destroyed = 0;
  for (Atom target : {targets, utf8, XA_STRING, unsupported})
  {
    for (unsigned i = 0; i < 8; ++i)
    {
      destroyed = XCreateSimpleWindow(client, DefaultRootWindow(client), 0, 0, 1, 1, 0, 0, 0);
      // The server cannot process the owner's reply until this requestor is already gone.
      XGrabServer(client);
      XConvertSelection(client, clipboard, target, property, destroyed, 0);
      XDestroyWindow(client, destroyed);
      XUngrabServer(client);
      XSync(client, 0);
    }
  }

  const Window requestor = XCreateSimpleWindow(client, DefaultRootWindow(client), 0, 0, 1, 1, 0, 0, 0);
  for (Atom target : {targets, utf8, XA_STRING, unsupported})
  {
    XConvertSelection(client, clipboard, target, property, requestor, 0);
    XFlush(client);
    XEvent response {};
    bool received = false;
    const auto deadline = Clock::now() + std::chrono::seconds(2);
    while (!received && Clock::now() < deadline)
    {
      received = XCheckTypedWindowEvent(client, requestor, SelectionNotify, &response);
      if (!received)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    Check(received, "clipboard worker serves later requests after requestor cancellation");
    if (!received)
      continue;
    Check(response.xselection.target == target, "selection notification preserves requested target");
    if (target == unsupported)
    {
      Check(response.xselection.property == 0, "unsupported target reports conversion failure");
      continue;
    }
    Check(response.xselection.property == property, "successful reply names the result property");
    Atom type = 0;
    int format = 0;
    unsigned long count = 0, remaining = 0;
    unsigned char* data = nullptr;
    Check(XGetWindowProperty(client, requestor, property, 0, 1024, 1, AnyPropertyType,
                             &type, &format, &count, &remaining, &data) == 0,
          "read recovered clipboard reply");
    if (target == targets)
    {
      Check(type == XA_ATOM && format == 32 && count == 3 && remaining == 0,
            "TARGETS reply contains three correctly encoded 32-bit atoms");
      if (data && count == 3)
      {
        const auto* atoms = reinterpret_cast<const Atom*>(data);
        Check(atoms[0] == targets && atoms[1] == utf8 && atoms[2] == XA_STRING,
              "TARGETS reply preserves supported formats on 64-bit Xlib");
      }
    }
    else
      Check(data && type == target && format == 8 && count == strlen(text) && remaining == 0 &&
            memcmp(data, text, count) == 0, "recovered clipboard reply preserves complete UTF-8 text");
    if (data)
      XFree(data);
  }
  if (hostHandler)
  {
    Check(sHostXErrors == 0, "clipboard reply failures never reach the host error handler");
    XDestroyWindow(client, destroyed);
    XSync(client, 0);
    Check(sHostXErrors == 1, "unrelated host X11 errors still reach its own handler");
    Check(XSetErrorHandler(previousHandler) == HostXErrorHandler,
          "clipboard transfers preserve the installed host error handler");
  }
  XDestroyWindow(client, requestor);
  XCloseDisplay(client);
  owner.CloseWindow();
}

static void TestTimer(bool expectFailure)
{
  std::atomic<unsigned> calls {0};
  std::unique_ptr<Timer> timer(Timer::Create([&](Timer&) { ++calls; }, 5));
  if (expectFailure)
  {
    Check(!timer, "failed timer creation returns nullptr");
    Check(calls == 0, "failed timer never calls back");
    return;
  }
  Check(bool(timer), "create core timer");
  if (!timer)
    return;
  const auto deadline = Clock::now() + std::chrono::milliseconds(300);
  while (calls < 3 && Clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  Check(calls >= 3, "core timer runs even when the child is scheduled first");
  timer->Stop();
  timer->Stop();
  const unsigned stopped = calls;
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  Check(calls == stopped, "no callbacks after idempotent Stop");
}

int main(int argc, char** argv)
{
  if (argc < 2 || argc > 3)
    return 1;
  if (strcmp(argv[1], "close") == 0)
    TestClose();
  else if (strcmp(argv[1], "glx-context") == 0)
    TestGLXContext();
  else if (strcmp(argv[1], "scaled-keys") == 0)
    TestScaledKeys();
  else if (strcmp(argv[1], "keyboard") == 0 || strcmp(argv[1], "keyboard-no-im") == 0 ||
           strcmp(argv[1], "keyboard-no-im-c") == 0)
    TestKeyboard(strcmp(argv[1], "keyboard") != 0, strcmp(argv[1], "keyboard-no-im-c") != 0);
  else if (strcmp(argv[1], "parent-validation") == 0)
    TestParentValidation();
  else if (strcmp(argv[1], "clipboard") == 0)
    TestClipboard();
  else if (strcmp(argv[1], "clipboard-instances") == 0)
    TestClipboardInstances();
  else if (strcmp(argv[1], "clipboard-fail") == 0)
    TestClipboardFailure();
  else if (strcmp(argv[1], "clipboard-disappearing") == 0 || strcmp(argv[1], "clipboard-host-handler") == 0)
    TestDisappearingClipboardRequestor(strcmp(argv[1], "clipboard-host-handler") == 0);
  else if (strcmp(argv[1], "clipboard-receiver") == 0 && argc == 3)
    TestClipboardReceiver(argv[2]);
  else if (strcmp(argv[1], "native-close") == 0 || strcmp(argv[1], "host-native-close") == 0)
    TestNativeClose(strcmp(argv[1], "host-native-close") == 0);
  else
    TestTimer(strcmp(argv[1], "core-timer-fail") == 0);
  printf("%s: %s\n", argv[1], sFailures ? "FAILED" : "passed");
  return sFailures ? 1 : 0;
}
