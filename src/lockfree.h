#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <type_traits>

namespace sa {

// Single-writer sequence lock. The writer (game main thread or the raytracing thread) never waits,
// and the reader (the WASAPI callback) never blocks: if it keeps colliding with a write it gives up
// after a bounded number of tries and the caller keeps its previous value.
template <class T>
class SeqLock {
    static_assert(std::is_trivially_copyable<T>::value, "SeqLock payload must be trivially copyable");
    static_assert(sizeof(T) % sizeof(uint32_t) == 0, "SeqLock payload must be a multiple of 4 bytes");
    static constexpr std::size_t kWords = sizeof(T) / sizeof(uint32_t);

public:
    void store(const T& value) {
        uint32_t raw[kWords];
        std::memcpy(raw, &value, sizeof(T));
        const uint32_t s = seq_.load(std::memory_order_relaxed);
        seq_.store(s + 1u, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        for (std::size_t i = 0; i < kWords; ++i) words_[i].store(raw[i], std::memory_order_relaxed);
        seq_.store(s + 2u, std::memory_order_release);
    }

    // Returns false (leaving `out` untouched) if nothing was stored yet or a write kept racing us.
    bool load(T& out) const {
        for (int attempt = 0; attempt < 64; ++attempt) {
            const uint32_t s0 = seq_.load(std::memory_order_acquire);
            if (s0 == 0u) return false;
            if (s0 & 1u) continue;
            uint32_t raw[kWords];
            for (std::size_t i = 0; i < kWords; ++i) raw[i] = words_[i].load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (seq_.load(std::memory_order_relaxed) == s0) {
                std::memcpy(&out, raw, sizeof(T));
                return true;
            }
        }
        return false;
    }

private:
    std::atomic<uint32_t> seq_{0};
    std::atomic<uint32_t> words_[kWords] = {};
};

// Signed distance a - b on the wrapping 32-bit frame timeline.
inline int32_t frame_diff(uint32_t a, uint32_t b) { return static_cast<int32_t>(a - b); }

// One channel of dry (unspatialized) PCM addressed by absolute timeline frame. The producer writes only
// frames at or after the committed write position, the consumer reads only committed frames, so the
// sample memory itself needs no locking; the positions that fence it live in SteamAudioEngine::Voice.
class DryRing {
public:
    static constexpr uint32_t kCapacity = 32768; // 743 ms at 44.1 kHz: room for any DirectSound mix-ahead (max 371 ms)
    static constexpr uint32_t kMask = kCapacity - 1u;

    // Producer thread, before the ring is first published. Returns false when out of memory.
    bool ensure_allocated() {
        if (!data_) data_.reset(new (std::nothrow) float[kCapacity]());
        return data_ != nullptr;
    }

    void write(uint32_t pos, const float* src, uint32_t n) {
        const uint32_t i = pos & kMask;
        const uint32_t first = (n <= kCapacity - i) ? n : (kCapacity - i);
        std::memcpy(data_.get() + i, src, first * sizeof(float));
        if (n > first) std::memcpy(data_.get(), src + first, (n - first) * sizeof(float));
    }

    void zero(uint32_t pos, uint32_t n) {
        if (n > kCapacity) { pos += n - kCapacity; n = kCapacity; }
        const uint32_t i = pos & kMask;
        const uint32_t first = (n <= kCapacity - i) ? n : (kCapacity - i);
        std::memset(data_.get() + i, 0, first * sizeof(float));
        if (n > first) std::memset(data_.get(), 0, (n - first) * sizeof(float));
    }

    void read(uint32_t pos, float* dst, uint32_t n) const {
        const uint32_t i = pos & kMask;
        const uint32_t first = (n <= kCapacity - i) ? n : (kCapacity - i);
        std::memcpy(dst, data_.get() + i, first * sizeof(float));
        if (n > first) std::memcpy(dst + first, data_.get(), (n - first) * sizeof(float));
    }

    // Ramps frames [pos, pos + n) as samples from..total of a fade-in that started `done` frames earlier.
    void apply_fade_in(uint32_t pos, uint32_t n, uint32_t done, uint32_t total) {
        for (uint32_t k = 0; k < n && done + k < total; ++k) {
            data_[(pos + k) & kMask] *= static_cast<float>(done + k + 1u) / static_cast<float>(total);
        }
    }

private:
    std::unique_ptr<float[]> data_;
};

} // namespace sa
