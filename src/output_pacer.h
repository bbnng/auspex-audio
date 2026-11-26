#pragma once

#include <algorithm>
#include <cstdint>

namespace sa {

// When to render the next WASAPI period: as late as possible, but never so late that the device runs dry. Belongs to the
// audio thread. The numbers describe one device: another buffer, period or rate means another one, and everything
// learned about the old one (reserve, late wake-ups, queue average) is worthless.
class OutputPacer {
public:
    // Returns true if this is another device (or the first one) and everything starts anew.
    bool configure(uint32_t buffer, uint32_t period, uint32_t rate) {
        if (buffer == buffer_ && period == period_ && rate == rate_) return false;
        buffer_ = buffer;
        period_ = period;
        rate_ = rate;
        // The audio engine takes about 10 ms at a time (or a period, with low-latency periods).
        tick_ = std::max(1u, std::min(period, rate / 90u));
        highest_ = buffer > period ? buffer - period : 0u;
        reserve_ = std::min(highest_, std::max(tick_ - 1u, rate / 250u));
        last_us_ = 0;
        avg_ = 0.0f;
        for (uint32_t& t : late_us_) t = 0;
        return true;
    }
    // The device is gone: whatever comes next starts anew, even one with the same numbers.
    void reset() { buffer_ = period_ = rate_ = 0; }

    // Render once no more than this is queued (frames at the device's rate).
    uint32_t reserve() const { return reserve_; }
    float queued_average() const { return avg_; }

    // The callback is about to render with `padding` frames still queued. Returns true if it came too late.
    bool woke(uint32_t now_us, uint32_t padding) {
        const uint32_t rate = std::max(1u, rate_);
        const uint32_t late_after = static_cast<uint32_t>(1.75e6 * std::max(period_, tick_) / rate) +
                                    static_cast<uint32_t>(1e6 * (reserve_ / tick_ * tick_) / rate);
        bool late = false;
        if (last_us_ != 0 && padding == 0 && now_us - last_us_ > late_after) {
            late = true;
            // Three dropouts within five seconds: this machine needs one more period in reserve.
            late_us_[0] = late_us_[1];
            late_us_[1] = late_us_[2];
            late_us_[2] = now_us;
            if (late_us_[0] != 0 && now_us - late_us_[0] < 5000000u) {
                reserve_ = std::min(highest_, reserve_ + tick_);
                for (uint32_t& t : late_us_) t = 0;
            }
        }
        last_us_ = now_us;
        avg_ += (static_cast<float>(padding) - avg_) * 0.05f;
        return late;
    }

private:
    uint32_t buffer_ = 0, period_ = 0, rate_ = 0; // the device these values belong to
    uint32_t tick_ = 1, highest_ = 0, reserve_ = 0;
    uint32_t last_us_ = 0;
    uint32_t late_us_[3] = {}; // when the last late wake-ups were
    float avg_ = 0.0f;
};

} // namespace sa
