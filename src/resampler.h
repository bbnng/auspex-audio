#pragma once

#include <cstdint>
#include <vector>

namespace sa {

// Windowed-sinc interpolation from the dry 44.1 kHz timeline to the device's rate. All voices of a block sit at the same
// fractional position, so the block's filter coefficients are computed once and shared. 16 taps look 7 frames back and
// 8 ahead: 0.18 ms, of sound only - direction is applied after it.
class Resampler {
public:
    static constexpr int kTaps = 16;
    static constexpr int kHistory = kTaps / 2 - 1; // frames before the position
    static constexpr int kAhead = kTaps / 2;       // frames after it

    void init(int max_frames);

    // Coefficients for output frames at positions frac, frac + step, ... (in input frames).
    void prepare(double frac, double step, int frames);

    // in[0] is the input frame kHistory before the block's first one.
    void process(const float* in, float* out) const;

private:
    static constexpr int kPhases = 256;
    std::vector<float> table_;  // (kPhases + 1) * kTaps
    std::vector<float> coeffs_; // frames * kTaps
    std::vector<int>   index_;
    int frames_ = 0;
};

} // namespace sa
