#pragma once

#include <cstdint>
#include <fstream>
#include <ios>
#include <string>

namespace XFILE
{

enum OpenFlags
{
  READ_TRUNCATED = 0x01,
};

enum class IOControl
{
  SEEK_POSSIBLE = 0,
};

class CFile
{
public:
  bool Open(const std::string& filename, unsigned int /* flags */)
  {
    m_stream.open(filename, std::ios::binary);
    return m_stream.is_open();
  }

  int IoControl(IOControl control, void* /* param */)
  {
    if (control == IOControl::SEEK_POSSIBLE)
      return 1;
    return 0;
  }

  int64_t GetLength()
  {
    if (!m_stream.is_open())
      return 0;
    const auto current = m_stream.tellg();
    m_stream.seekg(0, std::ios::end);
    const auto length = m_stream.tellg();
    m_stream.seekg(current, std::ios::beg);
    return static_cast<int64_t>(length);
  }

  int GetChunkSize() { return 4096; }

  int Read(uint8_t* buf, int size)
  {
    if (!m_stream.is_open())
      return 0;
    m_stream.read(reinterpret_cast<char*>(buf), size);
    return static_cast<int>(m_stream.gcount());
  }

  int64_t Seek(int64_t pos, int whence)
  {
    if (!m_stream.is_open())
      return -1;

    std::ios_base::seekdir dir = std::ios::beg;
    switch (whence)
    {
      case SEEK_SET:
        dir = std::ios::beg;
        break;
      case SEEK_CUR:
        dir = std::ios::cur;
        break;
      case SEEK_END:
        dir = std::ios::end;
        break;
      default:
        dir = std::ios::beg;
        break;
    }

    m_stream.clear();
    m_stream.seekg(pos, dir);
    return static_cast<int64_t>(m_stream.tellg());
  }

private:
  std::ifstream m_stream;
};

} // namespace XFILE
