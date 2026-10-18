#include "hle/save_dialog.h"

#include <algorithm>
#include <atomic>
#include <utility>

#include "core/log.h"

namespace tox2 {

namespace {

// Answers without showing or waiting. Auto picks what the player would most likely accept; Cancel backs out.
class PolicyDriver final : public SaveDialogDriver {
public:
    explicit PolicyDriver(SaveDialogPolicy policy) : policy_(policy) {}

    bool interactive() const override { return false; }

    std::optional<std::int32_t> showList(const SaveListRequest& request) override {
        const auto rows = request.content ? static_cast<std::int32_t>(request.content->rows.size()) : 0;
        std::optional<std::int32_t> row;
        if (policy_ == SaveDialogPolicy::Auto && rows > 0) {
            row = std::clamp(request.initial_row, 0, rows - 1);
        }
        if (row) {
            logInfo("tox2: savedata: dialog ({}): list of {} rows -> row {}", name(), rows, *row);
        } else {
            logInfo("tox2: savedata: dialog ({}): list of {} rows -> back", name(), rows);
        }
        return row;
    }

    SaveMessageResult showMessage(std::shared_ptr<SaveDialogContent> content) override {
        if (content && content->kind == DialogKind::YesNo) {
            const bool yes = policy_ == SaveDialogPolicy::Auto;
            logInfo("tox2: savedata: dialog ({}): confirmation -> {}", name(), yes ? "yes" : "no");
            return yes ? SaveMessageResult::Yes : SaveMessageResult::No;
        }
        logInfo("tox2: savedata: dialog ({}): message -> ok", name());
        return SaveMessageResult::Ok;
    }

private:
    const char* name() const { return policy_ == SaveDialogPolicy::Auto ? "auto" : "cancel"; }

    SaveDialogPolicy policy_;
};

// Placeholder until the waiting driver exists: answers like the Cancel policy.
class InteractiveDriver final : public SaveDialogDriver {
public:
    explicit InteractiveDriver(SaveDialogBoard& board) : cancel_(SaveDialogPolicy::Cancel), board_(board) {}

    bool interactive() const override { return true; }
    std::optional<std::int32_t> showList(const SaveListRequest& request) override { return cancel_.showList(request); }
    SaveMessageResult showMessage(std::shared_ptr<SaveDialogContent> content) override {
        return cancel_.showMessage(std::move(content));
    }

private:
    PolicyDriver cancel_;
    [[maybe_unused]] SaveDialogBoard& board_;
};

std::unique_ptr<SaveDialogDriver>& currentDriver() {
    static std::unique_ptr<SaveDialogDriver> driver = makeAutoSaveDialogDriver(SaveDialogPolicy::Auto);
    return driver;
}

std::atomic<bool> g_dialog_open{false};

}  // namespace

void SaveDialogBoard::publish(SaveDialogSnapshot snapshot) {
    std::lock_guard lock(mutex_);
    snapshot.serial = ++serial_;
    snapshot_ = std::move(snapshot);
}

void SaveDialogBoard::clear() {
    std::lock_guard lock(mutex_);
    snapshot_ = SaveDialogSnapshot{};
    snapshot_.serial = ++serial_;
}

SaveDialogSnapshot SaveDialogBoard::read() const {
    std::lock_guard lock(mutex_);
    return snapshot_;
}

void SaveDialogBoard::setDrawer(SaveDialogDrawer drawer) {
    std::lock_guard lock(mutex_);
    drawer_ = drawer;
}

SaveDialogDrawer SaveDialogBoard::drawer() const {
    std::lock_guard lock(mutex_);
    return drawer_;
}

SaveDialogBoard& saveDialogBoard() {
    static SaveDialogBoard board;
    return board;
}

std::unique_ptr<SaveDialogDriver> makeAutoSaveDialogDriver(SaveDialogPolicy policy) {
    return std::make_unique<PolicyDriver>(policy);
}

std::unique_ptr<SaveDialogDriver> makeInteractiveSaveDialogDriver(SaveDialogBoard& board) {
    return std::make_unique<InteractiveDriver>(board);
}

void setSaveDialogDriver(std::unique_ptr<SaveDialogDriver> driver) {
    currentDriver() = driver ? std::move(driver) : makeAutoSaveDialogDriver(SaveDialogPolicy::Auto);
}

SaveDialogDriver& saveDialogDriver() { return *currentDriver(); }

bool saveDialogOpen() { return g_dialog_open.load(); }

void resetSaveDialog() {
    setSaveDialogDriver(nullptr);
    g_dialog_open.store(false);
    saveDialogBoard().clear();
    saveDialogBoard().setDrawer(SaveDialogDrawer::None);
}

}  // namespace tox2
