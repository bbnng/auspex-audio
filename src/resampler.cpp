#include "resampler.h"

#include <cmath>
#include <emmintrin.h>

namespace sa {
namespace {

constexpr double kPi = 3.14159265358979323846;

double bessel_i0(double x) {
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 32; ++k) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
    }
    return sum;
}

} // namespace

void Resampler::init(int max_frames) {
    table_.assign(static_cast<std::size_t>(kPhases + 1) * kTaps, 0.0f);
    coeffs_.assign(static_cast<std::size_t>(max_frames) * kTaps, 0.0f);
    index_.assign(static_cast<std::size_t>(max_frames), 0);
    constexpr double kBeta = 7.5;
    constexpr double kCutoff = 0.94; // of the input's Nyquist frequency
    const double norm = bessel_i0(kBeta);
    for (int p = 0; p <= kPhases; ++p) {
        double sum = 0.0;
        double row[kTaps];
        for (int t = 0; t < kTaps; ++t) {
            const double x = static_cast<double>(t - kHistory) - static_cast<double>(p) / kPhases;
            const double w = x / (kTaps / 2);
            const double window = std::fabs(w) >= 1.0 ? 0.0 : bessel_i0(kBeta * std::sqrt(1.0 - w * w)) / norm;
            const double sinc = std::fabs(x) < 1e-9 ? 1.0 : std::sin(kPi * x * kCutoff) / (kPi * x * kCutoff);
            row[t] = sinc * window;
            sum += row[t];
        }
        for (int t = 0; t < kTaps; ++t) {
            table_[static_cast<std::size_t>(p) * kTaps + static_cast<std::size_t>(t)] = static_cast<float>(row[t] / sum);
        }
    }
}

void Resampler::prepare(double frac, double step, int frames) {
    frames_ = frames;
    for (int i = 0; i < frames; ++i) {
        const double x = frac + step * i;
        const double k = std::floor(x);
        const double phase = (x - k) * kPhases;
        const int p = phase >= kPhases ? kPhases - 1 : static_cast<int>(phase);
        const float f = static_cast<float>(phase - p);
        const float* a = table_.data() + static_cast<std::size_t>(p) * kTaps;
        const float* b = a + kTaps;
        float* c = coeffs_.data() + static_cast<std::size_t>(i) * kTaps;
        for (int t = 0; t < kTaps; ++t) c[t] = a[t] + (b[t] - a[t]) * f;
        index_[static_cast<std::size_t>(i)] = static_cast<int>(k);
    }
}

void Resampler::process(const float* in, float* out) const {
    for (int i = 0; i < frames_; ++i) {
        const float* s = in + index_[static_cast<std::size_t>(i)];
        const float* c = coeffs_.data() + static_cast<std::size_t>(i) * kTaps;
        __m128 acc = _mm_mul_ps(_mm_loadu_ps(s), _mm_loadu_ps(c));
        for (int t = 4; t < kTaps; t += 4) acc = _mm_add_ps(acc, _mm_mul_ps(_mm_loadu_ps(s + t), _mm_loadu_ps(c + t)));
        acc = _mm_add_ps(acc, _mm_movehl_ps(acc, acc));
        acc = _mm_add_ss(acc, _mm_shuffle_ps(acc, acc, 1));
        out[i] = _mm_cvtss_f32(acc);
    }
}

} // namespace sa
