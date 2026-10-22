#pragma once

#include "IPlug_include_in_plug_hdr.h"

class LinuxFailedEditor final : public iplug::Plugin
{
public:
  explicit LinuxFailedEditor(const iplug::InstanceInfo& info)
  : iplug::Plugin(info, iplug::MakeConfig(0, 1))
  {}

  void* OpenWindow(void*) override { return nullptr; }
};
