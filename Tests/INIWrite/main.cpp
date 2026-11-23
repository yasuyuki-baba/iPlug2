// Exercise SWELL's public INI API without GTK, an audio device or a display.
#define NOMINMAX
#include "../../WDL/swell/swell.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unistd.h>

char* g_swell_defini = nullptr;
namespace fs = std::filesystem;

static void Check(bool condition, const char* message)
{
  if (!condition) throw std::runtime_error(message);
}

static std::string Read(const fs::path& path)
{
  std::ifstream input(path);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

static void Test(const fs::path& root)
{
  const auto file = root / "settings.ini";
  const auto temporary = root / ".settings.ini.new";
  Check(WritePrivateProfileString("audio", "driver", "0", file.c_str()), "Initial write failed");
  const auto original = Read(file);

  fs::create_directory(temporary);
  errno = 0;
  Check(!WritePrivateProfileString("audio", "driver", "1", file.c_str()), "Open failure was reported as success");
  Check(errno != 0, "Open failure lost errno");
  Check(Read(file) == original, "Open failure changed existing settings");
  Check(GetPrivateProfileInt("audio", "driver", -1, file.c_str()) == 0, "Failed write poisoned the INI cache");
  fs::remove(temporary);

  fs::create_symlink("/dev/full", temporary);
  errno = 0;
  Check(!WritePrivateProfileString("audio", "driver", "2", file.c_str()), "Buffered failure was reported as success");
  Check(errno == ENOSPC, "Buffered failure lost ENOSPC");
  Check(Read(file) == original, "Buffered failure replaced existing settings");
  Check(!fs::is_symlink(file), "Failed temporary symlink was renamed over settings");
  Check(!fs::exists(temporary), "Failed temporary file was not cleaned up");
  Check(GetPrivateProfileInt("audio", "driver", -1, file.c_str()) == 0, "Buffered failure poisoned the cache");

  Check(WritePrivateProfileString("audio", "driver", "1", file.c_str()), "Retry after failure did not write");
  Check(GetPrivateProfileInt("audio", "driver", -1, file.c_str()) == 1, "Retry did not persist the requested value");
  Check(Read(file) != original, "Retry incorrectly treated a cached value as persisted");

  const auto directory = root / "directory.ini";
  fs::create_directory(directory);
  errno = 0;
  Check(!WritePrivateProfileString("audio", "driver", "0", directory.c_str()), "Rename failure was reported as success");
  Check(errno != 0 && fs::is_directory(directory), "Rename failure lost errno or changed the target");
  Check(!fs::exists(root / ".directory.ini.new"), "Rename failure left a temporary file");

  const auto section = root / "section.ini";
  const auto sectionTemporary = root / ".section.ini.new";
  Check(WritePrivateProfileSection("audio", "driver=0\0buffer=512\0", section.c_str()), "Section write failed");
  const auto sectionOriginal = Read(section);
  fs::create_symlink("/dev/full", sectionTemporary);
  Check(!WritePrivateProfileSection("audio", "driver=1\0", section.c_str()), "Section failure was reported as success");
  Check(Read(section) == sectionOriginal, "Section failure replaced the existing file");
  Check(GetPrivateProfileInt("audio", "driver", -1, section.c_str()) == 0, "Section failure poisoned the cache");

  if (geteuid() != 0)
  {
    const auto locked = root / "locked";
    fs::create_directory(locked);
    fs::permissions(locked, fs::perms::owner_read | fs::perms::owner_exec);
    errno = 0;
    const bool wrote = WritePrivateProfileString("audio", "driver", "0", (locked / "settings.ini").c_str());
    const int error = errno;
    fs::permissions(locked, fs::perms::owner_all);
    Check(!wrote && error == EACCES, "Permission failure was reported as success or lost EACCES");
  }
}

int main()
{
  char pattern[] = "/tmp/iplug-ini-write-XXXXXX";
  if (!mkdtemp(pattern)) return 1;
  const fs::path root(pattern);
  int result = 0;
  try
  {
    Test(root);
    std::puts("INI write failure, preservation and retry tests passed");
  }
  catch (const std::exception& error)
  {
    std::fprintf(stderr, "%s\n", error.what());
    result = 1;
  }
  fs::remove_all(root);
  return result;
}
