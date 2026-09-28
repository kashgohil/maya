#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace maya {

// Timing aggregation shared by live displays and benchmarks (docs/performance.md). CPU-only.

/// count, mean, P50/P95/P99, and extremes of a set of samples. Percentiles use the nearest-rank
/// definition on sorted samples: the ceil(p × N)-th smallest, one-based. Empty sets have count 0 and
/// zeros elsewhere.
struct Summary {
    size_t count = 0;
    double mean = 0.0;
    double min = 0.0;
    double p50 = 0.0;
    double p95 = 0.0;
    double p99 = 0.0;
    double max = 0.0;
};
Summary summarize(std::span<const double> samples);
/// Nearest-rank percentile of samples already sorted ascending; p in (0, 1]. Empty gives 0.
double percentile(std::span<const double> sorted, double p) noexcept;

/// The most recent samples, up to a fixed capacity, for live displays. Adding is O(1); summarizing
/// sorts a copy, so call it a few times a second, not per sample.
class SampleWindow {
public:
    explicit SampleWindow(size_t capacity = 240);
    void add(double sample);
    void clear() noexcept;
    size_t size() const noexcept { return m_filled; }
    size_t capacity() const noexcept { return m_samples.size(); }
    Summary summary() const;

private:
    std::vector<double> m_samples;
    size_t m_next = 0;
    size_t m_filled = 0;
};

/// A monotonic clock reading, in milliseconds since it started.
class Stopwatch {
public:
    using Clock = std::chrono::steady_clock;
    Stopwatch() noexcept : m_start(Clock::now()) {}
    void restart() noexcept { m_start = Clock::now(); }
    double milliseconds() const noexcept {
        return std::chrono::duration<double, std::milli>(Clock::now() - m_start).count();
    }

private:
    Clock::time_point m_start;
};

/// Where one host frame's CPU time went, in milliseconds. `update`, `wait`, `render`, and `submit`
/// are exclusive, in that order, and together make up `tick` (Engine::tick). `interval` is the wall
/// time since the previous frame started, which includes waiting for the display.
struct FrameTiming {
    uint64_t frame = 0; // the device's submission serial for this frame; 0 if nothing was submitted
    double interval = 0.0;
    double tick = 0.0;
    double update = 0.0; // Application::on_update: input, simulation, UI building
    double wait = 0.0; // GraphicsDevice::begin_frame: waiting for a frame slot
    double render = 0.0; // Application::on_render: extraction and encoding
    double submit = 0.0; // GraphicsDevice::end_frame: presenting and committing
};

} // namespace maya
