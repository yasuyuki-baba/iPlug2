// Exercise the plug-in's floating CLAP GUI from the host main thread.
#include <clap/clap.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <thread>
#include <unistd.h>

static void Require(bool condition, const char* message)
{
  if (!condition)
  {
    std::fprintf(stderr, "CLAP Wayland smoke test: %s\n", message);
    std::exit(1);
  }
}

enum class TimerMode { Host, Missing, Rejected };

struct Host
{
  TimerMode mTimerMode = TimerMode::Host;
  unsigned mTimerRegistrations = 0;
  clap_id mTimer = CLAP_INVALID_ID;
  clap_id mNextTimer = 0;
  std::atomic<bool> mCallbackRequested{false};
  bool mGuiClosed = false;

  static Host& From(const clap_host_t* host)
  {
    return *static_cast<Host*>(host->host_data);
  }

  static const void* GetExtension(const clap_host_t* host, const char* id)
  {
    static const clap_host_timer_support_t timers{
      [](const clap_host_t* host, uint32_t period, clap_id* timer) {
        auto& state = From(host);
        Require(period > 0 && state.mTimer == CLAP_INVALID_ID, "unexpected timer registration");
        ++state.mTimerRegistrations;
        if (state.mTimerMode == TimerMode::Rejected)
          return false;
        state.mTimer = ++state.mNextTimer;
        *timer = state.mTimer;
        return true;
      },
      [](const clap_host_t* host, clap_id timer) {
        auto& state = From(host);
        Require(timer != CLAP_INVALID_ID && timer == state.mTimer, "unexpected timer removal");
        state.mTimer = CLAP_INVALID_ID;
        return true;
      }
    };
    static const clap_host_gui_t gui{
      [](const clap_host_t*) {},
      [](const clap_host_t*, uint32_t, uint32_t) {
        Require(false, "floating editor asked the host to resize it");
        return false;
      },
      [](const clap_host_t*) { return false; },
      [](const clap_host_t*) { return false; },
      [](const clap_host_t* host, bool wasDestroyed) {
        Require(wasDestroyed && !From(host).mGuiClosed, "unexpected notification of editor close");
        From(host).mGuiClosed = true;
      }
    };
    if (std::strcmp(id, CLAP_EXT_TIMER_SUPPORT) == 0)
      return From(host).mTimerMode == TimerMode::Missing ? nullptr : &timers;
    if (std::strcmp(id, CLAP_EXT_GUI) == 0)
      return &gui;
    return nullptr;
  }
};

