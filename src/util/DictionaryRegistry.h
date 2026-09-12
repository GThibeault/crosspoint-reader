#pragma once

#include <string>
#include <vector>

// One StarDict dictionary group found under /dictionaries or /.dictionaries:
// a subfolder holding one or more complete StarDict file sets.
struct DictionaryEntry {
  std::string name;  // subfolder name (shown to the user, stored in settings)
};

namespace DictionaryRegistry {

// Scan /dictionaries/*/ and /.dictionaries/*/ for dictionary groups. Result is
// sorted case-insensitively by folder name.
void discover(std::vector<DictionaryEntry>& out);

// Resolve every complete StarDict file set in a folder to its extensionless
// base path. Results are sorted case-insensitively by stem, which is also the
// lookup fall-through order. Returns false when neither root contains a usable
// dictionary group with this name.
bool resolveBasePaths(const char* folderName, std::vector<std::string>& basePathsOut);

}  // namespace DictionaryRegistry
