#pragma once

#include <string>

class CLocalizeStrings
{
public:
  std::string Get(uint32_t id) const { return std::to_string(id); }
};
