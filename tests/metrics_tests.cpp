#include "maya/metrics/metrics.hpp"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <numeric>
#include <stdexcept>
#include <vector>

using namespace maya;
using Catch::Approx;

TEST_CASE("Summaries use nearest-rank percentiles on known samples", "[metrics]") {
    auto hundred = std::vector<double>(100);
    std::iota(hundred.begin(), hundred.end(), 1.0); // 1..100
    std::ranges::reverse(hundred); // order does not matter
    const auto s = summarize(hundred);
    CHECK(s.count == 100);
    CHECK(s.mean == Approx(50.5));
    CHECK(s.min == 1.0);
    CHECK(s.p50 == 50.0); // ceil(0.50 × 100) = 50th
    CHECK(s.p95 == 95.0);
    CHECK(s.p99 == 99.0);
    CHECK(s.max == 100.0);

    // Ten samples: P95 and P99 are both the largest (ceil(9.5) = ceil(9.9) = 10), P50 the fifth.
    const auto ten = std::vector<double>{7, 1, 9, 3, 5, 2, 8, 4, 10, 6};
    const auto t = summarize(ten);
    CHECK(t.p50 == 5.0);
    CHECK(t.p95 == 10.0);
    CHECK(t.p99 == 10.0);
    // A hitch is never discarded: it is the maximum and drives the tail.
    const auto hitch = std::vector<double>{16.6, 16.7, 16.6, 16.8, 16.7, 16.6, 16.7, 16.6, 16.7, 250.0};
    CHECK(summarize(hitch).p99 == 250.0);
    CHECK(summarize(hitch).mean == Approx(40.0));

    // Edges: one sample, duplicates, empty, and out-of-range p.
    const auto one = summarize(std::vector<double>{3.5});
    CHECK(one.p50 == 3.5);
    CHECK(one.p99 == 3.5);
    CHECK(summarize(std::vector<double>{2, 2, 2, 2}).p95 == 2.0);
    const auto none = summarize(std::vector<double>{});
    CHECK(none.count == 0);
    CHECK(none.mean == 0.0);
    CHECK(none.p99 == 0.0);
    const auto sorted = std::vector<double>{1, 2, 3};
    CHECK(percentile(sorted, 0.0) == 1.0); // the smallest rank is 1
    CHECK(percentile(sorted, 1.0) == 3.0);
    CHECK(percentile(sorted, 2.0) == 3.0);
    CHECK(percentile(std::vector<double>{}, 0.5) == 0.0);
}

TEST_CASE("A sample window keeps only the most recent samples", "[metrics]") {
    auto window = SampleWindow(4);
    CHECK(window.summary().count == 0);
    for (int i = 1; i <= 3; ++i) window.add(i);
    CHECK(window.size() == 3);
    CHECK(window.summary().mean == Approx(2.0));
    for (int i = 4; i <= 6; ++i) window.add(i); // 1 and 2 fall out
    CHECK(window.size() == 4);
    CHECK(window.capacity() == 4);
    const auto s = window.summary();
    CHECK(s.min == 3.0);
    CHECK(s.max == 6.0);
    CHECK(s.mean == Approx(4.5));
    window.clear();
    CHECK(window.summary().count == 0);
    CHECK_THROWS_AS(SampleWindow(0), std::invalid_argument);
}

TEST_CASE("The stopwatch is monotonic and restarts", "[metrics]") {
    auto clock = Stopwatch{};
    const auto first = clock.milliseconds();
    const auto second = clock.milliseconds();
    CHECK(first >= 0.0);
    CHECK(second >= first);
    clock.restart();
    CHECK(clock.milliseconds() <= second + 1000.0);
}
