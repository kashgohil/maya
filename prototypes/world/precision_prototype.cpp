// Coordinate precision prototype for the world-scale decisions (#1060, docs/architecture/world-scale-decision.md).
//
// Built twice from this file: maya_precision_prototype links the engine's single-precision Jolt, and
// maya_precision_prototype_double a copy built with JPH_DOUBLE_PRECISION. At origin offsets of 0 m,
// 100 m, 1 km, 2 km, 4 km, 8 km, and 10 km it measures:
//   - float spacing (the gap between neighbouring representable positions);
//   - rendering: where 2,000 vertices of an object 5 m from the camera land on a 1920x1080 view, computed
//     in float world space (as Maya renders today) and camera-relative (offset subtracted in double), against
//     a double reference, in pixels;
//   - picking: a ray from the camera to a ground point 30 m away, intersected in float, against double, in mm;
//   - physics (this build's Jolt precision): a 10-box stack resting for 600 ticks (top drift), a sphere rolling
//     at 5 m/s for 300 ticks (sideways error), and a downward ray cast (hit error);
//   - rebasing (single precision only): moving every body of a 5,000- and a 50,000-body world by a whole
//     offset, and whether a resting stack survives a rebase every 120 ticks;
//   - step cost: 5,000 boxes falling into a pile at the origin.
// Lines starting UNEXPECTED report what the decision relies on and make the run fail.

#include "world/jolt_scene.hpp"

#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/RayCast.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
double ms_since(Clock::time_point start) { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); }
int failures = 0;
void unexpected(const std::string& what) {
    std::printf("UNEXPECTED: %s\n", what.c_str());
    ++failures;
}
#ifdef JPH_DOUBLE_PRECISION
constexpr auto precision_name = "double";
constexpr bool double_precision = true;
#else
constexpr auto precision_name = "single";
constexpr bool double_precision = false;
#endif

constexpr double offsets[] = {0, 100, 1000, 2000, 4000, 8000, 10000};

