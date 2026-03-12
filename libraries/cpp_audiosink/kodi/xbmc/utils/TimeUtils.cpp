/*
 *  Copyright (C) 2005-2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "utils/TimeUtils.h"

#include <time.h>

int64_t CurrentHostCounter(void)
{
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return ((int64_t)now.tv_sec * 1000000000L) + now.tv_nsec;
}

int64_t CurrentHostFrequency(void)
{
  return 1000000000LL;
}
