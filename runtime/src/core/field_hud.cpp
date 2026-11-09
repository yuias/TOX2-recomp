#include "tox2/field_hud.h"

#include <atomic>

namespace {
std::atomic<bool> g_field_hud_hidden{false};
}  // namespace

bool tox2FieldHudHidden(void) { return g_field_hud_hidden.load(std::memory_order_relaxed); }

void tox2SetFieldHudHidden(bool hidden) { g_field_hud_hidden.store(hidden, std::memory_order_relaxed); }
