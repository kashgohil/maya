#include "maya/metrics/metrics.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>

namespace maya {

double percentile(std::span<const double> sorted, double p) noexcept {
    if (sorted.empty()) return 0.0;
    const auto rank = static_cast<size_t>(std::ceil(std::clamp(p, 0.0, 1.0) * double(sorted.size())));
    return sorted[std::clamp<size_t>(rank, 1, sorted.size()) - 1];
}

Summary summarize(std::span<const double> samples) {
    auto result = Summary{};
    if (samples.empty()) return result;
    auto sorted = std::vector<double>(samples.begin(), samples.end());
    std::ranges::sort(sorted);
    result.count = sorted.size();
    result.mean = std::accumulate(sorted.begin(), sorted.end(), 0.0) / double(sorted.size());
    result.min = sorted.front();
    result.p50 = percentile(sorted, 0.50);
    result.p95 = percentile(sorted, 0.95);
    result.p99 = percentile(sorted, 0.99);
    result.max = sorted.back();
    return result;
}

SampleWindow::SampleWindow(size_t capacity) : m_samples(capacity) {
    if (capacity == 0) throw std::invalid_argument("A sample window needs room for at least one sample");
}

void SampleWindow::add(double sample) {
    m_samples[m_next] = sample;
    m_next = (m_next + 1) % m_samples.size();
    m_filled = std::min(m_filled + 1, m_samples.size());
}

void SampleWindow::clear() noexcept {
    m_next = 0;
    m_filled = 0;
}

Summary SampleWindow::summary() const {
    // Order does not matter to a summary, so the filled part is used as it lies.
    return summarize(std::span(m_samples.data(), m_filled));
}

} // namespace maya
