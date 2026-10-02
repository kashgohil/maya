#pragma once
// A null device that reports GPU frame, pass, and present timings as a test scripts them, so code that
// consumes timings can be checked without a GPU. It says nothing about real GPU behavior.

#include "maya/rhi/null_device.hpp"
#include <map>
#include <string>
#include <vector>

namespace maya::test {

class TimingDevice final : public NullGraphicsDevice {
public:
    explicit TimingDevice(NullDeviceOptions options = {}) : NullGraphicsDevice(options) {}
    ~TimingDevice() override { shutdown(); }

    double frame_ms = 2.0; // every frame's GPU time
    double pass_ms = 0.25; // each timed pass's fragment stage; passes run back to back from the frame's start
    bool overrun = false; // the last pass of each frame ends after the frame: a measurement fault
    bool presents = true; // presented frames report being shown, 1/120 s apart
    std::vector<uint64_t> dropped; // serials reported as never shown

protected:
    bool backend_gpu_timing_supported() const noexcept override { return true; }
    std::string backend_pass_timing_reason() const override { return {}; }
    RhiDiagnostic backend_begin_pass(const RenderPassDesc& desc) override {
        if (frame_pass_timing()) {
            if (frame_pass_index() < max_timed_passes) m_encoding.labels.push_back(desc.label);
            else ++m_encoding.untimed;
        }
        return {};
    }
    RhiDiagnostic backend_begin_frame() override {
        m_encoding = {};
        return {};
    }
    void backend_submit(uint64_t serial, bool present) override {
        m_frames[serial] = std::move(m_encoding);
        m_encoding = {};
        completion()->record_timing(serial, frame_ms, double(serial));
        if (present && presents) {
            const auto never = std::ranges::find(dropped, serial) != dropped.end();
            completion()->record_present(serial, never ? std::nullopt : std::optional(double(serial) / 120.0));
        }
        NullGraphicsDevice::backend_submit(serial, present);
    }
    void backend_attach_pass_timings(GpuFrameTiming& timing) override {
        const auto found = m_frames.find(timing.frame);
        if (found == m_frames.end()) return;
        auto start = 0.0;
        for (const auto& label : found->second.labels) {
            auto pass = GpuPassTiming{label, start, start + 0.01 + pass_ms, 0.01, pass_ms};
            start = pass.end_ms;
            timing.passes.push_back(pass);
        }
        if (overrun && !timing.passes.empty()) timing.passes.back().end_ms = timing.milliseconds + 1.0;
        timing.untimed_passes = found->second.untimed;
        m_frames.erase(found);
    }
    bool backend_present_timing_supported() const noexcept override { return presents; }
    std::optional<double> backend_display_refresh_rate() const noexcept override { return 120.0; }

private:
    struct Frame {
        std::vector<std::string> labels;
        uint32_t untimed = 0;
    };
    Frame m_encoding;
    std::map<uint64_t, Frame> m_frames;
};

} // namespace maya::test
