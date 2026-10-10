#include "render/shader_compiler.h"

#include <atomic>
#include <cstdio>
#include <format>
#include <fstream>
#include <mutex>
#include <system_error>
#include <thread>

#include <shaderc/shaderc.hpp>

#include "core/log.h"
#include "rsx/rsx_census.h"

namespace tox2 {
namespace {

constexpr std::uint32_t kSpirvMagic = 0x07230203;
constexpr std::size_t kSpirvHeaderWords = 5;

std::mutex g_mutex;  // guards g_cache_dir
std::filesystem::path g_cache_dir;
std::atomic<std::uint64_t> g_cache_hits{0};
std::atomic<std::uint64_t> g_compiles{0};

// The GLSL hash keeps different texts apart even when the caller reuses a key (one ucode can
// yield several GLSL variants) or forgets to bump kTranslatorVersion.
std::filesystem::path cacheFilePath(const std::filesystem::path& dir, ShaderStage stage, std::uint64_t cache_key,
                                    std::string_view glsl) {
    const std::uint64_t glsl_hash = rsxHash64(reinterpret_cast<const std::uint8_t*>(glsl.data()), glsl.size());
    return dir / std::format("{}_{:016x}_{:016x}_{}.spv", stage == ShaderStage::Vertex ? "vert" : "frag", cache_key,
                             glsl_hash, kTranslatorVersion);
}

bool readCache(const std::filesystem::path& path, std::vector<std::uint32_t>& out) {
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec || size % 4 != 0 || size < kSpirvHeaderWords * 4) return false;
    std::vector<std::uint32_t> words(static_cast<std::size_t>(size / 4));
    std::ifstream file(path, std::ios::binary);
    if (!file.read(reinterpret_cast<char*>(words.data()), static_cast<std::streamsize>(size))) {
        return false;
    }
    if (words[0] != kSpirvMagic) return false;
    out = std::move(words);
    return true;
}

// Temp file in the same directory + rename, so a reader never sees a partial file and concurrent
// writers of the same key cannot corrupt each other.
void writeCache(const std::filesystem::path& path, const std::vector<std::uint32_t>& words) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        logOnce(LogLevel::Warn, "shader-cache-dir", "tox2: shader cache: cannot create {}: {}",
                path.parent_path().string(), ec.message());
        return;
    }
    std::filesystem::path temp = path;
    temp += std::format(".{:x}.tmp", std::hash<std::thread::id>{}(std::this_thread::get_id()));
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(words.data()), static_cast<std::streamsize>(words.size() * 4));
        if (!file) {
            file.close();
            std::filesystem::remove(temp, ec);
            return;
        }
    }
    std::filesystem::rename(temp, path, ec);
    if (ec) std::filesystem::remove(temp, ec);
}

}  // namespace

void setShaderCacheDir(const std::filesystem::path& dir) {
    std::lock_guard lock(g_mutex);
    g_cache_dir = dir;
}

ShaderCompilerStats shaderCompilerStats() {
    return {g_cache_hits.load(std::memory_order_relaxed), g_compiles.load(std::memory_order_relaxed)};
}

std::expected<std::vector<std::uint32_t>, std::string> compileGlsl(ShaderStage stage, std::string_view glsl,
                                                                   std::uint64_t cache_key) {
    std::filesystem::path dir;
    {
        std::lock_guard lock(g_mutex);
        dir = g_cache_dir;
    }
    std::filesystem::path path;
    if (!dir.empty()) {
        path = cacheFilePath(dir, stage, cache_key, glsl);
        std::vector<std::uint32_t> cached;
        if (readCache(path, cached)) {
            g_cache_hits.fetch_add(1, std::memory_order_relaxed);
            return cached;
        }
    }

    shaderc::Compiler compiler;
    shaderc::CompileOptions options;
    options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_3);
    options.SetTargetSpirv(shaderc_spirv_version_1_6);
    options.SetOptimizationLevel(shaderc_optimization_level_zero);
    g_compiles.fetch_add(1, std::memory_order_relaxed);
    const shaderc::SpvCompilationResult result = compiler.CompileGlslToSpv(
            glsl.data(), glsl.size(),
            stage == ShaderStage::Vertex ? shaderc_glsl_vertex_shader : shaderc_glsl_fragment_shader, "shader.glsl",
            options);
    if (result.GetCompilationStatus() != shaderc_compilation_status_success) {
        return std::unexpected(result.GetErrorMessage());
    }
    std::vector<std::uint32_t> words(result.cbegin(), result.cend());
    if (!path.empty()) writeCache(path, words);
    return words;
}

}  // namespace tox2
