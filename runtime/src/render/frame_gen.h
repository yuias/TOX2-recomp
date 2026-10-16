#pragma once

// Frame generation maths: the presented-rate target and the multiplier it implies, the game's flip rate and the
// camera description the generator needs. No Vulkan and no Streamline, so it is testable without a device.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "render/dlaa_math.h"

namespace tox2 {

// The presented frame rate the user asks for. Frame generation never changes the game's own flip rate.
enum class FrameGenTarget : std::uint8_t { Off, Fps60, Fps90, Fps120, Fps180, Unlimited };

inline std::string_view frameGenTargetName(FrameGenTarget target) {
    switch (target) {
        case FrameGenTarget::Off: return "off";
        case FrameGenTarget::Fps60: return "60";
        case FrameGenTarget::Fps90: return "90";
        case FrameGenTarget::Fps120: return "120";
        case FrameGenTarget::Fps180: return "180";
        case FrameGenTarget::Unlimited: return "unlimited";
    }
    return "off";
}

// Exactly the names frameGenTargetName returns: no case folding and no surrounding whitespace.
inline std::optional<FrameGenTarget> parseFrameGenTarget(std::string_view text) {
    for (const FrameGenTarget target : {FrameGenTarget::Off, FrameGenTarget::Fps60, FrameGenTarget::Fps90,
                                        FrameGenTarget::Fps120, FrameGenTarget::Fps180, FrameGenTarget::Unlimited}) {
        if (text == frameGenTargetName(target)) return target;
    }
    return std::nullopt;
}

// 60, 90, 120 or 180; 0 for Off and Unlimited, which name no rate.
inline std::uint32_t frameGenTargetFps(FrameGenTarget target) {
    switch (target) {
        case FrameGenTarget::Fps60: return 60;
        case FrameGenTarget::Fps90: return 90;
        case FrameGenTarget::Fps120: return 120;
        case FrameGenTarget::Fps180: return 180;
        case FrameGenTarget::Off:
        case FrameGenTarget::Unlimited: break;
    }
    return 0;
}

inline constexpr double kFrameGenVblankUs = 1'000'000.0 / 60.0;

// Presented frames per game frame; 1 = no generation. game_vblanks: 1 (60 fps) or 2 (30 fps), anything else gives 1.
// max_multiplier = 1 + DLSSGState::numFramesToGenerateMax. cap_fps 0 = no cap, otherwise game fps x m <= cap_fps.
// m = floor(target / game fps) for a fixed target, max_multiplier for Unlimited; then capped and clamped to >= 1.
// The ratio is floored because a non-integer one would need the game paced to fit, which is not ours to do.
inline std::uint32_t frameGenMultiplier(FrameGenTarget target, std::uint32_t game_vblanks, std::uint32_t max_multiplier,
                                        std::uint32_t cap_fps) {
    if (target == FrameGenTarget::Off || (game_vblanks != 1 && game_vblanks != 2)) return 1;
    const std::uint32_t game_fps = 60 / game_vblanks;
    std::uint32_t m = target == FrameGenTarget::Unlimited ? max_multiplier : frameGenTargetFps(target) / game_fps;
    if (cap_fps > 0) m = std::min(m, cap_fps / game_fps);
    m = std::min(m, max_multiplier);
    return std::max<std::uint32_t>(m, 1);
}

// Classifies the game's flip interval from the flips' completion times. A class (1 = 60 fps, 2 = 30 fps, 3+ = slower)
// is adopted after kConfirmFlips consecutive intervals of that class; an interval more than kToleranceUs away from a
// whole number of vblanks is irregular (no class; --frame-limit off or a hitch). Irregular intervals are a class of
// their own (0), so the adopted class also falls back to 0 after kConfirmFlips of them in a row.
class FlipRateTracker {
public:
    static constexpr std::uint32_t kConfirmFlips = 6;
    static constexpr double kToleranceUs = 4000.0;
    static constexpr std::uint64_t kDiscontinuityUs = 100'000;
    struct Update {
        std::uint32_t vblanks = 0;   // the adopted class after this flip; 0 = none yet or irregular
        bool changed = false;        // the adopted class changed at this flip
        bool discontinuity = false;  // this interval exceeded kDiscontinuityUs (history must be reset)
    };

