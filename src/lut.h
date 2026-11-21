#pragma once
#include <string>
#include <vector>

// A 3D LUT stored as RGB floats, red varying fastest (same order as .cube).
struct LutData {
    int size = 0;
    std::vector<float> rgb; // size^3 * 3
};

struct LutEntry {
    std::string id;      // "teal-orange" or "file:Name.cube"
    std::string name;    // display name
    std::string kind;    // "built-in" or "imported"
    int size = 33;
};

// Built-in looks plus imported .cube files in %APPDATA%\Ionization\luts.
std::vector<LutEntry> ListLuts();
// Generate/load LUT data by id. Returns false for "neutral" or unknown ids.
bool LoadLut(const std::string& id, LutData& out);
// Parse a .cube text file. Returns false with an error message on failure.
bool ParseCube(const std::string& text, LutData& out, std::string& error);
// Copy an external .cube into the luts folder; returns its id on success.
bool ImportCube(const std::wstring& srcPath, std::string& outId, std::string& error);
