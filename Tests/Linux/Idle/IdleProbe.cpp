#include "IdleProbe.h"
#include "IPlug_include_in_plug_src.h"
#ifndef NO_IGRAPHICS
#include "IControls.h"
#endif
#include <atomic>
#include <cstdio>

static std::atomic<unsigned> sIdleCalls {0}, sWrongThread {0}, sMessages {0};
static std::atomic<unsigned> sOpens {0}, sCloses {0}, sCloseErrors {0};
static LinuxIdleProbe* sProbe = nullptr;

static iplug::Config ProbeConfig()
{
  auto config = iplug::MakeConfig(1, 1);
#ifdef NO_IGRAPHICS
  config.plugHasUI = false;
#endif
  return config;
}

LinuxIdleProbe::LinuxIdleProbe(const iplug::InstanceInfo& info)
: iplug::Plugin(info, ProbeConfig())
{
  sProbe = this;
  GetParam(0)->InitDouble("Probe", 0., 0., 100., 0.01);
#ifndef NO_IGRAPHICS
  mMakeGraphicsFunc = [this] {
    return MakeGraphics(*this, PLUG_WIDTH, PLUG_HEIGHT, PLUG_FPS);
  };
  mLayoutFunc = [](iplug::igraphics::IGraphics* pGraphics) {
    pGraphics->AttachPanelBackground(iplug::igraphics::COLOR_GRAY);
    pGraphics->AttachControl(new iplug::igraphics::IPanelControl(
      iplug::igraphics::IRECT(200, 200, 400, 400), iplug::igraphics::COLOR_WHITE));
  };
#endif
  SendParameterValueFromAPI(0, 0.75, true);
}

LinuxIdleProbe::~LinuxIdleProbe()
{
  printf("Linux idle probe stopped: idle=%u wrong-thread=%u messages=%u\n",
         sIdleCalls.load(), sWrongThread.load(), sMessages.load());
  printf("Linux close probe: opens=%u closes=%u errors=%u\n",
         sOpens.load(), sCloses.load(), sCloseErrors.load());
  sProbe = nullptr;
}

void LinuxIdleProbe::OnIdle()
{
  if (std::this_thread::get_id() != mMainThread)
    ++sWrongThread;
  if (++sIdleCalls == 1)
    printf("Linux idle probe: main=%d\n", std::this_thread::get_id() == mMainThread);
}

void LinuxIdleProbe::OnUIOpen()
{
  ++sOpens;
  iplug::Plugin::OnUIOpen();
}

void LinuxIdleProbe::OnUIClose()
{
  ++sCloses;
  if (std::this_thread::get_id() != mMainThread)
    ++sCloseErrors;
#ifndef NO_IGRAPHICS
  if (!GetUI() || !GetUI()->GetBackgroundControl())
    ++sCloseErrors;
#endif
  if (sCloses > sOpens)
    ++sCloseErrors;
}

void LinuxIdleProbe::SendParameterValueFromDelegate(int paramIdx, double value, bool normalized)
{
  if (std::this_thread::get_id() != mMainThread)
    ++sWrongThread;
  if (++sMessages == 1)
    printf("Linux idle message: main=%d\n", std::this_thread::get_id() == mMainThread);
  iplug::Plugin::SendParameterValueFromDelegate(paramIdx, value, normalized);
}

extern "C" EXPORT unsigned LinuxIdleProbeCount(unsigned kind)
{
  switch (kind)
  {
    case 0: return sIdleCalls.load();
    case 1: return sWrongThread.load();
    case 2: return sMessages.load();
    case 3: return sOpens.load();
    case 4: return sCloses.load();
    case 5: return sCloseErrors.load();
    default:
      fprintf(stderr, "Unknown Linux probe counter: %u\n", kind);
      return 0;
  }
}

extern "C" EXPORT void LinuxIdleProbeQueue()
{
  if (sProbe)
    sProbe->SendParameterValueFromAPI(0, 0.5, true);
}
