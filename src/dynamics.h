#pragma once

namespace sa {

// Level control on the audio thread, per DSP block (2 to 3 ms) with ramped gains, without delay. Everything but the
// VoiceLeveler only ever turns down and leaves what is below its threshold exactly as it is.

// How many dB (<= 0) a compressor takes off a signal of `level_db`.
float compressor_gain_db(float level_db, float threshold_db, float ratio, float knee_db, float max_cut_db);

// Mean square that rises and falls with two time constants.
class LevelFollower {
public:
    void  reset() { ms_ = 0.0f; }
    float update(float block_mean_square, float block_seconds, float attack_seconds, float release_seconds);
    float db() const;

private:
    float ms_ = 0.0f;
};

struct LevelerSettings {
    float threshold_db;
    float ratio;
    float knee_db;
    float max_cut_db;
    // Below this frequency a second, separate compressor (0: none).
    float bass_hz;
    float bass_threshold_db;
    float bass_ratio;
    float bass_max_cut_db;
    float attack_seconds;
    float release_seconds;
};

// Frenzy: -18 dBFS RMS for ten seconds, a quarter of it below 150 Hz. engine.dll plays it, like all 2D, at half of that per ear.
constexpr LevelerSettings kStingerLeveler{ -22.0f, 3.0f, 6.0f, 6.0f, 150.0f, -28.0f, 3.0f, 6.0f, 0.015f, 0.25f };
// The sum of everything: the bass of combat music and effects together.
constexpr LevelerSettings kBassGuard{ 0.0f, 1.0f, 4.0f, 0.0f, 140.0f, -16.0f, 4.0f, 9.0f, 0.020f, 0.25f };

// Turns down one sound (or the bass of a mix), mono or stereo with the same gain on both sides, in place.
class LoudnessLeveler {
public:
    void reset();
    // `scale`: how loud the block is played afterwards. `right` may be `left` or null for mono.
    void process(float* left, float* right, int frames, float rate, const LevelerSettings& settings, float scale);
    // What it takes off right now, dB (<= 0).
    float cut_db() const;

private:
    LevelFollower full_, bass_;
    float gain_ = 1.0f, bass_gain_ = 1.0f;
    float lp_[2] = {};
    bool  started_ = false;
};

// ---- Voices at one loudness ----
//
// The game's voices were recorded anywhere between -35 and -10 dB (gated loudness, K-weighted as in ITU-R BS.1770):
// "Lovely evening!" -14, "Buy a watch." -26, the prostitute's "Hey, baby." -16 in a conversation, where others speak at
// -20. This measures what is being said and moves the whole line to one loudness. Nothing is known about the file, so it
// works for every voice, also a mod's.

struct VoiceLevelSettings {
    float target_db;        // where all voices go
    float cut;              // the share of what is above it that is taken off
    float max_cut_db;
    float boost;            // the share of what is missing that is added
    float max_boost_db;
    float tolerance_db;     // what is missing less than this is added only in part
    float outburst_db;      // no moment (50 ms) gets further above the target than this
    float up_db_per_s;      // how fast a line gets louder once it is heard
};

// Where the voices of the game are: measured by SpeechMeter, half of its conversations are above -18.6 dB, half of its
// float lines above -19.1. A little below, because what stands out are the loud ones.
constexpr float kVoiceTargetDb = -20.0f;
// What people say by themselves.
constexpr VoiceLevelSettings kBarkVoice{ kVoiceTargetDb, 1.0f, 10.0f, 1.0f, 8.0f, 4.0f, 6.0f, 16.0f };
// Conversations and story scenes: whoever was recorded hot comes down all the same, but a whisper stays softer than
// speech (half of what is missing, 4 dB at most) and an outburst stays an outburst.
constexpr VoiceLevelSettings kDialogueVoice{ kVoiceTargetDb, 0.9f, 10.0f, 0.5f, 4.0f, 4.0f, 8.0f, 8.0f };
// Cries, pain, laughter: no speech, and meant to stand out. Only what is far too hot comes down, half the way.
constexpr VoiceLevelSettings kCryVoice{ kVoiceTargetDb + 2.0f, 0.5f, 6.0f, 0.0f, 0.0f, 4.0f, 12.0f, 16.0f };

// How loud a voice is: the mean of the moments in which there is speech (windows of 50 ms, every 10 ms). Pauses, room
// tone and the hiss at the end of a recording do not count: whatever is 12 dB below the mean of the rest, or below -50 dB.
class SpeechMeter {
public:
    static constexpr float kFloorDb = -50.0f;

