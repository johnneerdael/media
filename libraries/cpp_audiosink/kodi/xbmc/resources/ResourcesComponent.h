#pragma once

#include "resources/LocalizeStrings.h"

class CResourcesComponent
{
public:
  CLocalizeStrings& GetLocalizeStrings() { return m_localize_strings; }

private:
  CLocalizeStrings m_localize_strings;
};
