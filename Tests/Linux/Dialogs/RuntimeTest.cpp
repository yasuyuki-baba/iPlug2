#include <X11/Xatom.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <future>
#include <thread>

#include "IGraphicsLinux.h"
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
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ++mCloses;
  }

  unsigned mCloses = 0;
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
  if (argc != 2)
    return 1;
  if (strcmp(argv[1], "close") == 0)
    TestClose();
  else if (strcmp(argv[1], "clipboard") == 0)
    TestClipboard();
  else
    TestTimer(strcmp(argv[1], "core-timer-fail") == 0);
  printf("%s: %s\n", argv[1], sFailures ? "FAILED" : "passed");
  return sFailures ? 1 : 0;
}