// --- Minimal math in a chosen precision, so each path's rounding is explicit. ------------------------
template<class T> struct V3 {
    T x, y, z;
    V3 operator+(V3 o) const { return {x + o.x, y + o.y, z + o.z}; }
    V3 operator-(V3 o) const { return {x - o.x, y - o.y, z - o.z}; }
    V3 operator*(T s) const { return {x * s, y * s, z * s}; }
    template<class U> V3<U> as() const { return {U(x), U(y), U(z)}; }
};
template<class T> T dot(V3<T> a, V3<T> b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
template<class T> V3<T> cross(V3<T> a, V3<T> b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
template<class T> V3<T> normalize(V3<T> v) { return v * (T(1) / std::sqrt(dot(v, v))); }
template<class T> struct M4 { // column-major, m[column][row]
    T m[4][4]{};
    V3<T> point(V3<T> p, T* w_out = nullptr) const {
        const auto x = m[0][0] * p.x + m[1][0] * p.y + m[2][0] * p.z + m[3][0];
        const auto y = m[0][1] * p.x + m[1][1] * p.y + m[2][1] * p.z + m[3][1];
        const auto z = m[0][2] * p.x + m[1][2] * p.y + m[2][2] * p.z + m[3][2];
        const auto w = m[0][3] * p.x + m[1][3] * p.y + m[2][3] * p.z + m[3][3];
        if (w_out) *w_out = w;
        return {x, y, z};
    }
    M4 operator*(const M4& b) const {
        M4 r;
        for (int c = 0; c < 4; ++c)
            for (int row = 0; row < 4; ++row)
                for (int k = 0; k < 4; ++k) r.m[c][row] += m[k][row] * b.m[c][k];
        return r;
    }
};
template<class T> M4<T> translation(V3<T> t) {
    M4<T> r;
    for (int i = 0; i < 4; ++i) r.m[i][i] = 1;
    r.m[3][0] = t.x, r.m[3][1] = t.y, r.m[3][2] = t.z;
    return r;
}
// Right-handed look-at, as Maya's camera_matrices builds the view from a rigid pose.
template<class T> M4<T> look_at(V3<T> eye, V3<T> target) {
    const auto f = normalize(target - eye), s = normalize(cross(f, V3<T>{0, 1, 0})), u = cross(s, f);
    M4<T> r;
    r.m[0][0] = s.x, r.m[1][0] = s.y, r.m[2][0] = s.z;
    r.m[0][1] = u.x, r.m[1][1] = u.y, r.m[2][1] = u.z;
    r.m[0][2] = -f.x, r.m[1][2] = -f.y, r.m[2][2] = -f.z;
    r.m[3][0] = -dot(s, eye), r.m[3][1] = -dot(u, eye), r.m[3][2] = dot(f, eye), r.m[3][3] = 1;
    return r;
}
// Reversed-Z-free perspective, depth 0..1, 60 degree vertical field of view, 16:9.
template<class T> M4<T> perspective() {
    const T f = T(1) / std::tan(T(0.5) * T(M_PI / 3)), aspect = T(16) / T(9), n = T(0.1), far = T(5000);
    M4<T> r;
    r.m[0][0] = f / aspect, r.m[1][1] = f, r.m[2][2] = far / (n - far), r.m[2][3] = -1, r.m[3][2] = far * n / (n - far);
    return r;
}
template<class T> std::pair<double, double> to_pixels(const M4<T>& view_projection, V3<T> world) {
    T w;
    const auto clip = view_projection.point(world, &w);
    return {(double(clip.x) / double(w) * 0.5 + 0.5) * 1920.0, (double(clip.y) / double(w) * 0.5 + 0.5) * 1080.0};
}

struct RenderError {
    double world_space, camera_relative;
};
RenderError render_error(double offset, double distance) {
    auto random = std::mt19937(990);
    auto unit = std::uniform_real_distribution<double>(-0.5, 0.5);
    auto worst = RenderError{0, 0};
    for (int trial = 0; trial < 20; ++trial) {
        // An object near the offset; the camera 5 m away and 1.7 m up, looking at it.
        const auto object = V3<double>{offset + unit(random) * 10, unit(random), offset + unit(random) * 10};
        const auto eye = object + normalize(V3<double>{3, 1.7, 4}) * distance;
        const auto reference = perspective<double>() * look_at(eye, object);
        // Float world space: positions rounded to float, matrices built and applied in float.
        const auto world_vp = perspective<float>() * look_at(eye.as<float>(), object.as<float>());
        const auto model_f = translation(object.as<float>());
        // Camera-relative: the object's offset from the eye is formed in double, then rounded.
        const auto relative_vp = perspective<float>() * look_at(V3<float>{0, 0, 0}, (object - eye).as<float>());
        const auto model_rel = translation((object - eye).as<float>());
        for (int v = 0; v < 100; ++v) {
            const auto local = V3<double>{unit(random), unit(random), unit(random)} * (distance / 10.0);
            const auto [rx, ry] = to_pixels(reference, object + local);
            const auto [wx, wy] = to_pixels(world_vp, model_f.point(local.as<float>()));
            const auto [cx, cy] = to_pixels(relative_vp, model_rel.point(local.as<float>()));
            worst.world_space = std::max(worst.world_space, std::hypot(wx - rx, wy - ry));
            worst.camera_relative = std::max(worst.camera_relative, std::hypot(cx - rx, cy - ry));
        }
    }
    return worst;
}

// A picking ray from the camera to a ground point 30 m away, intersected with the ground plane.
double pick_error_mm(double offset) {
    const auto eye = V3<double>{offset + 0.123, 1.7, offset + 0.456};
    const auto target = V3<double>{offset + 21.3, 0, offset + 21.1};
    const auto exact_t = (0 - eye.y) / (target - eye).y;
    const auto exact = eye + (target - eye) * exact_t;
    const auto eye_f = eye.as<float>(), direction = normalize((target - eye).as<float>());
    const auto t = (0 - eye_f.y) / direction.y;
    const auto hit = (eye_f + direction * t).as<double>();
    return std::hypot(hit.x - exact.x, hit.z - exact.z) * 1000.0;
}

// --- Physics at an offset, in this build's precision. -------------------------------------------------
// The same scene at every offset; its error is how far each body ends from where it ends at the origin.
struct PhysicsRun {
    std::vector<JPH::Vec3> stack; // each box's final position relative to the offset
    JPH::Vec3 sphere;
    double ray_mm;
    double rest_jitter_mm = 0; // how far any stack box moved over the last 120 ticks
    bool stack_standing, stack_asleep;
};
PhysicsRun physics_run(double offset, int rebase_every = 0) {
    auto scene = prototype::PhysicsScene{};
    auto& bodies = scene.system.GetBodyInterface();
    auto jobs = JPH::JobSystemThreadPool(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, 4);
    auto origin = JPH::RVec3(offset, 0, offset);
    const auto ground = bodies.CreateAndAddBody(JPH::BodyCreationSettings(new JPH::BoxShape(JPH::Vec3(50, 1, 50)), origin + JPH::RVec3(0, -1, 0),
        JPH::Quat::sIdentity(), JPH::EMotionType::Static, prototype::static_layer), JPH::EActivation::DontActivate);
    const auto box = JPH::RefConst<JPH::Shape>(new JPH::BoxShape(JPH::Vec3::sReplicate(0.25f)));
    auto stack = std::vector<JPH::BodyID>{};
    for (int i = 0; i < 10; ++i)
        stack.push_back(bodies.CreateAndAddBody(JPH::BodyCreationSettings(box, origin + JPH::RVec3(0, 0.25 + 0.5 * i, 0), JPH::Quat::sIdentity(),
            JPH::EMotionType::Dynamic, prototype::moving_layer), JPH::EActivation::Activate));
    // A sphere rolling at 5 m/s along x, 10 m beside the stack.
    auto sphere_settings = JPH::BodyCreationSettings(new JPH::SphereShape(0.3f), origin + JPH::RVec3(-20, 0.3, 10), JPH::Quat::sIdentity(),
        JPH::EMotionType::Dynamic, prototype::moving_layer);
    sphere_settings.mLinearVelocity = JPH::Vec3(5, 0, 0);
    sphere_settings.mAngularVelocity = JPH::Vec3(0, 0, -5 / 0.3f);
    const auto sphere = bodies.CreateAndAddBody(sphere_settings, JPH::EActivation::Activate);
    auto all = std::vector<JPH::BodyID>(stack);
    all.push_back(ground);
    all.push_back(sphere);

    auto settled = std::vector<JPH::RVec3>{};
    auto jitter = 0.0;
    for (int tick = 1; tick <= 600; ++tick) {
        scene.system.Update(1.0f / 60.0f, 1, &scene.temp, &jobs);
        if (tick == 480) for (const auto id : stack) settled.push_back(bodies.GetPosition(id) - origin);
        if (tick > 480)
            for (size_t i = 0; i < stack.size(); ++i)
                jitter = std::max(jitter, double(JPH::Vec3(bodies.GetPosition(stack[i]) - origin - settled[i]).Length()) * 1000.0);
        if (rebase_every && tick % rebase_every == 0) {
            // Move the whole world back and forth by the offset, as origin rebasing would.
            const auto shift = (tick / rebase_every) % 2 ? JPH::RVec3(-offset, 0, -offset) : JPH::RVec3(offset, 0, offset);
            for (const auto id : all) bodies.SetPosition(id, bodies.GetPosition(id) + shift, JPH::EActivation::DontActivate);
            origin += shift;
        }
    }
    auto result = PhysicsRun{};
    for (const auto id : stack) result.stack.push_back(JPH::Vec3(bodies.GetPosition(id) - origin));
    result.sphere = JPH::Vec3(bodies.GetPosition(sphere) - origin);
    result.stack_standing = result.stack.back().GetY() > 4.5f;
    result.rest_jitter_mm = jitter;
    result.stack_asleep = std::none_of(stack.begin(), stack.end(), [&](JPH::BodyID id) { return bodies.IsActive(id); });
    // A ray from 2 m above the ground straight down, 7 m from the stack.
    const auto ray = JPH::RRayCast(origin + JPH::RVec3(7.123, 2, 3.456), JPH::Vec3(0, -4, 0));
    auto hit = JPH::RayCastResult{};
    result.ray_mm = scene.system.GetNarrowPhaseQuery().CastRay(ray, hit)
        ? std::abs(double((ray.GetPointOnRay(hit.mFraction) - origin).GetY())) * 1000.0 : 1e9;
    return result;
}
struct PhysicsError {
    double stack_mm, sphere_mm;
};
PhysicsError compare(const PhysicsRun& run, const PhysicsRun& reference) {
    auto error = PhysicsError{0, double((run.sphere - reference.sphere).Length()) * 1000.0};
    for (size_t i = 0; i < run.stack.size(); ++i) error.stack_mm = std::max(error.stack_mm, double((run.stack[i] - reference.stack[i]).Length()) * 1000.0);
    return error;
}

double rebase_ms(int count) {
    auto scene = prototype::PhysicsScene{};
    auto& bodies = scene.system.GetBodyInterface();
    const auto box = JPH::RefConst<JPH::Shape>(new JPH::BoxShape(JPH::Vec3::sReplicate(0.25f)));
    auto ids = std::vector<JPH::BodyID>{};
    for (int i = 0; i < count; ++i)
        ids.push_back(bodies.CreateBody(JPH::BodyCreationSettings(box, JPH::RVec3(float(i % 200) * 2, 0.25, float(i / 200) * 2),
            JPH::Quat::sIdentity(), i % 4 ? JPH::EMotionType::Static : JPH::EMotionType::Dynamic,
            i % 4 ? prototype::static_layer : prototype::moving_layer))->GetID());
    const auto state = bodies.AddBodiesPrepare(ids.data(), int(ids.size()));
    bodies.AddBodiesFinalize(ids.data(), int(ids.size()), state, JPH::EActivation::DontActivate);
    scene.system.OptimizeBroadPhase();
    const auto start = Clock::now();
    for (const auto id : ids) bodies.SetPosition(id, bodies.GetPosition(id) + JPH::RVec3(-1024, 0, -1024), JPH::EActivation::DontActivate);
    scene.system.OptimizeBroadPhase();
    return ms_since(start);
}

double pile_step_ms() {
    auto scene = prototype::PhysicsScene{};
    auto& bodies = scene.system.GetBodyInterface();
    auto jobs = JPH::JobSystemThreadPool(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, 13);
    bodies.CreateAndAddBody(JPH::BodyCreationSettings(new JPH::BoxShape(JPH::Vec3(60, 1, 60)), JPH::RVec3(0, -1, 0), JPH::Quat::sIdentity(),
        JPH::EMotionType::Static, prototype::static_layer), JPH::EActivation::DontActivate);
    const auto box = JPH::RefConst<JPH::Shape>(new JPH::BoxShape(JPH::Vec3::sReplicate(0.25f)));
    auto ids = std::vector<JPH::BodyID>{};
    for (int i = 0; i < 5000; ++i)
        ids.push_back(bodies.CreateBody(JPH::BodyCreationSettings(box, JPH::RVec3(float(i % 25) * 0.6f - 7.5f, 0.5f + float(i / 500) * 0.6f,
            float((i / 25) % 20) * 0.6f - 6.0f), JPH::Quat::sIdentity(), JPH::EMotionType::Dynamic, prototype::moving_layer))->GetID());
    const auto state = bodies.AddBodiesPrepare(ids.data(), int(ids.size()));
    bodies.AddBodiesFinalize(ids.data(), int(ids.size()), state, JPH::EActivation::Activate);
    scene.system.OptimizeBroadPhase();
    auto total = 0.0;
    for (int step = 0; step < 300; ++step) {
        const auto start = Clock::now();
        scene.system.Update(1.0f / 60.0f, 1, &scene.temp, &jobs);
        if (step >= 60) total += ms_since(start);
    }
    return total / 240.0;
}
} // namespace

int main() {
    std::printf("Coordinate precision prototype (#1060): Jolt in %s precision, sizeof(JPH::Body) %zu bytes\n\n", precision_name,
        sizeof(JPH::Body));
    auto jolt = prototype::JoltRuntime{};
    const auto reference = physics_run(0);
    std::printf("render: the worst pixel error of vertices on an object 5 m (and 0.5 m) from the camera, 1920x1080\n");
    std::printf("%9s %12s %17s %17s %8s | %8s %9s %8s %7s %6s\n", "offset m", "float gap mm", "world px 5m/0.5m", "rel px 5m/0.5m",
        "pick mm", "rest mm", "asleep", "roll mm", "ray mm", "stands");
    for (const auto offset : offsets) {
        const auto gap = double(std::nextafter(float(offset), 1e30f) - float(offset)) * 1000.0;
        const auto render = render_error(offset, 5.0), near = render_error(offset, 0.5);
        const auto run = physics_run(offset);
        const auto error = compare(run, reference);
        std::printf("%9.0f %12.4f %8.3f/%8.3f %8.4f/%8.4f %8.3f | %8.3f %9s %8.3f %7.3f %6s\n", offset, offset ? gap : 0.0,
            render.world_space, near.world_space, render.camera_relative, near.camera_relative, pick_error_mm(offset), run.rest_jitter_mm,
            run.stack_asleep ? "yes" : "no", error.sphere_mm, run.ray_mm, run.stack_standing ? "yes" : "NO");
        // Camera-relative rendering must stay sub-pixel everywhere; that is what makes float storage viable.
        if (std::max(render.camera_relative, near.camera_relative) > 0.05) unexpected("camera-relative rendering is off by " + std::to_string(render.camera_relative) + " px at " + std::to_string(offset) + " m");
        if (!run.stack_standing) unexpected("the stack fell at " + std::to_string(offset) + " m in " + precision_name + " precision");
        if (double_precision && (error.sphere_mm > 0.1 || run.rest_jitter_mm > 0.1 || !run.stack_asleep))
            unexpected("double-precision Jolt at " + std::to_string(offset) + " m: sphere " + std::to_string(error.sphere_mm) + " mm off, rest jitter "
                + std::to_string(run.rest_jitter_mm) + " mm, asleep " + (run.stack_asleep ? "yes" : "no"));
    }
    if (!double_precision) {
        std::printf("\nRebasing (single precision): moving every body by 1,448 m and rebuilding the broad phase\n");
        for (const auto count : {5000, 50000}) std::printf("  %6d bodies: %.2f ms\n", count, rebase_ms(count));
        const auto rebased = physics_run(4000, 120);
        const auto error = compare(rebased, reference);
        std::printf("  the scene rebased by 4 km and back every 120 ticks: sphere %.3f mm from the origin run, rest jitter %.3f mm, asleep %s, stands %s\n",
            error.sphere_mm, rebased.rest_jitter_mm, rebased.stack_asleep ? "yes" : "no", rebased.stack_standing ? "yes" : "NO");
        if (!rebased.stack_standing) unexpected("the stack fell when rebased");
    }
    pile_step_ms();
    std::printf("\nStep cost, 5,000 boxes falling into a pile (%s precision): %.2f ms a step\n", precision_name, pile_step_ms());
    if (failures) std::printf("\n%d unexpected results\n", failures);
    return failures ? 1 : 0;
}
