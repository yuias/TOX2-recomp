#pragma once

// CPU maths for the DLAA pass: the jitter sequence, the perspective gate and the camera matrices handed to
// Streamline. No Vulkan, so it is testable without a device.
//
// Matrices are row-major and act on row vectors (p' = p * m), the convention of Streamline's sl::float4x4 and of the
// guest's vertex constants c0..c3 (clip = x*c0 + y*c1 + z*c2 + w*c3), so the guest rows need no transposition.

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

namespace tox2 {

using Mat4 = std::array<float, 16>;

// The DLSS programming guide asks for at least 8 * (output / input)^2 phases; the ratio is 1 for DLAA.
inline constexpr std::uint32_t kDlaaJitterPhases = 8;

// A projected point further than this from where it started (NDC units, the full range is 2) is a camera cut.
inline constexpr float kDlaaCameraCutNdc = 0.5f;

// Radical inverse of `index` (>= 1) in `base`, in [0, 1).
constexpr float halton(std::uint32_t index, std::uint32_t base) {
    double result = 0.0;
    double fraction = 1.0;
    while (index > 0) {
        fraction /= base;
        result += fraction * (index % base);
        index /= base;
    }
    return static_cast<float>(result);
}

// Sub-pixel jitter of a frame in input pixels, x right and y down, centred on 0 (each component in [-0.5, 0.5)).
// Halton(2, 3) from index 1, as index 0 would give (0, 0).
inline std::array<float, 2> dlaaJitterPixels(std::uint64_t frame) {
    const auto index = static_cast<std::uint32_t>(frame % kDlaaJitterPhases) + 1;
    return {halton(index, 2) - 0.5f, halton(index, 3) - 0.5f};
}

// True when the draw uses a perspective projection. With clip.w = x*c0.w + y*c1.w + z*c2.w + w*c3.w, an
// orthographic matrix has c0.w = c1.w = c2.w = 0 (floats 3, 7 and 11 of the vertex constants c0..c3), so only a
// perspective draw is jittered: 2D and screen-space draws must not move. False with fewer than 16 words.
inline bool dlaaPerspective(std::span<const std::uint32_t> vp_constants) {
    if (vp_constants.size() < 16) return false;
    for (const std::size_t word : {std::size_t{3}, std::size_t{7}, std::size_t{11}}) {
        if (std::bit_cast<float>(vp_constants[word]) != 0.0f) return true;
    }
    return false;
}

inline Mat4 mat4Identity() { return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; }

// The matrix that applies `a`, then `b`, to a row vector.
inline Mat4 mat4Multiply(const Mat4& a, const Mat4& b) {
    Mat4 out{};
    for (std::size_t row = 0; row < 4; ++row) {
        for (std::size_t col = 0; col < 4; ++col) {
            double sum = 0.0;
            for (std::size_t k = 0; k < 4; ++k) sum += double{a[row * 4 + k]} * double{b[k * 4 + col]};
            out[row * 4 + col] = static_cast<float>(sum);
        }
    }
    return out;
}

// Gauss-Jordan with partial pivoting in double; false (and `out` untouched) for a singular or non-finite matrix.
inline bool mat4Inverse(const Mat4& m, Mat4& out) {
    double a[4][8] = {};
    double largest = 0.0;
    for (std::size_t row = 0; row < 4; ++row) {
        for (std::size_t col = 0; col < 4; ++col) {
            const double value = m[row * 4 + col];
            if (!std::isfinite(value)) return false;
            a[row][col] = value;
            largest = std::max(largest, std::abs(value));
        }
        a[row][4 + row] = 1.0;
    }
    if (largest == 0.0) return false;
    // Relative to the largest entry, so a matrix of small numbers is not mistaken for a singular one.
    const double pivot_floor = largest * 1e-12;
    for (std::size_t col = 0; col < 4; ++col) {
        std::size_t pivot = col;
        for (std::size_t row = col + 1; row < 4; ++row) {
            if (std::abs(a[row][col]) > std::abs(a[pivot][col])) pivot = row;
        }
        if (std::abs(a[pivot][col]) <= pivot_floor) return false;
        if (pivot != col) {
            for (std::size_t k = 0; k < 8; ++k) std::swap(a[col][k], a[pivot][k]);
        }
        const double scale = 1.0 / a[col][col];
        for (std::size_t k = 0; k < 8; ++k) a[col][k] *= scale;
        for (std::size_t row = 0; row < 4; ++row) {
            if (row == col) continue;
            const double factor = a[row][col];
            if (factor == 0.0) continue;
            for (std::size_t k = 0; k < 8; ++k) a[row][k] -= factor * a[col][k];
        }
    }
    Mat4 result{};
    for (std::size_t row = 0; row < 4; ++row) {
        for (std::size_t col = 0; col < 4; ++col) {
            const double value = a[row][4 + col];
            if (!std::isfinite(value)) return false;
            result[row * 4 + col] = static_cast<float>(value);
        }
    }
    out = result;
    return true;
}

// p * m for a row vector.
inline std::array<float, 4> mat4Transform(const std::array<float, 4>& p, const Mat4& m) {
    std::array<float, 4> out{};
    for (std::size_t col = 0; col < 4; ++col) {
        double sum = 0.0;
        for (std::size_t k = 0; k < 4; ++k) sum += double{p[k]} * double{m[k * 4 + col]};
        out[col] = static_cast<float>(sum);
    }
    return out;
}

// What identifies one camera within a frame: the guest's c0..c3 and everything the vertex epilogue combines with
// the clip position.
struct DlaaCameraKey {
    Mat4 mvp{};  // c0..c3
    float viewport_scale[3] = {};
    float viewport_offset[3] = {};
    float clip_w = 0.0f, clip_h = 0.0f, clip_min = 0.0f, clip_max = 1.0f;
    bool operator==(const DlaaCameraKey&) const = default;
};

// MVP * V * Dr: maps a world position to the clip space the draw leaves in gl_Position, whose depth is the one in
// the depth buffer. V is the vertex epilogue's viewport transform (x and y normalised to the clip size, no y flip),
// Dr its depth range remap (identity for the range (0, 1)). The epilogue's snap of z onto a 24-bit grid is not part
// of it; it only removes rounding noise.
inline Mat4 dlaaClipMatrix(const DlaaCameraKey& key) {
    const float half_w = 0.5f * key.clip_w;
    const float half_h = 0.5f * key.clip_h;
    Mat4 viewport = mat4Identity();
    viewport[0] = key.viewport_scale[0] / half_w;
    viewport[5] = key.viewport_scale[1] / half_h;
    viewport[10] = key.viewport_scale[2];
    viewport[12] = (key.viewport_offset[0] - half_w) / half_w;
    viewport[13] = (key.viewport_offset[1] - half_h) / half_h;
    viewport[14] = key.viewport_offset[2];

    Mat4 depth = mat4Identity();
    if (key.clip_min != 0.0f || key.clip_max != 1.0f) {
        const float near_plane = std::min(key.clip_min, key.clip_max);
        const float range = std::max(key.clip_min, key.clip_max) - near_plane;
        // The epilogue writes depth 0 for a degenerate range.
        depth[10] = range > 1e-6f ? 1.0f / range : 0.0f;
        depth[14] = range > 1e-6f ? -near_plane / range : 0.0f;
    }
    return mat4Multiply(mat4Multiply(key.mvp, viewport), depth);
}

// True when the camera moved so far that the previous frame is not a usable history: a test point leaves the view
// (w <= 0, or not a number) or lands more than kDlaaCameraCutNdc from where it started. `clip_to_prev_clip` maps
// the current frame's clip space onto the previous frame's.
inline bool dlaaCameraCut(const Mat4& clip_to_prev_clip) {
    constexpr float kCoords[3] = {-0.5f, 0.0f, 0.5f};
    for (const float x : kCoords) {
        for (const float y : kCoords) {
            const std::array<float, 4> p = mat4Transform({x, y, 0.5f, 1.0f}, clip_to_prev_clip);
            if (!(p[3] > 0.0f)) return true;
            const float dx = p[0] / p[3] - x;
            const float dy = p[1] / p[3] - y;
            if (!(std::hypot(dx, dy) <= kDlaaCameraCutNdc)) return true;
        }
    }
    return false;
}

// F * m * F with F = diag(1, -1, 1, 1): the same transform for a clip space whose y axis is mirrored.
inline Mat4 dlaaFlipY(const Mat4& m) {
    constexpr float kSign[4] = {1.0f, -1.0f, 1.0f, 1.0f};
    Mat4 out = m;
    for (std::size_t row = 0; row < 4; ++row) {
        for (std::size_t col = 0; col < 4; ++col) out[row * 4 + col] *= kSign[row] * kSign[col];
    }
    return out;
}

}  // namespace tox2
