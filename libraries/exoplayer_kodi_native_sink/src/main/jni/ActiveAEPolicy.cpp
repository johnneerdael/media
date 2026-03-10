/*
 *  Copyright (C) 2010-2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 *
 *  Local modifications for Nuvio:
 *  - Extracted and reduced the passthrough policy slice from ActiveAE.cpp.
 *  - Removed Kodi settings-service and engine-object dependencies.
 *  - Wrapped the definitions in the androidx_media3 namespace.
 */

#include "ActiveAEPolicy.h"

namespace androidx_media3 {

bool CActiveAEPolicy::SupportsRaw(AEAudioFormat& format, const CAEDeviceInfo** selected_device) const
{
  if (!m_settings.passthrough)
    return false;

  if (m_settings.config == CActiveAESettings::AE_CONFIG_FIXED)
    return false;

  if (format.m_streamInfo.m_type == CAEStreamInfo::STREAM_TYPE_AC3 && !m_settings.ac3passthrough)
    return false;
  if (format.m_streamInfo.m_type == CAEStreamInfo::STREAM_TYPE_DTS_512 && !m_settings.dtspassthrough)
    return false;
  if (format.m_streamInfo.m_type == CAEStreamInfo::STREAM_TYPE_DTS_1024 && !m_settings.dtspassthrough)
    return false;
  if (format.m_streamInfo.m_type == CAEStreamInfo::STREAM_TYPE_DTS_2048 && !m_settings.dtspassthrough)
    return false;
  if (format.m_streamInfo.m_type == CAEStreamInfo::STREAM_TYPE_DTSHD_CORE && !m_settings.dtspassthrough)
    return false;
  if (format.m_streamInfo.m_type == CAEStreamInfo::STREAM_TYPE_EAC3 && !m_settings.eac3passthrough)
    return false;
  if (format.m_streamInfo.m_type == CAEStreamInfo::STREAM_TYPE_TRUEHD && !m_settings.truehdpassthrough)
    return false;
  if (format.m_streamInfo.m_type == CAEStreamInfo::STREAM_TYPE_DTSHD && !m_settings.dtshdpassthrough)
    return false;
  if (format.m_streamInfo.m_type == CAEStreamInfo::STREAM_TYPE_DTSHD_MA && !m_settings.dtshdpassthrough)
    return false;

  return m_sink.FindSupportingPassthroughDevice(m_settings.passthroughdevice, format, selected_device);
}

bool CActiveAEPolicy::UsesDtsCoreFallback() const
{
  return m_settings.usesdtscorefallback;
}

}  // namespace androidx_media3
