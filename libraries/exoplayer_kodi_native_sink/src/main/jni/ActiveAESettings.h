/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 *
 *  Local modifications for Nuvio:
 *  - Reduced the dependency surface to the native passthrough module.
 *  - Replaced Kodi's settings-service callbacks with a plain settings snapshot.
 *  - Wrapped the declarations in the androidx_media3 namespace.
 */

#pragma once

#include <string>

namespace androidx_media3 {

struct UserAudioSettings;

class CActiveAESettings
{
public:
  enum AudioConfig
  {
    AE_CONFIG_AUTO = 0,
    AE_CONFIG_FIXED = 1,
  };

  struct AudioSettings
  {
    std::string device;
    std::string passthroughdevice;
    int config = AE_CONFIG_AUTO;
    int channels = 0;
    int samplerate = 0;
    bool passthrough = false;
    bool ac3passthrough = false;
    bool ac3transcode = false;
    bool eac3passthrough = false;
    bool truehdpassthrough = false;
    bool dtspassthrough = false;
    bool dtshdpassthrough = false;
    bool usesdtscorefallback = false;
  };

  static AudioSettings Load(const UserAudioSettings& user_settings, bool has_passthrough_device);
};

}  // namespace androidx_media3
