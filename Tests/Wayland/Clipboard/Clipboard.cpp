#include "IPlugEffect.h"
#include "IPlug_include_in_plug_src.h"

#include <cstdio>
#include <string>

IPlugEffect::IPlugEffect(const InstanceInfo& info)
: iplug::Plugin(info, MakeConfig(kNumParams, kNumPresets))
{
  GetParam(kGain)->InitDouble("Gain", 0., 0., 100., 0.01);
  mMakeGraphicsFunc = [&] { return MakeGraphics(*this, PLUG_WIDTH, PLUG_HEIGHT, PLUG_FPS); };
  mLayoutFunc = [](IGraphics* pGraphics) {
    pGraphics->AttachPanelBackground(IColor(255, 17, 218, 45));
    pGraphics->SetKeyHandlerFunc([pGraphics](const IKeyPress& key, bool isUp) {
      if (isUp || key.VK != kVK_F2)
        return false;

      std::string text;
      text.reserve(256 * 1024);
      for (unsigned i = 0; i < 8192; ++i)
      {
        char block[33];
        std::snprintf(block, sizeof(block), "%08x0123456789abcdef01234567", i);
        text += block;
      }
      if (pGraphics->SetTextInClipboard(text.c_str()))
        std::fputs("CLAP clipboard copied\n", stderr);
      return true;
    });
  };
}

void IPlugEffect::ProcessBlock(sample**, sample**, int)
{
}
