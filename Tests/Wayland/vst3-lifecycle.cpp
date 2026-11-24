// Exercise attach, timer dispatch and detach against a real Wayland connection.
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/gui/iplugviewcontentscalesupport.h"
#include "pluginterfaces/gui/iwaylandframe.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include <wayland-client.h>
#include <dlfcn.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <poll.h>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

using namespace Steinberg;
using namespace Steinberg::Vst;

static void Require(bool condition, const char* message)
{
  if (!condition)
  {
    std::fprintf(stderr, "VST3 Wayland smoke test: %s\n", message);
    std::exit(1);
  }
}

class Host final : public IPlugFrame, public Linux::IRunLoop, public IWaylandHost
{
public:
  tresult PLUGIN_API queryInterface(const TUID id, void** object) override
  {
    QUERY_INTERFACE(id, object, INLINE_UID_OF(FUnknown), IPlugFrame)
    QUERY_INTERFACE(id, object, INLINE_UID_OF(IPlugFrame), IPlugFrame)
    QUERY_INTERFACE(id, object, INLINE_UID_OF(Linux::IRunLoop), Linux::IRunLoop)
    QUERY_INTERFACE(id, object, INLINE_UID_OF(IWaylandHost), IWaylandHost)
    *object = nullptr;
    return kNoInterface;
  }

  uint32 PLUGIN_API addRef() override { return ++mReferences; }
  uint32 PLUGIN_API release() override { return --mReferences; }
  tresult PLUGIN_API resizeView(IPlugView* view, ViewRect* size) override { return view->onSize(size); }
  tresult PLUGIN_API registerEventHandler(Linux::IEventHandler*, Linux::FileDescriptor) override { return kNotImplemented; }
  tresult PLUGIN_API unregisterEventHandler(Linux::IEventHandler*) override { return kNotImplemented; }

  tresult PLUGIN_API registerTimer(Linux::ITimerHandler* handler, Linux::TimerInterval) override
  {
    Require(std::find(mTimers.begin(), mTimers.end(), handler) == mTimers.end(), "timer registered twice");
    mTimers.push_back(handler);
    handler->addRef();
    return kResultOk;
  }

  tresult PLUGIN_API unregisterTimer(Linux::ITimerHandler* handler) override
  {
    const auto found = std::find(mTimers.begin(), mTimers.end(), handler);
    Require(found != mTimers.end(), "timer unregistered twice");
    mTimers.erase(found);
    handler->release();
    return kResultOk;
  }

  wl_display* PLUGIN_API openWaylandConnection() override
  {
    Require(!mBorrowed, "connection borrowed twice");
    mBorrowed = true;
    return mDisplay;
  }

  tresult PLUGIN_API closeWaylandConnection(wl_display* display) override
  {
    Require(mBorrowed && display == mDisplay, "unexpected Wayland connection close");
    mBorrowed = false;
    return kResultOk;
  }

  wl_display* mDisplay = nullptr;
  std::vector<Linux::ITimerHandler*> mTimers;
  unsigned mReferences = 1;
  bool mBorrowed = false;
};