    // completed_us 0 (a replay, which has no flip time) and a time that goes backwards are ignored.
    Update onFlip(std::uint64_t completed_us) {
        Update update;
        update.vblanks = adopted_;
        if (completed_us == 0) return update;
        const std::uint64_t previous = last_us_;
        last_us_ = completed_us;
        if (previous == 0 || completed_us <= previous) return update;

        const std::uint64_t interval = completed_us - previous;
        if (interval > kDiscontinuityUs) {
            // A stall says nothing about the rate that follows: the run restarts, the adopted class stays.
            update.discontinuity = true;
            run_ = 0;
            return update;
        }

        const std::uint32_t observed = classify(static_cast<double>(interval));
        if (run_ > 0 && observed == candidate_) {
            ++run_;
        } else {
            candidate_ = observed;
            run_ = 1;
        }
        if (run_ >= kConfirmFlips && candidate_ != adopted_) {
            adopted_ = candidate_;
            update.changed = true;
        }
        update.vblanks = adopted_;
        return update;
    }

    std::uint32_t vblanks() const { return adopted_; }

private:
    // The number of whole vblanks the interval spans, 0 when it is not within tolerance of one.
    static std::uint32_t classify(double interval_us) {
        const double count = std::round(interval_us / kFrameGenVblankUs);
        if (count < 1.0) return 0;
        return std::abs(interval_us - count * kFrameGenVblankUs) <= kToleranceUs ? static_cast<std::uint32_t>(count)
                                                                                 : 0;
    }

