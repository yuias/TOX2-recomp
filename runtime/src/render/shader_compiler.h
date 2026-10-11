#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace tox2 {

enum class ShaderStage { Vertex, Fragment };

// Part of every cache file name. Bump it on any change to the GLSL the translators emit, so stale
// SPIR-V is never reused.
inline constexpr std::uint32_t kTranslatorVersion = 3;

// Directory of the on-disk SPIR-V cache; an empty path disables caching. Set it before the first
// compileGlsl call.
void setShaderCacheDir(const std::filesystem::path& dir);

struct ShaderCompilerStats {
    std::uint64_t cache_hits = 0;
    std::uint64_t compiles = 0;  // shaderc invocations, successful or not
};
ShaderCompilerStats shaderCompilerStats();

// Compiles GLSL 4.50 to SPIR-V 1.6 for Vulkan 1.3 without optimisation. The result is cached as
// <cache_dir>/<stage>_<cache_key>_<hash of glsl>_<kTranslatorVersion>.spv. The GLSL hash means
// different texts never share a file, even under the same cache_key (which stays in the name for
// readability, e.g. the ucode hash). A missing, short or corrupt file is recompiled and rewritten.
// On failure the error is the shaderc message. Thread-safe.
std::expected<std::vector<std::uint32_t>, std::string> compileGlsl(ShaderStage stage, std::string_view glsl,
                                                                   std::uint64_t cache_key);

}  // namespace tox2
