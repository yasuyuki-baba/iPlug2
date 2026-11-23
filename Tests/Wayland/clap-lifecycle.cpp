// Exercise the plug-in's floating CLAP GUI from the host main thread.
#include <clap/clap.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <thread>

static void Require(bool condition, const char* message)
{
  if (!condition)
  {
    std::fprintf(stderr, "CLAP Wayland smoke test: %s\n", message);
    std::exit(1);
  }
}

struct Host
{
  clap_id mTimer = CLAP_INVALID_ID;
  clap_id mNextTimer = 0;
  std::atomic<bool> mCallbackRequested{false};
  bool mGuiClosed = false;

  static Host& From(const clap_host_t* host)
  {
    return *static_cast<Host*>(host->host_data);
  }

  static const void* GetExtension(const clap_host_t*, const char* id)
  {
    static const clap_host_timer_support_t timers{
      [](const clap_host_t* host, uint32_t period, clap_id* timer) {
        auto& state = From(host);
        Require(period > 0 && state.mTimer == CLAP_INVALID_ID, "unexpected timer registration");
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
      return &timers;
    if (std::strcmp(id, CLAP_EXT_GUI) == 0)
      return &gui;
    return nullptr;
  }
};

int main(int argc, char** argv)
{
  Require(argc >= 2 && argc <= 3, "expected a .clap path and optional mode");
  const bool interactive = argc == 3 && std::strcmp(argv[2], "interactive") == 0;
  const bool unavailable = argc == 3 && std::strcmp(argv[2], "unavailable") == 0;
  Require(argc == 2 || interactive || unavailable, "unknown mode");
  Host state;
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
    for (int cycle = 0; cycle < (interactive ? 1 : 3); ++cycle)
    {
      Require(gui->show(plugin) && state.mTimer != CLAP_INVALID_ID, "could not show floating editor");
      Require(gui->show(plugin), "could not show an already visible editor");
      for (int frame = 0; frame < (interactive ? 6000 : 35) && !state.mGuiClosed; ++frame)
      {
        if (state.mCallbackRequested.exchange(false))
          plugin->on_main_thread(plugin);
        if (state.mTimer != CLAP_INVALID_ID)
          timers->on_timer(plugin, state.mTimer);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      if (interactive && state.mGuiClosed)
      {
        Require(!gui->show(plugin), "closed editor could show before destroy acknowledgement");
        Require(state.mTimer == CLAP_INVALID_ID, "compositor close left the timer registered");
        std::puts("CLAP Wayland smoke test: host received closed(true)");
        break;
      }
      Require(!state.mGuiClosed, "compositor closed the editor unexpectedly");
      Require(gui->hide(plugin) && state.mTimer == CLAP_INVALID_ID, "hide left the timer registered");
    }
  }
  gui->destroy(plugin);
  Require(state.mTimer == CLAP_INVALID_ID, "destroy left the timer registered");
  plugin->destroy(plugin);
  entry->deinit();
  dlclose(library);
  std::puts(unavailable ? "CLAP Wayland smoke test: missing compositor rejected" :
    "CLAP Wayland smoke test: editor lifecycle passed");
}