int main(int argc, char** argv)
{
  Require(argc >= 2 && argc <= 4, "expected a .clap path, scenario and timer mode");
  const char* scenario = argc >= 3 ? argv[2] : "lifecycle";
  const bool interactive = std::strcmp(scenario, "interactive") == 0;
  const bool unavailable = std::strcmp(scenario, "unavailable") == 0;
  const bool clipboard = std::strcmp(scenario, "clipboard") == 0;
  Require(clipboard || interactive || unavailable || std::strcmp(scenario, "lifecycle") == 0, "unknown scenario");
  const char* timerMode = argc == 4 ? argv[3] : "host-timer";
  Host state;
  if (std::strcmp(timerMode, "no-timer") == 0)
    state.mTimerMode = TimerMode::Missing;
  else if (std::strcmp(timerMode, "reject-timer") == 0)
    state.mTimerMode = TimerMode::Rejected;
  else
    Require(std::strcmp(timerMode, "host-timer") == 0, "unknown timer mode");
  const bool usesHostTimer = state.mTimerMode == TimerMode::Host;
  const clap_host_t host{
    CLAP_VERSION, &state, "Wayland smoke host", "iPlug2", "", "1",
    Host::GetExtension, [](const clap_host_t*) {}, [](const clap_host_t*) {},
    [](const clap_host_t* host) { Host::From(host).mCallbackRequested = true; }
  };
  auto* library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  Require(library != nullptr, library ? "" : dlerror());
  auto* entry = static_cast<const clap_plugin_entry_t*>(dlsym(library, "clap_entry"));
  Require(entry && entry->init(argv[1]), "could not initialize module");
  auto* factory = static_cast<const clap_plugin_factory_t*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
  Require(factory && factory->get_plugin_count(factory) > 0, "missing plug-in factory");
  auto* descriptor = factory->get_plugin_descriptor(factory, 0);
  auto* plugin = factory->create_plugin(factory, &host, descriptor->id);
  Require(plugin && plugin->init(plugin), "could not initialize plug-in");
  auto* gui = static_cast<const clap_plugin_gui_t*>(plugin->get_extension(plugin, CLAP_EXT_GUI));
  auto* timers = static_cast<const clap_plugin_timer_support_t*>(plugin->get_extension(plugin, CLAP_EXT_TIMER_SUPPORT));
  Require(gui && timers, "missing GUI or timer extension");
  Require(gui->is_api_supported(plugin, CLAP_WINDOW_API_WAYLAND, true) &&
          !gui->is_api_supported(plugin, CLAP_WINDOW_API_WAYLAND, false) &&
          gui->is_api_supported(plugin, CLAP_WINDOW_API_X11, false), "incorrect advertised GUI APIs");
  Require(gui->create(plugin, CLAP_WINDOW_API_WAYLAND, true), "could not create floating editor");
  Require(!gui->create(plugin, CLAP_WINDOW_API_WAYLAND, true), "accepted duplicate create");
  gui->suggest_title(plugin, "IPlugEffect CLAP Wayland");
  clap_window_t parent{};
  parent.api = CLAP_WINDOW_API_X11;
  Require(!gui->set_parent(plugin, &parent), "accepted a parent for the floating editor");

  if (unavailable)
  {
    Require(!gui->show(plugin) && state.mTimer == CLAP_INVALID_ID, "show succeeded without a compositor");
  }
  else
  {
    auto tick = [&] {
      if (state.mCallbackRequested.exchange(false))
        plugin->on_main_thread(plugin);
      if (state.mTimer != CLAP_INVALID_ID)
        timers->on_timer(plugin, state.mTimer);
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    };
    auto waitForCommand = [&](const char* variable) {
      const char* path = std::getenv(variable);
      Require(path != nullptr, "missing clipboard test command path");
      for (int tickCount = 0; tickCount < 2000 && !state.mGuiClosed; ++tickCount)
      {
        if (access(path, F_OK) == 0)
          return;
        tick();
      }
      Require(false, "clipboard test command was not received");
    };
    for (int cycle = 0; cycle < (interactive || clipboard ? 1 : 3); ++cycle)
    {
      Require(gui->show(plugin), "could not show floating editor");
      const auto timer = state.mTimer;
      Require((timer != CLAP_INVALID_ID) == usesHostTimer, "unexpected host timer state");
      std::fputs("CLAP editor shown\n", stderr);
      Require(gui->show(plugin), "could not show an already visible editor");
      if (cycle == 0 && !interactive && !clipboard)
      {
        Require(gui->hide(plugin) && gui->hide(plugin), "hide before the first frame failed");
        tick();
        Require(state.mTimer == timer && gui->show(plugin), "show before the first frame failed");
      }
      if (clipboard)
        waitForCommand("IPLUG_TEST_HIDE");
      else
        for (int frame = 0; frame < 35 && !state.mGuiClosed; ++frame)
          tick();
      Require(!state.mGuiClosed, "compositor closed the editor unexpectedly");
      Require(gui->hide(plugin) && state.mTimer == timer, "hide changed the Wayland event timer");
      std::fputs("CLAP editor hidden\n", stderr);
      if (clipboard)
        waitForCommand("IPLUG_TEST_DESTROY");
      else
        for (int frame = 0; frame < (interactive ? 6000 : 10) && !state.mGuiClosed; ++frame)
          tick();
      std::fputs("CLAP hidden interval ended\n", stderr);
      if (interactive)
      {
        Require(state.mGuiClosed, "hidden editor did not report compositor disconnect");
        Require(!gui->show(plugin), "closed editor could show before destroy acknowledgement");
        Require(state.mTimer == CLAP_INVALID_ID, "compositor close left the timer registered");
        std::puts("CLAP Wayland smoke test: hidden editor reported closed(true)");
      }
      else
        Require(state.mTimer == timer, "hidden editor lost its Wayland event timer");
    }
  }
  gui->destroy(plugin);
  if (clipboard)
  {
    std::fputs("CLAP editor destroyed\n", stderr);
    const char* exitPath = std::getenv("IPLUG_TEST_EXIT");
    Require(exitPath != nullptr, "missing clipboard test exit path");
    for (int i = 0; i < 500 && access(exitPath, F_OK) != 0; ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  Require(state.mTimer == CLAP_INVALID_ID, "destroy left the timer registered");
  const unsigned expectedRegistrations = unavailable || state.mTimerMode == TimerMode::Missing ? 0 : 1;
  Require(state.mTimerRegistrations == expectedRegistrations, "unexpected number of host timer registrations");
  // The host can still have a main-thread callback queued when the GUI closes.
  plugin->on_main_thread(plugin);
  Require(state.mGuiClosed == interactive, "unexpected close notification after destroy");
  plugin->destroy(plugin);
  entry->deinit();
  dlclose(library);
  std::puts(unavailable ? "CLAP Wayland smoke test: missing compositor rejected" :
    "CLAP Wayland smoke test: editor lifecycle passed");
}
