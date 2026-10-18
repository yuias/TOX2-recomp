#include "overlay/ui_font.h"

#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

#include "core/log.h"
#include "hle/save_dialog_text.h"
#include "imgui_internal.h"

// A font that does not parse would hit an IM_ASSERT inside ImGui, so it is checked here first. A private static
// copy keeps this independent of the one inside imgui_draw.cpp.
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "imstb_truetype.h"

namespace tox2 {

namespace {

constexpr const char* kFirmwareFontName = "SCE-PS3-SR-R-JPN.TTF";
constexpr const char* kFallbackFontName = "BIZUDPGothic-Regular.ttf";
constexpr int kProbeCodePoint = 0x30BB;  // セ: a font without it cannot show the dialog text
constexpr std::uintmax_t kMaxFontBytes = 256u << 20;

std::optional<std::vector<std::uint8_t>> readFile(const std::filesystem::path& path) {
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec || size == 0 || size > kMaxFontBytes) return std::nullopt;
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (static_cast<std::uintmax_t>(in.gcount()) != size) return std::nullopt;
    return bytes;
}

enum class FontCheck { Ok, NotAFont, NoKana };

FontCheck checkFont(const std::vector<std::uint8_t>& bytes) {
    const unsigned char* data = bytes.data();
    const int offset = stbtt_GetFontOffsetForIndex(data, 0);
    if (offset < 0) return FontCheck::NotAFont;
    stbtt_fontinfo info;
    if (!stbtt_InitFont(&info, data, offset)) return FontCheck::NotAFont;
    return stbtt_FindGlyphIndex(&info, kProbeCodePoint) != 0 ? FontCheck::Ok : FontCheck::NoKana;
}

void addText(std::set<ImWchar>& out, std::string_view text) {
    const char* p = text.data();
    const char* end = p + text.size();
    while (p < end) {
        unsigned int c = 0;
        const int used = ImTextCharFromUtf8(&c, p, end);
        if (used <= 0) break;
        p += used;
        // Control characters (newline) draw no glyph.
        if (c >= 0x20) out.insert(static_cast<ImWchar>(c));
    }
}

}  // namespace

std::vector<std::filesystem::path> uiFontCandidates(const std::filesystem::path& explicit_path,
                                                    const std::filesystem::path& libsre_dir,
                                                    const std::filesystem::path& elf_dir,
                                                    const std::filesystem::path& exe_dir) {
    std::vector<std::filesystem::path> out;
    if (!explicit_path.empty()) out.push_back(explicit_path);
    if (!libsre_dir.empty()) out.push_back(libsre_dir / kFirmwareFontName);
    if (!elf_dir.empty()) out.push_back(elf_dir / kFirmwareFontName);
    if (!exe_dir.empty()) out.push_back(exe_dir / "fonts" / kFallbackFontName);
    return out;
}

std::optional<UiFontFile> loadUiFont(std::span<const std::filesystem::path> candidates) {
    for (const std::filesystem::path& path : candidates) {
        std::optional<std::vector<std::uint8_t>> bytes = readFile(path);
        if (!bytes) continue;
        switch (checkFont(*bytes)) {
            case FontCheck::Ok:
                logInfo("tox2: overlay: UI font {}", path.string());
                return UiFontFile{path, std::move(*bytes)};
            case FontCheck::NotAFont: logWarn("tox2: overlay: UI font {} skipped (not a font)", path.string()); break;
            case FontCheck::NoKana: logWarn("tox2: overlay: UI font {} skipped (no Japanese glyphs)", path.string());
        }
    }
    logWarn("tox2: overlay: no UI font with Japanese glyphs found, the save dialog uses ImGui's default font");
    return std::nullopt;
}

std::set<ImWchar> dialogCodePoints(const SaveDialogContent& content) {
    std::set<ImWchar> out;
    for (ImWchar c = 0x20; c <= 0x7E; ++c) out.insert(c);
    for (const std::string_view text :
         {kHeadingSave,      kHeadingLoad, kHeadingDelete, kLegendEnter,    kLegendBack,
          kNewDataTitle,     kNewDataLine, kEmptyList,     kConfirmSaveNew, kConfirmLoadNew,
          kConfirmOverwrite, kConfirmLoad, kConfirmDelete, kErrorNoSpace,   kErrorNeedSpaceLabel,
          kErrorFailure,     kErrorBroken, kErrorNoData,   kButtonYes,      kButtonNo,
          kButtonOk}) {
        addText(out, text);
    }
    for (const SaveDialogRow& row : content.rows) {
        addText(out, row.title);
        addText(out, row.info);
        addText(out, row.detail);
    }
    addText(out, content.message);
    return out;
}

ImVector<ImWchar> glyphRanges(const std::set<ImWchar>& code_points) {
    ImFontGlyphRangesBuilder builder;
    for (const ImWchar c : code_points) builder.AddChar(c);
    ImVector<ImWchar> ranges;
    builder.BuildRanges(&ranges);
    return ranges;
}

}  // namespace tox2
