#include "DictionaryRegistry.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>
#include <utility>

#include "StringUtils.h"

namespace DictionaryRegistry {
namespace {

// Dictionaries are looked up in both roots, in order. The hidden variant
// lets users keep the folder out of the file browser (hidden by default,
// see FileBrowserActivity's showHiddenFiles check).
constexpr const char* DICT_ROOTS[] = {"/dictionaries", "/.dictionaries"};

// Collect every .idx that has matching definition data. The caller reuses the
// vector across folders so discovery does not repeatedly allocate temporary
// containers.
bool findBasePaths(const char* folderPath, std::vector<std::string>& basePathsOut) {
  basePathsOut.clear();
  auto dir = Storage.open(folderPath);
  if (!dir || !dir.isDirectory()) return false;

  dir.rewindDirectory();
  char name[128];
  for (auto entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
    entry.getName(name, sizeof(name));
    // Skip macOS metadata files (AppleDouble resource forks)
    if (entry.isDirectory() || strncmp(name, "._", 2) == 0) continue;

    const size_t len = strlen(name);
    if (len <= 4 || strcmp(name + len - 4, ".idx") != 0) continue;

    name[len - 4] = '\0';
    const std::string base = std::string(folderPath) + "/" + name;
    if (!Storage.exists((base + ".dict").c_str()) && !Storage.exists((base + ".dict.dz").c_str())) {
      LOG_DBG("DREG", "Skipping %s: no .dict or .dict.dz", base.c_str());
      continue;
    }
    basePathsOut.push_back(base);
  }

  std::sort(basePathsOut.begin(), basePathsOut.end(), [](const std::string& a, const std::string& b) {
    return StringUtils::asciiCaseCmp(a.c_str(), b.c_str()) < 0;
  });
  return !basePathsOut.empty();
}

}  // namespace

void discover(std::vector<DictionaryEntry>& out) {
  out.clear();
  out.reserve(8);
  std::vector<std::string> basePaths;
  basePaths.reserve(4);

  for (const char* dictRoot : DICT_ROOTS) {
    auto rootDir = Storage.open(dictRoot);
    if (!rootDir || !rootDir.isDirectory()) {
      LOG_DBG("DREG", "No %s directory on SD card", dictRoot);
      continue;
    }

    rootDir.rewindDirectory();
    char name[128];
    for (auto entry = rootDir.openNextFile(); entry; entry = rootDir.openNextFile()) {
      entry.getName(name, sizeof(name));
      if (!entry.isDirectory() || name[0] == '.') continue;

      std::string folderPath = std::string(dictRoot) + "/" + name;
      if (!findBasePaths(folderPath.c_str(), basePaths)) continue;

      DictionaryEntry e;
      e.name = name;
      out.push_back(std::move(e));
      LOG_DBG("DREG", "Found dictionary group: %s (%u sources)", name, static_cast<unsigned>(basePaths.size()));
    }
  }

  // Case-insensitive sort by folder name (matches FileBrowserActivity ordering).
  std::sort(out.begin(), out.end(), [](const DictionaryEntry& a, const DictionaryEntry& b) {
    return StringUtils::asciiCaseCmp(a.name.c_str(), b.name.c_str()) < 0;
  });
}

bool resolveBasePaths(const char* folderName, std::vector<std::string>& basePathsOut) {
  basePathsOut.clear();
  if (basePathsOut.capacity() < 4) basePathsOut.reserve(4);
  if (!folderName || folderName[0] == '\0') return false;
  // folderName is persisted in the settings JSON: reject separators and dot
  // prefixes so a crafted value cannot escape the dictionary roots.
  if (folderName[0] == '.' || strpbrk(folderName, "/\\") != nullptr) return false;

  for (const char* dictRoot : DICT_ROOTS) {
    std::string folderPath = std::string(dictRoot) + "/" + folderName;
    if (findBasePaths(folderPath.c_str(), basePathsOut)) return true;
  }
  return false;
}

}  // namespace DictionaryRegistry
