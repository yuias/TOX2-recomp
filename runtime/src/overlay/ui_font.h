// Font choice and glyph sets of the save data dialog. ImGui 1.91 bakes a fixed glyph set into the atlas, so the
// dialog collects the code points it needs up front.

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <set>
#include <span>
#include <vector>

#include "hle/save_dialog.h"
#include "imgui.h"

namespace tox2 {

struct UiFontFile {
    std::filesystem::path path;
    std::vector<std::uint8_t> bytes;
};

// Candidates, in order: `explicit_path` (--ui-font); <libsre dir>/SCE-PS3-SR-R-JPN.TTF;
// <elf dir>/SCE-PS3-SR-R-JPN.TTF; <exe dir>/fonts/BIZUDPGothic-Regular.ttf. An empty path or directory is skipped.
std::vector<std::filesystem::path> uiFontCandidates(const std::filesystem::path& explicit_path,
                                                    const std::filesystem::path& libsre_dir,
                                                    const std::filesystem::path& elf_dir,
                                                    const std::filesystem::path& exe_dir);

// The first readable candidate that stb_truetype parses and that has a glyph for U+30BB; logs which one.
// nullopt (with a warning) when none: the dialog then uses ImGui's default font and Japanese shows as '?'.
std::optional<UiFontFile> loadUiFont(std::span<const std::filesystem::path> candidates);

// Code points the atlas must hold for a dialog: printable ASCII, every constant of save_dialog_text.h and every
// string of `content` (UTF-8). The digits and punctuation of the formatters are printable ASCII.
std::set<ImWchar> dialogCodePoints(const SaveDialogContent& content);

// Sorted (first, last) pairs ended by 0, as ImFontConfig::GlyphRanges expects.
ImVector<ImWchar> glyphRanges(const std::set<ImWchar>& code_points);

}  // namespace tox2
