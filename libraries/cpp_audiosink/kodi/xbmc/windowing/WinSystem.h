#pragma once

#include "guilib/DispResource.h"

class CWinSystemBase
{
public:
  void Register(IDispResource* /* resource */) {}
  void Unregister(IDispResource* /* resource */) {}
};
