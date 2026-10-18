// Japanese UI strings of the save data dialogs and the formatting of the texts built from save data. Source files
// are UTF-8, and the strings are plain data, so they live in this one header.

#pragma once

#include <cstdint>
#include <ctime>
#include <optional>
#include <string>
#include <string_view>

namespace tox2 {

inline constexpr std::string_view kHeadingSave = "セーブ";
inline constexpr std::string_view kHeadingLoad = "ロード";
inline constexpr std::string_view kHeadingDelete = "削除";  // reserved for Delete2
inline constexpr std::string_view kLegendEnter = "決定";
inline constexpr std::string_view kLegendBack = "戻る";
inline constexpr std::string_view kNewDataTitle = "新しいデータ";
inline constexpr std::string_view kNewDataLine = "新しいデータを作成します";
inline constexpr std::string_view kEmptyList = "セーブデータがありません";
inline constexpr std::string_view kConfirmSaveNew = "新しいデータにセーブしますか？";
// Deliberately not RPCS3's "save this data?" text, which it also shows for a load list's new-data row.
inline constexpr std::string_view kConfirmLoadNew = "新しいデータを作成しますか？";
inline constexpr std::string_view kConfirmOverwrite = "このデータに上書きしますか？";
inline constexpr std::string_view kConfirmLoad = "このデータをロードしますか？";
inline constexpr std::string_view kConfirmDelete = "このデータを削除しますか？";  // reserved for Delete2
inline constexpr std::string_view kErrorNoSpace = "空き容量が足りません。";
inline constexpr std::string_view kErrorNeedSpaceLabel = "必要な空き容量：";  // followed by "N KB"
inline constexpr std::string_view kErrorFailure = "セーブまたはロードに失敗しました。";
inline constexpr std::string_view kErrorBroken = "セーブデータが壊れています。";
inline constexpr std::string_view kErrorNoData = "セーブデータが見つかりません。";
inline constexpr std::string_view kButtonYes = "はい";
inline constexpr std::string_view kButtonNo = "いいえ";
inline constexpr std::string_view kButtonOk = "OK";

// Decimal units as RPCS3: ceil(bytes / 1000) "N KB"; above 1000 KB, ceil(KB / 1000) "N MB".
std::string formatSaveSize(std::uint64_t bytes);
// Locale-independent "YYYY/MM/DD  HH:MM" (zero-padded, two spaces) of a broken-down local time.
std::string formatSaveDate(const std::tm& local);
// Row line 2: date, three spaces, size.
std::string saveRowInfo(const std::tm& local_mtime, std::uint64_t bytes);
// Confirmation text: `question`, blank line, then title, subtitle, date, size each on a line, blank line, detail.
// An empty field keeps its line.
std::string saveConfirmText(std::string_view question, std::string_view title, std::string_view subtitle,
                            std::string_view date, std::string_view size, std::string_view detail);
// The OK-dialog text for a CBRESULT error; nullopt for results without a dialog (PARAM error).
// ERR_INVALID returns `invalid_msg` as given (empty when the game's pointer is null).
std::optional<std::string> saveErrorText(std::int32_t cb_result, std::int32_t need_kb, std::string_view invalid_msg);

}  // namespace tox2
