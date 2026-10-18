#include "hle/save_dialog_text.h"

#include <cstdio>

namespace tox2 {

namespace {

// CellSaveDataCBResult::result values that make RPCS3 show an error dialog.
constexpr std::int32_t kResultErrNoSpace = -1;
constexpr std::int32_t kResultErrFailure = -2;
constexpr std::int32_t kResultErrBroken = -3;
constexpr std::int32_t kResultErrNoData = -4;
constexpr std::int32_t kResultErrInvalid = -5;

}  // namespace

std::string formatSaveSize(std::uint64_t bytes) {
    const std::uint64_t kb = (bytes + 999) / 1000;
    if (kb > 1000) return std::to_string((kb + 999) / 1000) + " MB";
    return std::to_string(kb) + " KB";
}

std::string formatSaveDate(const std::tm& local) {
    // snprintf, not std::put_time: the output must not depend on the C locale.
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04d/%02d/%02d  %02d:%02d", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                  local.tm_hour, local.tm_min);
    return buf;
}

std::string saveRowInfo(const std::tm& local_mtime, std::uint64_t bytes) {
    return formatSaveDate(local_mtime) + "   " + formatSaveSize(bytes);
}

std::string saveConfirmText(std::string_view question, std::string_view title, std::string_view subtitle,
                            std::string_view date, std::string_view size, std::string_view detail) {
    std::string text;
    text.reserve(question.size() + title.size() + subtitle.size() + date.size() + size.size() + detail.size() + 8);
    text += question;
    text += "\n\n";
    text += title;
    text += '\n';
    text += subtitle;
    text += '\n';
    text += date;
    text += '\n';
    text += size;
    text += "\n\n";
    text += detail;
    return text;
}

std::optional<std::string> saveErrorText(std::int32_t cb_result, std::int32_t need_kb, std::string_view invalid_msg) {
    switch (cb_result) {
        case kResultErrNoSpace:
            return std::string(kErrorNoSpace) + "\n\n" + std::string(kErrorNeedSpaceLabel) + std::to_string(need_kb) +
                   " KB";
        case kResultErrFailure: return std::string(kErrorFailure);
        case kResultErrBroken: return std::string(kErrorBroken);
        case kResultErrNoData: return std::string(kErrorNoData);
        case kResultErrInvalid: return std::string(invalid_msg);
        default: return std::nullopt;
    }
}

}  // namespace tox2
