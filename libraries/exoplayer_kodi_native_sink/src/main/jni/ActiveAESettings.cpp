/*
 *  Copyright (C) 2010-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 *
 *  Local modifications for Nuvio:
 *  - Reduced the implementation to the Kodi passthrough settings fields used by the native
 *    selector path.
 *  - Removed Kodi settings-service integration.
 *  - Wrapped the definitions in the androidx_media3 namespace.
 */

#include "ActiveAESettings.h"

#include "AESinkAUDIOTRACK.h"
#include "KodiCapabilitySelector.h"

namespace androidx_media3 {

CActiveAESettings::AudioSettings CActiveAESettings::Load(const UserAudioSettings& user_settings,
                                                         bool has_passthrough_device)
{
  AudioSettings settings;
  settings.device = CAESinkAUDIOTRACK::kRawDeviceName;
  settings.passthroughdevice = CAESinkAUDIOTRACK::kRawDeviceName;
  settings.channels = user_settings.max_pcm_channel_layout;
  settings.passthrough = user_settings.passthrough_enabled && has_passthrough_device;
  settings.ac3passthrough = user_settings.ac3_passthrough_enabled;
  settings.eac3passthrough = user_settings.eac3_passthrough_enabled;
  settings.truehdpassthrough = user_settings.truehd_passthrough_enabled;
  settings.dtspassthrough = user_settings.dts_passthrough_enabled;
  settings.dtshdpassthrough = user_settings.dtshd_passthrough_enabled;
  settings.usesdtscorefallback = user_settings.dtshd_core_fallback_enabled;
  return settings;
}

}  // namespace androidx_media3
