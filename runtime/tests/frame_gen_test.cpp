// Self-checking tests for the frame generation maths. No Vulkan device and no Streamline are needed.

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <string_view>

#include "render/frame_gen.h"

namespace {

using tox2::FlipRateTracker;
using tox2::FrameGenTarget;

int g_passed;
int g_failed;

#define CHECK(cond)                                                                \
    do {                                                                           \
        if (cond) {                                                                \
            g_passed++;                                                            \
        } else {                                                                   \
            g_failed++;                                                            \
            std::fprintf(stderr, "check failed (line %d): %s\n", __LINE__, #cond); \
        }                                                                          \
    } while (0)

constexpr FrameGenTarget kTargets[] = {FrameGenTarget::Off,    FrameGenTarget::Fps60,  FrameGenTarget::Fps90,
                                       FrameGenTarget::Fps120, FrameGenTarget::Fps180, FrameGenTarget::Unlimited};

void testNames() {
    constexpr std::string_view kNames[] = {"off", "60", "90", "120", "180", "unlimited"};
    for (std::size_t i = 0; i < 6; ++i) {
        CHECK(tox2::frameGenTargetName(kTargets[i]) == kNames[i]);
        const auto parsed = tox2::parseFrameGenTarget(kNames[i]);
        CHECK(parsed.has_value() && *parsed == kTargets[i]);
    }
    for (const std::string_view bad : {"120 ", " 120", "Off", "OFF", "Unlimited", "", "30", "240", "0"}) {
        CHECK(!tox2::parseFrameGenTarget(bad).has_value());
    }
    CHECK(tox2::frameGenTargetFps(FrameGenTarget::Off) == 0);
    CHECK(tox2::frameGenTargetFps(FrameGenTarget::Fps60) == 60);
    CHECK(tox2::frameGenTargetFps(FrameGenTarget::Fps90) == 90);
    CHECK(tox2::frameGenTargetFps(FrameGenTarget::Fps120) == 120);
    CHECK(tox2::frameGenTargetFps(FrameGenTarget::Fps180) == 180);
    CHECK(tox2::frameGenTargetFps(FrameGenTarget::Unlimited) == 0);
}

void testMultiplier() {
    using tox2::frameGenMultiplier;
    // The D3 table: max 6, no cap. Columns: game 30 fps (2 vblanks), game 60 fps (1 vblank).
    const std::uint32_t expected[6][2] = {{1, 1}, {2, 1}, {3, 1}, {4, 2}, {6, 3}, {6, 6}};
    for (std::size_t i = 0; i < 6; ++i) {
        CHECK(frameGenMultiplier(kTargets[i], 2, 6, 0) == expected[i][0]);
        CHECK(frameGenMultiplier(kTargets[i], 1, 6, 0) == expected[i][1]);
    }

    // An RTX 40 GPU generates at most one frame.
    CHECK(frameGenMultiplier(FrameGenTarget::Fps120, 2, 2, 0) == 2);
    CHECK(frameGenMultiplier(FrameGenTarget::Fps180, 2, 2, 0) == 2);
    CHECK(frameGenMultiplier(FrameGenTarget::Unlimited, 2, 2, 0) == 2);
    CHECK(frameGenMultiplier(FrameGenTarget::Fps180, 1, 2, 0) == 2);
    CHECK(frameGenMultiplier(FrameGenTarget::Fps60, 2, 2, 0) == 2);
    CHECK(frameGenMultiplier(FrameGenTarget::Fps90, 2, 2, 0) == 2);

    // The refresh rate of a FIFO swapchain caps the presented rate.
    CHECK(frameGenMultiplier(FrameGenTarget::Unlimited, 2, 6, 144) == 4);
    CHECK(frameGenMultiplier(FrameGenTarget::Fps180, 2, 6, 144) == 4);
    CHECK(frameGenMultiplier(FrameGenTarget::Fps120, 2, 6, 144) == 4);
    CHECK(frameGenMultiplier(FrameGenTarget::Fps120, 2, 6, 60) == 2);
    CHECK(frameGenMultiplier(FrameGenTarget::Fps120, 1, 6, 50) == 1);
    CHECK(frameGenMultiplier(FrameGenTarget::Unlimited, 1, 6, 50) == 1);
    CHECK(frameGenMultiplier(FrameGenTarget::Unlimited, 1, 6, 144) == 2);

    // Anything but 60 and 30 fps is left alone, and a missing maximum never gives less than 1.
    for (const FrameGenTarget target : kTargets) {
        CHECK(frameGenMultiplier(target, 0, 6, 0) == 1);
        CHECK(frameGenMultiplier(target, 3, 6, 0) == 1);
        CHECK(frameGenMultiplier(target, 4, 6, 0) == 1);
        CHECK(frameGenMultiplier(target, 2, 0, 0) == 1);
    }
}

// Feeds `count` flips `interval_us` apart; returns the last update and whether any changed the class.
struct Run {
    FlipRateTracker::Update last;
    int changes = 0;
    bool discontinuity = false;
};

Run feed(FlipRateTracker& tracker, std::uint64_t& now_us, int count, std::uint64_t interval_us) {
    Run run;
    for (int i = 0; i < count; ++i) {
        now_us += interval_us;
        run.last = tracker.onFlip(now_us);
        run.changes += run.last.changed ? 1 : 0;
        run.discontinuity = run.discontinuity || run.last.discontinuity;
    }
    return run;
}

void testFlipRateTracker() {
    {
        // The first flip has no interval, so the class is adopted at the 7th flip (6th interval).
        FlipRateTracker tracker;
        std::uint64_t now = 1'000'000;
        CHECK(tracker.onFlip(now).vblanks == 0);
        Run run = feed(tracker, now, 5, 16'667);
        CHECK(run.last.vblanks == 0 && run.changes == 0);
        run = feed(tracker, now, 1, 16'667);
        CHECK(run.last.vblanks == 1 && run.changes == 1 && tracker.vblanks() == 1);
        run = feed(tracker, now, 100, 16'667);
        CHECK(run.last.vblanks == 1 && run.changes == 0 && !run.discontinuity);

        // Jitter of +-2 ms on the intervals keeps the class.
        for (int i = 0; i < 50; ++i) {
            run = feed(tracker, now, 1, i % 2 == 0 ? 16'667 + 2'000 : 16'667 - 2'000);
            CHECK(run.last.vblanks == 1 && !run.last.changed);
        }

        // 30 fps: the old class holds until the 6th interval.
        run = feed(tracker, now, 5, 33'333);
        CHECK(run.last.vblanks == 1 && run.changes == 0);
        run = feed(tracker, now, 1, 33'333);
        CHECK(run.last.vblanks == 2 && run.changes == 1 && tracker.vblanks() == 2);

        // One hitch of 50 ms (3 vblanks) does not change the class and does not break the 30 fps run.
        run = feed(tracker, now, 1, 50'000);
        CHECK(run.last.vblanks == 2 && !run.last.changed && !run.last.discontinuity);
        run = feed(tracker, now, 20, 33'333);
        CHECK(run.last.vblanks == 2 && run.changes == 0);

        // A stall beyond 100 ms is a discontinuity; the class stays and measuring resumes after it.
        run = feed(tracker, now, 1, 150'000);
        CHECK(run.last.discontinuity && !run.last.changed && run.last.vblanks == 2);
        run = feed(tracker, now, 1, 33'333);
        CHECK(!run.last.discontinuity && run.last.vblanks == 2);

        // 25 ms is not within 4 ms of a whole number of vblanks: irregular, no class after 6.
        run = feed(tracker, now, 5, 25'000);
        CHECK(run.last.vblanks == 2 && run.changes == 0);
        run = feed(tracker, now, 1, 25'000);
        CHECK(run.last.vblanks == 0 && run.changes == 1 && tracker.vblanks() == 0);
    }
    {
        // Irregular from the start never adopts a class.
        FlipRateTracker tracker;
        std::uint64_t now = 5'000;
        tracker.onFlip(now);
        const Run run = feed(tracker, now, 20, 25'000);
        CHECK(run.last.vblanks == 0 && run.changes == 0);
    }
    {
        // A replay has no flip time: nothing is learned.
        FlipRateTracker tracker;
        for (int i = 0; i < 20; ++i) {
            const auto update = tracker.onFlip(0);
            CHECK(update.vblanks == 0 && !update.changed && !update.discontinuity);
        }
    }
    {
        // Intervals slower than 30 fps are adopted as their own class.
        FlipRateTracker tracker;
        std::uint64_t now = 1'000;
        tracker.onFlip(now);
        const Run run = feed(tracker, now, 6, 50'000);
        CHECK(run.last.vblanks == 3 && run.changes == 1);
    }
}

// ---- fgCameraFromClip ----

using Vec3 = std::array<double, 3>;

Vec3 sub(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
double dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
Vec3 normalized(const Vec3& a) {
    const double length = std::sqrt(dot(a, a));
    return {a[0] / length, a[1] / length, a[2] / length};
}
Vec3 toVec(const std::array<float, 3>& a) { return {a[0], a[1], a[2]}; }

struct LookAt {
    tox2::Mat4 matrix;
    Vec3 right, up, fwd;
};

// Left handed look-at (z forward) as a row-vector matrix: p_view = (p - eye) . (right, up, fwd).
LookAt lookAt(const Vec3& eye, const Vec3& target, const Vec3& up_hint) {
    LookAt view;
    view.fwd = normalized(sub(target, eye));
    view.right = normalized(cross(up_hint, view.fwd));
    view.up = cross(view.fwd, view.right);
    const Vec3* axes[3] = {&view.right, &view.up, &view.fwd};
    view.matrix = tox2::mat4Identity();
    for (std::size_t col = 0; col < 3; ++col) {
        for (std::size_t row = 0; row < 3; ++row) view.matrix[row * 4 + col] = static_cast<float>((*axes[col])[row]);
        view.matrix[12 + col] = static_cast<float>(-dot(eye, *axes[col]));
    }
    return view;
}

// y-up perspective, depth 0..1, row vectors, view-space z forward.
tox2::Mat4 perspective(double fov_y, double aspect, double near_plane, double far_plane) {
    const double f = 1.0 / std::tan(fov_y * 0.5);
    tox2::Mat4 m{};
    m[0] = static_cast<float>(f / aspect);
    m[5] = static_cast<float>(f);
    m[10] = static_cast<float>(far_plane / (far_plane - near_plane));
    m[11] = 1.0f;
    m[14] = static_cast<float>(-near_plane * far_plane / (far_plane - near_plane));
    return m;
}

tox2::Mat4 orthographic(double width, double height, double near_plane, double far_plane) {
    tox2::Mat4 m = tox2::mat4Identity();
    m[0] = static_cast<float>(2.0 / width);
    m[5] = static_cast<float>(2.0 / height);
    m[10] = static_cast<float>(1.0 / (far_plane - near_plane));
    m[14] = static_cast<float>(-near_plane / (far_plane - near_plane));
    return m;
}

bool near(double a, double b, double tolerance) { return std::abs(a - b) <= tolerance; }

bool vecNear(const Vec3& a, const Vec3& b, double tolerance) {
    return near(a[0], b[0], tolerance) && near(a[1], b[1], tolerance) && near(a[2], b[2], tolerance);
}

void checkCamera(const tox2::FgCamera& camera, const LookAt& view, const Vec3& eye, double fov_y, double aspect,
                 double near_plane, double far_plane) {
    CHECK(vecNear(toVec(camera.pos), eye, 1e-3));
    CHECK(vecNear(toVec(camera.fwd), view.fwd, 1e-3));
    CHECK(vecNear(toVec(camera.up), view.up, 1e-3));
    CHECK(vecNear(toVec(camera.right), view.right, 1e-3));
    CHECK(near(camera.near_plane, near_plane, near_plane * 0.005));
    CHECK(near(camera.far_plane, far_plane, far_plane * 0.005));
    CHECK(near(camera.fov_y, fov_y, 1e-3));
    CHECK(near(camera.aspect, aspect, 1e-3));

    const Vec3 right = toVec(camera.right), up = toVec(camera.up), fwd = toVec(camera.fwd);
    CHECK(near(dot(right, right), 1.0, 1e-5) && near(dot(up, up), 1.0, 1e-5) && near(dot(fwd, fwd), 1.0, 1e-5));
    CHECK(near(dot(right, up), 0.0, 1e-5) && near(dot(right, fwd), 0.0, 1e-5) && near(dot(up, fwd), 0.0, 1e-5));
}

void testCamera() {
    constexpr double kFovY = 60.0 * 3.14159265358979323846 / 180.0;
    constexpr double kAspect = 16.0 / 9.0;
    const Vec3 eye{3.0, 2.0, -5.0};

    const LookAt view = lookAt(eye, {0.0, 0.0, 0.0}, {0.0, 1.0, 0.0});
    const tox2::Mat4 clip = tox2::mat4Multiply(view.matrix, perspective(kFovY, kAspect, 0.5, 200.0));
    const auto camera = tox2::fgCameraFromClip(clip);
    CHECK(camera.has_value());
    if (camera) checkCamera(*camera, view, eye, kFovY, kAspect, 0.5, 200.0);

    // Another pose and projection, so the result does not depend on the first one's symmetries.
    const Vec3 eye2{-40.0, 12.0, 7.5};
    const LookAt view2 = lookAt(eye2, {10.0, -3.0, 25.0}, {0.0, 1.0, 0.0});
    const tox2::Mat4 clip2 = tox2::mat4Multiply(view2.matrix, perspective(0.9, 1.5, 1.0, 1000.0));
    const auto camera2 = tox2::fgCameraFromClip(clip2);
    CHECK(camera2.has_value());
    if (camera2) checkCamera(*camera2, view2, eye2, 0.9, 1.5, 1.0, 1000.0);

    CHECK(!tox2::fgCameraFromClip(tox2::mat4Multiply(view.matrix, orthographic(20.0, 11.25, 0.5, 200.0))));
    CHECK(!tox2::fgCameraFromClip(tox2::mat4Identity()));
    CHECK(!tox2::fgCameraFromClip(tox2::Mat4{}));

    tox2::Mat4 singular = clip;
    for (std::size_t row = 0; row < 4; ++row) singular[row * 4 + 1] = singular[row * 4 + 0];
    CHECK(!tox2::fgCameraFromClip(singular));
}

}  // namespace

int main() {
    testNames();
    testMultiplier();
    testFlipRateTracker();
    testCamera();
    std::printf("frame gen tests: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed != 0 ? 1 : 0;
}
