#pragma once

// Dear ImGui keeps its current context in one process global (GImGui), so overlays with their own contexts take
// turns: every use of any overlay's context happens under imguiMutex(), with that context made current.
//
// Lock order: imguiMutex() sits between the guest kernel lock and the leaf locks (for example the save dialog
// board). A thread may take a leaf lock while holding it, but must never take the kernel lock while holding it,
// and no thread may take it while holding a leaf lock. Overlay code therefore reads the runtime state it shows
// (which may take the kernel lock) before opening a scope, and copies it into its own state.

#include <mutex>

#include "imgui.h"

namespace tox2 {

inline std::recursive_mutex& imguiMutex() {
    static std::recursive_mutex mutex;
    return mutex;
}

// Locks imguiMutex() and makes `context` current (null is allowed); restores the previous context on destruction.
// Recursive, because the Win32 handler re-enters the window procedure on the same thread: ReleaseCapture sends
// WM_CAPTURECHANGED before it returns.
class ImGuiContextScope {
public:
    explicit ImGuiContextScope(ImGuiContext* context) : lock_(imguiMutex()), previous_(ImGui::GetCurrentContext()) {
        ImGui::SetCurrentContext(context);
    }
    ~ImGuiContextScope() { ImGui::SetCurrentContext(previous_); }
    ImGuiContextScope(const ImGuiContextScope&) = delete;
    ImGuiContextScope& operator=(const ImGuiContextScope&) = delete;

private:
    std::lock_guard<std::recursive_mutex> lock_;
    ImGuiContext* previous_;
};

}  // namespace tox2
