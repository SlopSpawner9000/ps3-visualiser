// Shareable custom presets: a one-line text format and a folder of saved presets.
//
// Format: "PS3VIS-PRESET1:" followed by base64 of UTF-8 text lines
//     name=<preset name>
//     <PARAMETER NAME>=<value>      (one per firmware parameter, e.g. "TERRAIN HEIGHT=37")
// Base64 keeps it intact when pasted into chats, forums or e-mails.
#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

struct SharedPreset {
    std::string name;
    std::map<std::string, float> values;
};

namespace presetlib {

std::string encode(const SharedPreset &p);
// Accepts the encoded line (surrounding whitespace and line breaks are ignored). Values whose
// names aren't in `known` are dropped. False if it isn't a preset.
bool decode(const std::string &text, const std::vector<std::string> &known, SharedPreset &out);

// The library folder (created if needed): <pref dir>/presets.
std::filesystem::path folder();
struct Entry {
    std::string name; // the preset's own name (from inside the file)
    std::filesystem::path file;
};
// Every valid preset file in the folder (and up to two levels of subfolders): .ps3vis, .txt or no
// extension, at most 64 KB, holding a preset that decodes to at least one known parameter. Sorted
// by name. Cached; rescanned at most every 1.5 s, or after save/rename/remove.
std::vector<Entry> list(const std::vector<std::string> &known);
// Saves under a unique file name (adds " 2", " 3", ... if the name is taken). Returns the file.
std::filesystem::path save(const SharedPreset &p);
bool load(const std::filesystem::path &file, const std::vector<std::string> &known, SharedPreset &out);
// Gives a saved preset a new name (inside the file and as its file name). False on failure.
bool rename(const Entry &e, const std::string &newName, const std::vector<std::string> &known);
bool remove(const Entry &e);
// Cleans a preset name: no control characters, trimmed, at most 60 bytes (whole UTF-8 characters).
std::string cleanName(const std::string &name);

} // namespace presetlib
