/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "utils/StringUtils.h"

#include <stdarg.h>

#ifdef __GNUC__
#define XBMCCOMMONS_ATTRIB_EXCEPTION_FORMAT __attribute__((format(printf,2,3)))
#else
#define XBMCCOMMONS_ATTRIB_EXCEPTION_FORMAT
#endif

#define XBMCCOMMONS_COPYVARARGS(fmt) va_list argList; va_start(argList, fmt); Set(fmt, argList); va_end(argList)
#define XBMCCOMMONS_STANDARD_EXCEPTION(E) \
  class E : public XbmcCommons::Exception \
  { \
  public: \
    inline E(const char* message,...) XBMCCOMMONS_ATTRIB_EXCEPTION_FORMAT : Exception(#E) { XBMCCOMMONS_COPYVARARGS(message); } \
    inline E(const E& other) : Exception(other) {} \
  }

namespace XbmcCommons
{
class Exception
{
private:
  std::string classname;
  std::string message;

protected:
  inline explicit Exception(const char* classname_) : classname(classname_) {}
  inline Exception(const char* classname_, const char* message_)
    : classname(classname_), message(message_)
  {
  }
  inline Exception(const Exception& other) = default;

  inline void Set(const char* fmt, va_list& argList)
  {
    message = StringUtils::FormatV(fmt, argList);
  }

  inline void SetMessage(const char* fmt, ...) XBMCCOMMONS_ATTRIB_EXCEPTION_FORMAT
  {
    XBMCCOMMONS_COPYVARARGS(fmt);
  }

  inline void setClassname(const char* cn) { classname = cn; }

public:
  virtual ~Exception();
  virtual void LogThrowMessage(const char* prefix = nullptr) const;
  inline virtual const char* GetExMessage() const { return message.c_str(); }
};

XBMCCOMMONS_STANDARD_EXCEPTION(UncheckedException);

#define XBMCCOMMONS_HANDLE_UNCHECKED \
  catch (const XbmcCommons::UncheckedException& ) { throw; } \
  catch (const XbmcCommons::UncheckedException* ) { throw; }

} // namespace XbmcCommons