int main(int argc, char** argv)
{
  Require(argc == 2 || (argc == 3 && (strcmp(argv[2], "--concurrent") == 0 || strcmp(argv[2], "--host-reads") == 0)),
          "expected a VST3 module .so path [--concurrent]");
  const bool concurrent = argc == 3 && strcmp(argv[2], "--concurrent") == 0;
  const bool hostReads = argc == 3 && strcmp(argv[2], "--host-reads") == 0;
  Host host;
  host.mDisplay = wl_display_connect(nullptr);
  Require(host.mDisplay != nullptr, "could not connect to Weston");
  wl_compositor* compositor = nullptr;
  const wl_registry_listener registryListener{
    [](void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
      if (strcmp(interface, wl_compositor_interface.name) == 0)
        *static_cast<wl_compositor**>(data) = static_cast<wl_compositor*>(
          wl_registry_bind(registry, name, &wl_compositor_interface, std::min(4u, version)));
    },
    [](void*, wl_registry*, uint32_t) {}
  };
  auto* registry = wl_display_get_registry(host.mDisplay);
  wl_registry_add_listener(registry, &registryListener, &compositor);
  Require(wl_display_roundtrip(host.mDisplay) >= 0 && compositor, "could not bind compositor");
  auto* parent = wl_compositor_create_surface(compositor);
  Require(parent != nullptr, "could not create parent surface");

  auto* library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  Require(library != nullptr, library ? "" : dlerror());
  const auto entry = reinterpret_cast<bool (*)(void*)>(dlsym(library, "ModuleEntry"));
  const auto exitModule = reinterpret_cast<bool (*)()>(dlsym(library, "ModuleExit"));
  const auto factoryFn = reinterpret_cast<IPluginFactory* (*)()>(dlsym(library, "GetPluginFactory"));
  Require(entry && exitModule && factoryFn && entry(library), "could not initialize module");
  auto factory = owned(factoryFn());
  PClassInfo info{};
  Require(factory && factory->getClassInfo(0, &info) == kResultOk, "missing component class");
  void* instance = nullptr;
  Require(factory->createInstance(info.cid, INLINE_UID_OF(IComponent), &instance) == kResultOk && instance,
          "could not create component");
  auto component = owned(static_cast<IComponent*>(instance));
  Require(component->initialize(static_cast<IPlugFrame*>(&host)) == kResultOk, "component init failed");
  FUnknownPtr<IEditController> controller(component);
  Require(controller != nullptr, "missing edit controller");
  const auto idleTimers = host.mTimers.size();

  std::atomic<bool> stop{false};
  std::thread hostReader;
  if (concurrent)
  {
    hostReader = std::thread([&] {
      auto* queue = wl_display_create_queue(host.mDisplay);
      Require(queue != nullptr, "could not create the concurrent host queue");
      while (!stop)
      {
        Require(wl_display_roundtrip_queue(host.mDisplay, queue) >= 0 &&
                wl_display_dispatch_pending(host.mDisplay) >= 0, "concurrent host dispatch failed");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      wl_event_queue_destroy(queue);
    });
  }

  // The SDK's editorhost covers visible mapping and input. Here the parent
  // stays unmapped so we can exercise attachment and teardown repeatedly.
  const unsigned cycles = concurrent ? 50 : 3;
  for (unsigned cycle = 0; cycle < cycles; ++cycle)
  {
    auto view = owned(controller->createView("editor"));
    Require(view && view->isPlatformTypeSupported(kPlatformTypeWaylandSurfaceID) == kResultTrue,
            "Wayland editor platform not advertised");
    if (!concurrent)
    {
      FUnknownPtr<IPlugViewContentScaleSupport> scale(view);
      Require(scale && scale->setContentScaleFactor(cycle == 1 ? 2.f : 1.f) == kResultOk, "host scale rejected");
    }
    Require(view->setFrame(&host) == kResultOk &&
            view->attached(parent, kPlatformTypeWaylandSurfaceID) == kResultOk &&
            host.mBorrowed && host.mTimers.size() == idleTimers + 1, "editor attachment failed");
    // Leave a readable fd and a prepared host reader until both timer dispatch
    // and removal return. A second read from the UI thread would deadlock.
    wl_event_queue* heldQueue = nullptr;
    wl_callback* reply = nullptr;
    if (hostReads)
    {
      heldQueue = wl_display_create_queue(host.mDisplay);
      Require(heldQueue && wl_display_prepare_read_queue(host.mDisplay, heldQueue) == 0, "host prepare failed");
      reply = wl_display_sync(host.mDisplay);
      Require(reply && wl_display_flush(host.mDisplay) >= 0, "host sync failed");
      pollfd fd{wl_display_get_fd(host.mDisplay), POLLIN, 0};
      Require(poll(&fd, 1, 1000) == 1, "expected a readable connection");
    }
    for (auto* timer : host.mTimers)
      timer->onTimer();
    Require(view->removed() == kResultOk && !host.mBorrowed && host.mTimers.size() == idleTimers,
            "editor did not return its connection and timer");
    if (hostReads)
    {
      Require(wl_display_read_events(host.mDisplay) >= 0, "host read failed");
      wl_callback_destroy(reply);
      wl_event_queue_destroy(heldQueue);
    }
    view->setFrame(nullptr);
    Require(wl_display_roundtrip(host.mDisplay) >= 0, "Wayland error after editor removal");
  }

  stop = true;
  if (hostReader.joinable())
    hostReader.join();

  controller = nullptr;
  Require(component->terminate() == kResultOk && host.mTimers.empty(), "host timer leaked");
  component = nullptr;
  factory = nullptr;
  Require(host.mReferences == 1, "host reference leaked");
  Require(exitModule(), "module exit failed");
  dlclose(library);
  wl_surface_destroy(parent);
  wl_compositor_destroy(compositor);
  wl_registry_destroy(registry);
  wl_display_disconnect(host.mDisplay);
  std::printf("VST3 Wayland smoke test: %u attach/dispatch/remove cycles passed%s\n",
              cycles, concurrent ? " with a concurrent host reader" : "");
}
