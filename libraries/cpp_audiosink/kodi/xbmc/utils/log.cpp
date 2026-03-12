/*
 *  Copyright (C) 2005-2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "utils/log.h"

#include "ServiceBroker.h"
#include "utils/StringUtils.h"

#include <spdlog/sinks/dist_sink.h>

namespace
{
constexpr auto kLogPattern = "%Y-%m-%d %T.%e T:%-5t %7l <%n>: %v";

const char* ComponentName(uint32_t component)
{
  switch (component)
  {
    case LOGAUDIO:
      return "audio";
    case LOGFFMPEG:
      return "ffmpeg";
    case LOGVIDEO:
      return "video";
    default:
      return "general";
  }
}
} // namespace

CLog::CLog()
  : m_platform(IPlatformLog::CreatePlatformLog()),
    m_sinks(std::make_shared<spdlog::sinks::dist_sink_mt>()),
    m_defaultLogger(CreateLogger("general"))
{
  m_platform->AddSinks(m_sinks);
  spdlog::set_default_logger(m_defaultLogger);
  spdlog::set_pattern(kLogPattern);
  spdlog::flush_on(spdlog::level::debug);
  SetLogLevel(m_logLevel);
}

CLog::~CLog()
{
  Deinitialize();
  spdlog::drop_all();
}

void CLog::Initialize(const std::string& /* path */)
{
}

void CLog::Deinitialize()
{
  spdlog::apply_all([](const std::shared_ptr<spdlog::logger>& logger) { logger->flush(); });
}

void CLog::SetLogLevel(int level)
{
  if (level < LOG_LEVEL_NONE || level > LOG_LEVEL_MAX)
    return;

  m_logLevel = level;

  auto spdLevel = spdlog::level::info;
  if (level <= LOG_LEVEL_NONE)
    spdLevel = spdlog::level::off;
  else if (level >= LOG_LEVEL_DEBUG)
    spdLevel = spdlog::level::trace;

  if (m_defaultLogger != nullptr && m_defaultLogger->level() == spdLevel)
    return;

  spdlog::set_level(spdLevel);
}

bool CLog::IsLogLevelLogged(int loglevel) const
{
  const int currentLogLevel = m_logLevel.load();
  if (currentLogLevel >= LOG_LEVEL_DEBUG)
    return true;
  if (currentLogLevel <= LOG_LEVEL_NONE)
    return false;

  return (loglevel & LOGMASK) >= LOGINFO;
}

bool CLog::CanLogComponent(uint32_t component) const
{
  if (component == LOG_COMPONENT_GENERAL)
    return true;

  if (!m_componentLogEnabled.load())
    return false;

  return (m_componentLogLevels.load() & component) == component;
}

void CLog::SetIecVerboseLoggingEnabled(bool enabled)
{
  m_componentLogEnabled.store(enabled);
  m_componentLogLevels.store(enabled ? static_cast<uint32_t>(LOGAUDIO) : 0U);
  SetLogLevel(enabled ? LOG_LEVEL_DEBUG : LOG_LEVEL_NORMAL);
}

Logger CLog::GetLogger(const std::string& loggerName)
{
  auto logger = spdlog::get(loggerName);
  if (logger == nullptr)
    logger = CreateLogger(loggerName);

  return logger;
}

void CLog::Log(int level, const char* message)
{
  GetInstance().FormatAndLogInternal(MapLogLevel(level), LOG_COMPONENT_GENERAL, "{}",
                                     fmt::make_format_args(message));
}

void CLog::Log(int level, const std::string& message)
{
  GetInstance().FormatAndLogInternal(MapLogLevel(level), LOG_COMPONENT_GENERAL, "{}",
                                     fmt::make_format_args(message));
}

CLog& CLog::GetInstance()
{
  return CServiceBroker::GetLogging();
}

spdlog::level::level_enum CLog::MapLogLevel(int level)
{
  switch (level)
  {
    case LOGDEBUG:
      return spdlog::level::debug;
    case LOGINFO:
      return spdlog::level::info;
    case LOGWARNING:
      return spdlog::level::warn;
    case LOGERROR:
      return spdlog::level::err;
    case LOGFATAL:
      return spdlog::level::critical;
    case LOGNONE:
      return spdlog::level::off;
    default:
      return spdlog::level::info;
  }
}

void CLog::FormatAndLogInternal(spdlog::level::level_enum level,
                                uint32_t component,
                                fmt::string_view format,
                                fmt::format_args args)
{
  if (level < m_defaultLogger->level())
    return;

  auto message = fmt::vformat(format, args);
  FormatLineBreaks(message);
  GetLoggerById(component)->log(level, message);
}

void CLog::FormatAndLogInternal(const std::string& loggerName,
                                spdlog::level::level_enum level,
                                fmt::string_view format,
                                fmt::format_args args)
{
  if (level < m_defaultLogger->level())
    return;

  auto message = fmt::vformat(format, args);
  FormatLineBreaks(message);
  GetLogger(loggerName)->log(level, message);
}

Logger CLog::CreateLogger(const std::string& loggerName)
{
  auto logger = std::make_shared<spdlog::logger>(loggerName, m_sinks);
  logger->set_level(spdlog::level::trace);
  spdlog::initialize_logger(logger);
  return logger;
}

Logger CLog::GetLoggerById(uint32_t component)
{
  return GetLogger(ComponentName(component));
}

void CLog::FormatLineBreaks(std::string& message) const
{
  StringUtils::Replace(message, "\n", "\n                                                   ");
}