    std::uint64_t last_us_ = 0;
    std::uint32_t adopted_ = 0, candidate_ = 0, run_ = 0;
};

struct FgCamera {
    std::array<float, 3> pos{}, up{0.0f, 1.0f, 0.0f}, right{1.0f, 0.0f, 0.0f}, fwd{0.0f, 0.0f, 1.0f};  // orthonormal
    float near_plane = 1.0f, far_plane = 1000.0f, fov_y = 1.0f, aspect = 1.0f;
};

namespace fg_detail {

using Vec3 = std::array<double, 3>;

inline Vec3 sub(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
inline double dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline double length(const Vec3& a) { return std::sqrt(dot(a, a)); }
inline Vec3 scaled(const Vec3& a, double s) { return {a[0] * s, a[1] * s, a[2] * s}; }
inline Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

inline double det3(const double (&m)[3][3]) {
    return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
           m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
}

inline std::array<float, 3> toFloat3(const Vec3& v) {
    return {static_cast<float>(v[0]), static_cast<float>(v[1]), static_cast<float>(v[2])};
}

}  // namespace fg_detail

// From a row-vector world-to-clip matrix (p_clip = p * m, clip y up, depth 0..1), the matrix handed to Streamline as
// cameraViewToClip: centre = null vector of the x, y, w columns; near/far = distances from the centre to the
// unprojected NDC (0,0,0)/(0,0,1); fwd = their direction; up from NDC (0,1,z) orthogonalised (Gram-Schmidt);
// right = cross(up, fwd) with the handedness that makes clip x increase along it; fov_y from the rays through NDC
// y = -1 and +1; aspect from the x extent. nullopt when the matrix is not a perspective projection or is singular.
inline std::optional<FgCamera> fgCameraFromClip(const Mat4& world_to_clip) {
    using namespace fg_detail;

    // The eye is the only point whose clip x, y and w all vanish. For an orthographic matrix the w column is
    // (0, 0, 0, 1), which puts that point at infinity.
    double rows[3][4];
    for (std::size_t i = 0; i < 4; ++i) {
        rows[0][i] = world_to_clip[i * 4 + 0];
        rows[1][i] = world_to_clip[i * 4 + 1];
        rows[2][i] = world_to_clip[i * 4 + 3];
    }
    double null_vector[4];
    double largest = 0.0;
    for (std::size_t skip = 0; skip < 4; ++skip) {
        double minor[3][3];
        for (std::size_t r = 0; r < 3; ++r) {
            std::size_t c = 0;
            for (std::size_t col = 0; col < 4; ++col) {
                if (col != skip) minor[r][c++] = rows[r][col];
            }
        }
        null_vector[skip] = (skip % 2 == 0 ? 1.0 : -1.0) * det3(minor);
        if (!std::isfinite(null_vector[skip])) return std::nullopt;
        largest = std::max(largest, std::abs(null_vector[skip]));
    }
    if (largest == 0.0 || std::abs(null_vector[3]) <= largest * 1e-9) return std::nullopt;
    const Vec3 centre{null_vector[0] / null_vector[3], null_vector[1] / null_vector[3],
                      null_vector[2] / null_vector[3]};

    Mat4 clip_to_world;
    if (!mat4Inverse(world_to_clip, clip_to_world)) return std::nullopt;
    // A point in front of the camera has h.w > 0, because its clip w is 1 / h.w.
    const auto unproject = [&](double x, double y, double z, Vec3& out) {
        const std::array<float, 4> h = mat4Transform(
                {static_cast<float>(x), static_cast<float>(y), static_cast<float>(z), 1.0f}, clip_to_world);
        if (!(h[3] > 0.0f)) return false;
        out = {double{h[0]} / h[3], double{h[1]} / h[3], double{h[2]} / h[3]};
        return true;
    };
    Vec3 near_centre, far_centre, near_top, near_bottom, near_right, near_left;
    if (!unproject(0, 0, 0, near_centre) || !unproject(0, 0, 1, far_centre) || !unproject(0, 1, 0, near_top) ||
        !unproject(0, -1, 0, near_bottom) || !unproject(1, 0, 0, near_right) || !unproject(-1, 0, 0, near_left)) {
        return std::nullopt;
    }

    const double near_distance = length(sub(near_centre, centre));
    const double far_distance = length(sub(far_centre, centre));
    if (!(near_distance > 0.0) || !(far_distance > near_distance) || !std::isfinite(far_distance)) return std::nullopt;

    const Vec3 axis = sub(far_centre, near_centre);
    const double axis_length = length(axis);
    if (!(axis_length > 0.0)) return std::nullopt;
    const Vec3 fwd = scaled(axis, 1.0 / axis_length);

    const Vec3 up_raw = sub(near_top, near_centre);
    const Vec3 up_ortho = sub(up_raw, scaled(fwd, dot(up_raw, fwd)));
    const double up_length = length(up_ortho);
    if (!(up_length > 0.0)) return std::nullopt;
    const Vec3 up = scaled(up_ortho, 1.0 / up_length);

    Vec3 right = cross(up, fwd);
    // The sign that makes clip x grow along `right`; it differs between left and right handed worlds.
    if (dot(right, sub(near_right, near_left)) < 0.0) right = scaled(right, -1.0);

    const Vec3 to_top = sub(near_top, centre);
    const Vec3 to_bottom = sub(near_bottom, centre);
    const double fov_y = std::atan2(length(cross(to_bottom, to_top)), dot(to_bottom, to_top));
    const double height = length(sub(near_top, near_bottom));
    if (!(height > 0.0)) return std::nullopt;
    const double aspect = length(sub(near_right, near_left)) / height;

    FgCamera camera;
    camera.pos = toFloat3(centre);
    camera.up = toFloat3(up);
    camera.right = toFloat3(right);
    camera.fwd = toFloat3(fwd);
    camera.near_plane = static_cast<float>(near_distance);
    camera.far_plane = static_cast<float>(far_distance);
    camera.fov_y = static_cast<float>(fov_y);
    camera.aspect = static_cast<float>(aspect);
    if (!std::isfinite(camera.fov_y) || !std::isfinite(camera.aspect) || !(camera.fov_y > 0.0f)) return std::nullopt;
    return camera;
}

}  // namespace tox2
