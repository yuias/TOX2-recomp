// Save data dialogs as seen by cellSaveData: what is shown (content), what the overlay draws (snapshot on the board)
// and who answers (the driver). The Auto and Cancel drivers answer at once and never wait, so runs without a window,
// replays and tests never block.

#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "hle/save_dialog_model.h"

namespace tox2 {

enum class SaveListKind : std::uint8_t { Save, Load, Delete };

struct SaveDialogRow {
    std::string title;   // line 1: SUB_TITLE, or TITLE when SUB_TITLE is empty; new data: icon title or kNewDataTitle
    std::string info;    // line 2: saveRowInfo(); new data: kNewDataLine
    std::string detail;  // line 3: DETAIL (may hold newlines); empty for new data
    std::shared_ptr<const std::vector<std::uint8_t>> icon_png;  // ICON0.PNG or newData iconBuf; null: placeholder
    bool new_data = false;
};

// Immutable once published.
struct SaveDialogContent {
    std::uint64_t id = 0;  // unique per opened dialog, assigned by the driver
    DialogKind kind = DialogKind::List;
    SaveListKind list_kind = SaveListKind::Save;  // heading
    std::vector<SaveDialogRow> rows;              // List only
    std::string message;                          // YesNo / Ok only
};

struct SaveDialogSnapshot {
    std::shared_ptr<const SaveDialogContent> content;  // null: no dialog
    std::int32_t selected_row = 0;
    std::int32_t first_visible_row = 0;
    float opacity = 0.0f;
    std::uint64_t serial = 0;  // grows with every publish
};

enum class SaveDialogDrawer : std::uint8_t { None, Ready, Failed };

// Shared between the waiting guest thread (writer) and the RSX thread (reader); own mutex, no kernel lock.
class SaveDialogBoard {
public:
    // Stores `snapshot` with a fresh `serial`, so a reader can tell any two publishes apart.
    void publish(SaveDialogSnapshot snapshot);
    // Empties the board (null content); counts as a publish for `serial`.
    void clear();
    SaveDialogSnapshot read() const;
    void setDrawer(SaveDialogDrawer drawer);  // the overlay: Ready when constructed, Failed when its init fails
    SaveDialogDrawer drawer() const;

private:
    mutable std::mutex mutex_;
    SaveDialogSnapshot snapshot_;
    std::uint64_t serial_ = 0;
    SaveDialogDrawer drawer_ = SaveDialogDrawer::None;
};
SaveDialogBoard& saveDialogBoard();

struct SaveListRequest {
    std::shared_ptr<SaveDialogContent> content;  // kind List; id is set by the driver
    std::int32_t initial_row = 0;
};
enum class SaveMessageResult : std::uint8_t { Yes, No, Ok, Failed };  // Failed: DRAWING_BEGIN refused

// Called by cellSaveData on the calling guest thread with the kernel lock held.
class SaveDialogDriver {
public:
    virtual ~SaveDialogDriver() = default;
    virtual bool interactive() const = 0;
    // The chosen row, or nullopt for back/cancel.
    virtual std::optional<std::int32_t> showList(const SaveListRequest& request) = 0;
    // kind YesNo or Ok in `content`.
    virtual SaveMessageResult showMessage(std::shared_ptr<SaveDialogContent> content) = 0;
};

enum class SaveDialogPolicy : std::uint8_t { Auto, Cancel };
std::unique_ptr<SaveDialogDriver> makeAutoSaveDialogDriver(SaveDialogPolicy policy);
std::unique_ptr<SaveDialogDriver> makeInteractiveSaveDialogDriver(SaveDialogBoard& board);
// null: Auto. Before the guest runs or under the kernel lock.
void setSaveDialogDriver(std::unique_ptr<SaveDialogDriver> driver);
SaveDialogDriver& saveDialogDriver();
bool saveDialogOpen();   // an interactive dialog holds DRAWING_BEGIN now (kernel lock)
void resetSaveDialog();  // tests: Auto driver, board cleared, drawer None

}  // namespace tox2
