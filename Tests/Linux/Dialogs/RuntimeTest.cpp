#include <X11/Xatom.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <future>
#include <thread>
#include <sys/wait.h>
#include <unistd.h>

#include "IGraphicsLinux.h"
#include "IControls.h"
#include "IPlugTimer.h"

using namespace iplug;
using namespace iplug::igraphics;
using Clock = std::chrono::steady_clock;

static int sFailures = 0;

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
