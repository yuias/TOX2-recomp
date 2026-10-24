#pragma once

// Which Vulkan device the renderer takes when the loader lists several. Free of Vulkan headers so that option
// parsing and its tests do not depend on them; vk_context.cpp maps the driver id and the device type.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace tox2 {

// The driver the user asks for. Auto prefers MoltenVK over KosmicKrisp among devices of the same type.
enum class VkDriverChoice : std::uint8_t { Auto, MoltenVk, KosmicKrisp };

constexpr std::string_view vkDriverChoiceName(VkDriverChoice choice) {
    switch (choice) {
        case VkDriverChoice::MoltenVk: return "moltenvk";
        case VkDriverChoice::KosmicKrisp: return "kosmickrisp";
        case VkDriverChoice::Auto: break;
    }
    return "auto";
}

// The three names above; nullopt for anything else (the match is exact).
constexpr std::optional<VkDriverChoice> parseVkDriverChoice(std::string_view text) {
    if (text == "auto") return VkDriverChoice::Auto;
    if (text == "moltenvk") return VkDriverChoice::MoltenVk;
    if (text == "kosmickrisp") return VkDriverChoice::KosmicKrisp;
    return std::nullopt;
}

enum class VkDriverKind : std::uint8_t { Other, MoltenVk, KosmicKrisp };

// A usable device, as the choice sees it.
struct VkDeviceCandidate {
    std::string name;         // device name, for the error message
    std::string driver_name;  // reported driver name, for the error message
    bool discrete = false;
    VkDriverKind driver = VkDriverKind::Other;
};

// The index of the device to create, or why none is eligible. A discrete GPU beats any other type. Among devices of
// one type Auto takes MoltenVK first: KosmicKrisp costs more GPU time per draw and per pixel on the same hardware.
// Then the first device in the list wins. A named driver makes only the devices with that driver eligible.
inline std::expected<std::size_t, std::string> chooseVkDevice(std::span<const VkDeviceCandidate> devices,
                                                              VkDriverChoice choice) {
    std::optional<std::size_t> best;
    int best_rank = -1;
    for (std::size_t i = 0; i < devices.size(); i++) {
        const VkDeviceCandidate& device = devices[i];
        if (choice == VkDriverChoice::MoltenVk && device.driver != VkDriverKind::MoltenVk) continue;
        if (choice == VkDriverChoice::KosmicKrisp && device.driver != VkDriverKind::KosmicKrisp) continue;
        const int rank = (device.discrete ? 2 : 0) + (device.driver == VkDriverKind::MoltenVk ? 1 : 0);
        if (rank > best_rank) {
            best = i;
            best_rank = rank;
        }
    }
    if (best) return *best;

    if (choice == VkDriverChoice::Auto) return std::unexpected("no usable Vulkan device");
    std::string found;
    for (const VkDeviceCandidate& device : devices) {
        if (!found.empty()) found += ", ";
        found += std::format("{} ({})", device.name, device.driver_name);
    }
    if (found.empty()) found = "no usable device";
    const std::string_view wanted = choice == VkDriverChoice::MoltenVk ? "MoltenVK" : "KosmicKrisp";
    return std::unexpected(std::format("no Vulkan device with driver {} (found: {})", wanted, found));
}

}  // namespace tox2