    void reset();
    // What comes next, in order. `right` null or `left`: mono.
    void add(const float* left, const float* right, int frames, float rate);

    float seconds() const { return seconds_; }   // of speech so far
    float db() const { return db_; }             // its loudness; meaningless while seconds() is 0
    float window_db() const { return window_db_; } // the last 50 ms
    float peak() const { return peak_; }         // highest sample of the last 10 ms step
    int   steps() const { return steps_; }       // 10 ms steps done, ever
    bool  speech() const { return speech_; }     // the last 50 ms were speech
    // How many more frames the 10 ms step takes that is being collected.
    int   frames_to_step(float rate);

private:
    static constexpr int kBins = 120;  // half a dB each, from kFloorDb up
    static constexpr int kWindow = 5;
    struct Powers {
        double p[kBins];
        Powers();
    };
    static const Powers kPower;
    void step();
    void set_rate(float rate);

    float rate_ = 0.0f;
    int   step_frames_ = 441;
    // Two biquads: a shelf for what the head does, a high-pass.
    float b_[2][3] = {}, a_[2][2] = {};
    float z_[2][2][2] = {}; // side, biquad, state
    double sum_ = 0.0;
    float sum_peak_ = 0.0f;
    int   count_ = 0;
    float ring_[kWindow] = {};
    unsigned bins_[kBins] = {};
    int   steps_ = 0;
    float seconds_ = 0.0f, db_ = 0.0f, window_db_ = -120.0f, peak_ = 0.0f;
    bool  speech_ = false;
};

class VoiceLeveler {
public:
    // Further ahead than this nothing has to be heard before it is played.
    static constexpr float kLookAheadSeconds = 0.15f;

    // A new sound. `known_db`: what it is likely to be as loud as, trusted as much as `known_seconds` of hearing it.
    void  reset(float known_db, float known_seconds);
    // What will be played, in order, up to kLookAheadSeconds early.
    void  listen(const float* left, const float* right, int frames, float rate);
    // The block that is played now, which begins `ahead_seconds` before the last sample that listen() got. In place,
    // `right` null or `left`: mono. Off: back to the recording as it is.
    void  process(float* left, float* right, int frames, float rate, const VoiceLevelSettings& settings, float ahead_seconds,
                  bool on);
    float gain_db() const { return gain_db_; }
    // The line as far as it was heard: its loudness, and the seconds of speech in it (0: none).
    float db() const { return meter_.db(); }
    float seconds() const { return meter_.seconds(); }

private:
    float wanted_db(const VoiceLevelSettings& settings, int ahead_steps) const;

    static constexpr int kHistory = 64; // 10 ms steps
    SpeechMeter meter_;
    float known_db_ = kVoiceTargetDb, known_seconds_ = 0.0f;
    float window_[kHistory] = {}, peak_[kHistory] = {};
    bool  speech_[kHistory] = {};
    float gain_db_ = 0.0f;
    bool  started_ = false;
    bool  heard_ = false; // speech has been played: the gain has to move smoothly from here on
};

// What was heard before, so that a line is right from its first word the second time, and a new line of the same
// speaker (the same folder) starts where the others were. Fixed size, forgets the oldest of those that share a place.
class VoiceMemory {
public:
    // Where to begin. `file`/`speaker`: hashes of the names, 0: unknown.
    void recall(unsigned file, unsigned speaker, float& known_db, float& known_seconds) const;
    void remember(unsigned file, unsigned speaker, float db, float seconds);
    void clear();

private:
    struct Entry { unsigned key; float db; float seconds; };
    static constexpr unsigned kFiles = 512, kSpeakers = 128;
    Entry files_[kFiles] = {};
    Entry speakers_[kSpeakers] = {};
};

// Keeps the output below `ceiling`. The gain of a block is known before its first sample leaves, so a peak is met by a
// ramp that ends where it begins to exceed the ceiling; it comes back over `release_seconds`. A clipper bends the waveform of
// everything that plays at that moment instead, heard as a harsh, fat sound.
class PeakLimiter {
public:
    static constexpr float kCeiling = 0.85f;
    static constexpr float kMinGain = 0.25f; // -12 dB; what is louder still is the clipper's
    static constexpr float kReleaseSeconds = 0.12f;

    void  reset() { gain_ = 1.0f; }
    void  process(float* interleaved_lr, int frames, float rate);
    float gain() const { return gain_; }

private:
    float gain_ = 1.0f;
};

} // namespace sa
