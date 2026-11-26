#include "dynamics.h"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace sa {
namespace {

constexpr float kFloorMs = 1e-12f;

// ---- VoiceLeveler: constants fitted to all 6200 voice recordings of the game ----
constexpr float kGateDb = 12.0f;
constexpr float kNoVoiceDb = -44.0f, kNoVoiceFade = 8.0f; // nobody's voice was recorded below: noise, nothing to raise
constexpr float kOutburstCut = 0.6f;   // of what is above the outburst level
constexpr float kOutburstMore = 4.0f;  // the most it takes off in addition
constexpr int   kTrailSteps = 8;
constexpr float kRaiseSeconds = 0.3f;   // a quiet first word is no quiet voice yet
constexpr float kPeakCeiling = 0.80f;  // a raised voice never gets a higher peak
constexpr float kDownDbPerS = 60.0f, kOffDbPerS = 60.0f;
constexpr float kUnknownSeconds = 0.10f, kLoudTrust = 0.3f, kKnownFile = 8.0f;
constexpr float kKnownSpeakerSeconds = 0.4f, kSpeakerMemorySeconds = 20.0f;

float ease(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

float db_to_gain(float db) { return std::pow(10.0f, db / 20.0f); }

} // namespace

float compressor_gain_db(float level_db, float threshold_db, float ratio, float knee_db, float max_cut_db) {
    if (ratio <= 1.0f || max_cut_db <= 0.0f) return 0.0f;
    const float slope = 1.0f - 1.0f / ratio;
    const float over = level_db - threshold_db;
    float cut = 0.0f;
    if (over > 0.5f * knee_db) {
        cut = slope * over;
    } else if (over > -0.5f * knee_db && knee_db > 0.0f) {
        const float t = over + 0.5f * knee_db;
        cut = slope * t * t / (2.0f * knee_db);
    }
    return -std::min(cut, max_cut_db);
}

float LevelFollower::update(float block_mean_square, float block_seconds, float attack_seconds, float release_seconds) {
    if (!(block_mean_square < 1e6f)) block_mean_square = 0.0f; // no sound at all (NaN, infinite)
    const float seconds = block_mean_square > ms_ ? attack_seconds : release_seconds;
    ms_ += (block_mean_square - ms_) * (1.0f - std::exp(-block_seconds / seconds));
    if (ms_ < kFloorMs) ms_ = 0.0f;
    return ms_;
}

float LevelFollower::db() const { return 10.0f * std::log10(std::max(ms_, kFloorMs)); }

void LoudnessLeveler::reset() {
    full_.reset();
    bass_.reset();
    gain_ = bass_gain_ = 1.0f;
    lp_[0] = lp_[1] = 0.0f;
    started_ = false;
}

float LoudnessLeveler::cut_db() const { return 20.0f * std::log10(std::max(1e-3f, gain_ * bass_gain_)); }

void LoudnessLeveler::process(float* left, float* right, int frames, float rate, const LevelerSettings& s, float scale) {
    if (!left || frames <= 0 || rate <= 0.0f) return;
    const bool stereo = right && right != left;
    const float block_seconds = static_cast<float>(frames) / rate;
    const bool with_bass = s.bass_hz > 0.0f;
    const float lp_coeff = with_bass ? 1.0f - std::exp(-6.2831853f * s.bass_hz / rate) : 0.0f;

    // Levels first, the gains they lead to are reached at the end of this block.
    double sum = 0.0, bass_sum = 0.0;
    float* side[2] = { left, stereo ? right : nullptr };
    float z[2] = { lp_[0], lp_[1] };
    for (int c = 0; c < 2; ++c) {
        if (!side[c]) continue;
        for (int i = 0; i < frames; ++i) {
            const float x = side[c][i];
            sum += static_cast<double>(x) * x;
            if (with_bass) {
                z[c] += (x - z[c]) * lp_coeff;
                bass_sum += static_cast<double>(z[c]) * z[c];
            }
        }
    }
    const double n = static_cast<double>(frames) * (stereo ? 2.0 : 1.0);
    const double power = static_cast<double>(scale) * scale;
    // A sound that begins is measured for what it is, not for the silence before it.
    const float attack = started_ ? s.attack_seconds : std::min(s.attack_seconds, block_seconds);
    full_.update(static_cast<float>(sum / n * power), block_seconds, attack, s.release_seconds);
    const float target = db_to_gain(compressor_gain_db(full_.db(), s.threshold_db, s.ratio, s.knee_db, s.max_cut_db));
    float bass_target = 1.0f;
    if (with_bass) {
        bass_.update(static_cast<float>(bass_sum / n * power), block_seconds, attack, s.release_seconds);
        bass_target = db_to_gain(compressor_gain_db(bass_.db(), s.bass_threshold_db, s.bass_ratio, s.knee_db, s.bass_max_cut_db));
    }
    const float g0 = started_ ? gain_ : target, b0 = started_ ? bass_gain_ : bass_target;
    started_ = true;
    gain_ = target;
    bass_gain_ = bass_target;

    const bool idle = g0 > 0.9999f && target > 0.9999f && b0 > 0.9999f && bass_target > 0.9999f;
    const float inv = 1.0f / static_cast<float>(frames);
    for (int c = 0; c < 2 && !idle; ++c) {
        if (!side[c]) continue;
        float low = lp_[c];
        for (int i = 0; i < frames; ++i) {
            const float t = static_cast<float>(i + 1) * inv;
            float x = side[c][i];
            if (with_bass) {
                low += (x - low) * lp_coeff;
                x += (b0 + (bass_target - b0) * t - 1.0f) * low;
            }
            side[c][i] = x * (g0 + (target - g0) * t);
        }
    }
    for (int c = 0; c < 2; ++c) lp_[c] = std::fabs(z[c]) < 1e6f && std::fabs(z[c]) > 1e-12f ? z[c] : 0.0f;
}

// ---- Voices at one loudness ----

void SpeechMeter::reset() {
    const float rate = rate_;
    *this = SpeechMeter();
    if (rate > 0.0f) set_rate(rate);
}

// The K weighting of ITU-R BS.1770, for any sample rate.
void SpeechMeter::set_rate(float rate) {
    rate_ = rate;
    step_frames_ = std::max(1, static_cast<int>(rate * 0.010f + 0.5f));
    constexpr double kPi = 3.14159265358979323846;
    {
        const double q = 0.7071752369554196, k = std::tan(kPi * 1681.974450955533 / rate);
        const double vh = std::pow(10.0, 3.999843853973347 / 20.0), vb = std::pow(vh, 0.4996667741545416);
        const double a0 = 1.0 + k / q + k * k;
        b_[0][0] = static_cast<float>((vh + vb * k / q + k * k) / a0);
        b_[0][1] = static_cast<float>(2.0 * (k * k - vh) / a0);
        b_[0][2] = static_cast<float>((vh - vb * k / q + k * k) / a0);
        a_[0][0] = static_cast<float>(2.0 * (k * k - 1.0) / a0);
        a_[0][1] = static_cast<float>((1.0 - k / q + k * k) / a0);
    }
    {
        // 80 Hz instead of 38 Hz: rumble under a recording is not how loud somebody speaks, and a float has room for it.
        const double q = 0.7071067811865476, k = std::tan(kPi * 80.0 / rate);
        const double a0 = 1.0 + k / q + k * k;
        b_[1][0] = static_cast<float>(1.0 / a0);
        b_[1][1] = static_cast<float>(-2.0 / a0);
        b_[1][2] = static_cast<float>(1.0 / a0);
        a_[1][0] = static_cast<float>(2.0 * (k * k - 1.0) / a0);
        a_[1][1] = static_cast<float>((1.0 - k / q + k * k) / a0);
    }
}

int SpeechMeter::frames_to_step(float rate) {
    if (rate != rate_ && rate > 0.0f) set_rate(rate);
    return std::max(1, step_frames_ - count_);
}

void SpeechMeter::add(const float* left, const float* right, int frames, float rate) {
    if (!left || frames <= 0 || !(rate > 0.0f)) return;
    if (rate != rate_) set_rate(rate);
    const int sides = right && right != left ? 2 : 1;
    const float* in[2] = { left, right };
    for (int i = 0; i < frames; ++i) {
        for (int c = 0; c < sides; ++c) {
            float x = in[c][i];
            if (!(std::fabs(x) < 64.0f)) x = 0.0f; // no sound at all (NaN, infinite)
            sum_peak_ = std::max(sum_peak_, std::fabs(x));
            for (int s = 0; s < 2; ++s) {
                const float y = b_[s][0] * x + z_[c][s][0];
                z_[c][s][0] = b_[s][1] * x - a_[s][0] * y + z_[c][s][1];
                z_[c][s][1] = b_[s][2] * x - a_[s][1] * y;
                x = y;
            }
            sum_ += static_cast<double>(x) * x / sides;
        }
        if (++count_ >= step_frames_) step();
    }
    for (auto& side : z_) {
        for (auto& biquad : side) {
            for (float& z : biquad) {
                if (!(std::fabs(z) > 1e-15f && std::fabs(z) < 1e6f)) z = 0.0f;
            }
        }
    }
}

// The power in the middle of each bin, made while the DLL loads.
const SpeechMeter::Powers SpeechMeter::kPower;

SpeechMeter::Powers::Powers() {
    for (int i = 0; i < kBins; ++i) p[i] = std::pow(10.0, (kFloorDb + 0.5 * i + 0.25) / 10.0);
}

void SpeechMeter::step() {
    ring_[steps_ % kWindow] = static_cast<float>(sum_ / count_);
    peak_ = sum_peak_;
    sum_ = 0.0;
    sum_peak_ = 0.0f;
    count_ = 0;
    ++steps_;
    float window = 0.0f;
    for (float p : ring_) window += p;
    window_db_ = 10.0f * std::log10(std::max(window / kWindow, kFloorMs));
    speech_ = false;
    const int bin = static_cast<int>(std::floor((window_db_ - kFloorDb) * 2.0f));
    if (bin < 0) return;
    ++bins_[std::min(bin, kBins - 1)];

    // The mean of everything, then of what is not far below it.
    const auto mean_from = [&](int first, unsigned& n) {
        double sum = 0.0;
        n = 0;
        for (int i = std::max(0, first); i < kBins; ++i) {
            sum += kPower.p[i] * bins_[i];
            n += bins_[i];
        }
        return n > 0 ? static_cast<float>(10.0 * std::log10(sum / n)) : -120.0f;
    };
    unsigned n = 0;
    const float all = mean_from(0, n);
    const float gate = all - kGateDb;
    db_ = mean_from(static_cast<int>(std::ceil((gate - kFloorDb) * 2.0f)), n);
    seconds_ = 0.010f * static_cast<float>(n);
    speech_ = window_db_ >= gate;
}

void VoiceLeveler::reset(float known_db, float known_seconds) {
    meter_.reset();
    known_db_ = known_db;
    known_seconds_ = known_seconds;
    std::fill(std::begin(window_), std::end(window_), -120.0f);
    std::fill(std::begin(peak_), std::end(peak_), 0.0f);
    std::fill(std::begin(speech_), std::end(speech_), false);
    gain_db_ = 0.0f;
    heard_ = started_ = false;
}

void VoiceLeveler::listen(const float* left, const float* right, int frames, float rate) {
    const bool stereo = right && right != left;
    while (frames > 0) {
        const int before = meter_.steps();
        const int n = std::min(frames, meter_.frames_to_step(rate));
        meter_.add(left, right, n, rate);
        if (meter_.steps() != before) {
            const int i = before % kHistory;
            window_[i] = meter_.window_db();
            peak_[i] = meter_.peak();
            speech_[i] = meter_.speech();
        }
        left += n;
        if (stereo) right += n;
        frames -= n;
    }
}

float VoiceLeveler::wanted_db(const VoiceLevelSettings& s, int ahead_steps) const {
    // Loud is loud, whatever was expected; quiet may only be the beginning.
    const float heard = meter_.seconds(), known = known_seconds_ * (meter_.db() > known_db_ ? kLoudTrust : 1.0f);
    const float trust = known + heard;
    if (trust <= 0.0f) return 0.0f;
    const float level = (known * known_db_ + heard * meter_.db()) / trust;
    const float voice = heard > 0.0f ? meter_.db() : known_db_;
    float gain = s.target_db - level;
    if (gain < 0.0f) gain = std::max(-s.max_cut_db, gain * s.cut);
    else gain = std::min(s.max_boost_db, gain * s.boost * ease(gain / std::max(0.1f, s.tolerance_db))) * std::clamp((voice - kNoVoiceDb) / kNoVoiceFade, 0.0f, 1.0f) *
                (trust / (trust + kRaiseSeconds));

    // What is about to be heard, and was heard just now.
    float loudest = -120.0f, peak = 1e-6f;
    for (int i = 0; i < std::min({ ahead_steps + kTrailSteps, meter_.steps(), kHistory }); ++i) {
        const int k = (meter_.steps() - 1 - i) % kHistory;
        loudest = std::max(loudest, window_[k]);
        peak = std::max(peak, peak_[k]);
    }
    const float over = loudest + gain - (s.target_db + s.outburst_db);
    if (over > 0.0f) gain -= over * kOutburstCut;
    if (gain > 0.0f) gain = std::max(0.0f, std::min(gain, 20.0f * std::log10(kPeakCeiling / peak)));
    return std::max(gain, -s.max_cut_db - kOutburstMore);
}

void VoiceLeveler::process(float* left, float* right, int frames, float rate, const VoiceLevelSettings& s, float ahead_seconds,
                           bool on) {
    if (!left || frames <= 0 || rate <= 0.0f) return;
    const float seconds = static_cast<float>(frames) / rate;
    const int ahead = std::clamp(static_cast<int>(ahead_seconds * 100.0f), 0, kHistory - 1 - kTrailSteps);
    const float wanted = on ? wanted_db(s, ahead) : 0.0f;
    const float from = started_ ? gain_db_ : wanted;
    float to = wanted;
    if (heard_ && started_) {
        // Louder only while somebody speaks: pauses, breath and hiss are not raised.
        const bool speaking = meter_.speech() || !on;
        const float up = speaking ? (on ? s.up_db_per_s : kOffDbPerS) * seconds : 0.0f;
        to = std::clamp(wanted, from - kDownDbPerS * seconds, from + up);
    }
    started_ = true;
    gain_db_ = to;
    if (meter_.steps() > ahead) {
        if (speech_[(meter_.steps() - 1 - ahead) % kHistory]) heard_ = true;
    }
    if (from == 0.0f && to == 0.0f) return;
    const float g0 = db_to_gain(from), g1 = db_to_gain(to);
    const float step = (g1 - g0) / static_cast<float>(frames);
    const bool stereo = right && right != left;
    for (int i = 0; i < frames; ++i) {
        const float g = g0 + step * static_cast<float>(i + 1);
        left[i] *= g;
        if (stereo) right[i] *= g;
    }
}

void VoiceMemory::recall(unsigned file, unsigned speaker, float& known_db, float& known_seconds) const {
    known_db = kVoiceTargetDb;
    known_seconds = kUnknownSeconds;
    const Entry& f = files_[file % kFiles];
    const Entry& s = speakers_[speaker % kSpeakers];
    if (file != 0u && f.key == file) {
        known_db = f.db;
        known_seconds = kKnownFile * f.seconds;
    } else if (speaker != 0u && s.key == speaker) {
        known_db = s.db;
        known_seconds = kKnownSpeakerSeconds;
    }
}

void VoiceMemory::remember(unsigned file, unsigned speaker, float db, float seconds) {
    if (!(seconds >= 0.1f) || !(db > -70.0f && db < 20.0f)) return;
    if (file != 0u) {
        Entry& f = files_[file % kFiles];
        // Interrupted this time: what was heard to its end counts.
        if (f.key != file || f.seconds <= seconds * 1.1f) f = Entry{ file, db, seconds };
    }
    if (speaker != 0u) {
        Entry& s = speakers_[speaker % kSpeakers];
        if (s.key != speaker) {
            s = Entry{ speaker, db, seconds };
        } else {
            const float old = std::min(s.seconds, kSpeakerMemorySeconds);
            s.db = (s.db * old + db * seconds) / (old + seconds);
            s.seconds = old + seconds;
        }
    }
}

void VoiceMemory::clear() {
    for (Entry& e : files_) e = Entry{};
    for (Entry& e : speakers_) e = Entry{};
}

void PeakLimiter::process(float* lr, int frames, float rate) {
    if (!lr || frames <= 0 || rate <= 0.0f) return;
    float peak = 0.0f;
    int first_over = frames;
    for (int i = 0; i < frames; ++i) {
        const float a = std::max(std::fabs(lr[i * 2]), std::fabs(lr[i * 2 + 1]));
        if (!(a < 1e6f)) continue; // the clipper turns these into silence
        peak = std::max(peak, a);
        if (a * gain_ > kCeiling && first_over == frames) first_over = i;
    }
    const float start = gain_;
    if (first_over < frames) {
        const float target = std::max(kMinGain, kCeiling / peak);
        const int ramp = std::max(1, first_over);
        for (int i = 0; i < frames; ++i) {
            const float g = i + 1 >= ramp ? target : start + (target - start) * static_cast<float>(i + 1) / static_cast<float>(ramp);
            lr[i * 2] *= g;
            lr[i * 2 + 1] *= g;
        }
        gain_ = target;
        return;
    }
    if (start >= 1.0f) return;
    // Back up, but never over what this block's peak allows.
    const float block_seconds = static_cast<float>(frames) / rate;
    const float allowed = peak > kCeiling ? kCeiling / peak : 1.0f;
    float next = start + (1.0f - start) * (1.0f - std::exp(-block_seconds / kReleaseSeconds));
    if (next > 0.9995f) next = 1.0f;
    next = std::max(start, std::min(next, allowed));
    const float step = (next - start) / static_cast<float>(frames);
    for (int i = 0; i < frames; ++i) {
        const float g = start + step * static_cast<float>(i + 1);
        lr[i * 2] *= g;
        lr[i * 2 + 1] *= g;
    }
    gain_ = next;
}

} // namespace sa
