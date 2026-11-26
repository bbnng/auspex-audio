#pragma once

#include <phonon.h>
#include <cmath>
#include <cstdint>

namespace sa {

// Source engine units are inches (1 inch = 0.0254 meters).
constexpr float kSourceToMeters = 0.0254f;
constexpr float kMetersToSource = 1.0f / kSourceToMeters;

// Right-handed coordinate conversion from Source (Z-up, inches) to Steam Audio (Y-up, meters):
//   Source +X (forward/east) -> Steam Audio -Z (ahead)
//   Source +Y (left/north)   -> Steam Audio -X (-right)
//   Source +Z (up)           -> Steam Audio +Y (up)
// Basis determinant = +1 (pure right-handed rotation, preserves triangle winding).
inline IPLVector3 source_pos_to_ipl(float sx, float sy, float sz) {
    return IPLVector3{
        -sy * kSourceToMeters,
         sz * kSourceToMeters,
        -sx * kSourceToMeters
    };
}

inline IPLVector3 source_pos_to_ipl(const float p[3]) {
    return source_pos_to_ipl(p[0], p[1], p[2]);
}

inline IPLVector3 source_dir_to_ipl(float dx, float dy, float dz) {
    IPLVector3 v{ -dy, dz, -dx };
    const float len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (len > 1e-6f) {
        const float inv = 1.0f / len;
        v.x *= inv;
        v.y *= inv;
        v.z *= inv;
    } else {
        v = { 0.0f, 0.0f, -1.0f };
    }
    return v;
}

inline IPLVector3 source_dir_to_ipl(const float d[3]) {
    return source_dir_to_ipl(d[0], d[1], d[2]);
}

inline IPLVector3 ipl_cross(IPLVector3 a, IPLVector3 b) {
    return IPLVector3{
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x
    };
}

inline float ipl_dot(IPLVector3 a, IPLVector3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

inline IPLVector3 ipl_normalize(IPLVector3 v, IPLVector3 fallback) {
    const float len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (len > 1e-6f) {
        const float inv = 1.0f / len;
        return IPLVector3{ v.x * inv, v.y * inv, v.z * inv };
    }
    return fallback;
}

// Build an orthonormal IPLCoordinateSpace3 from Source listener vectors.
// Note: Steam Audio's IPLCoordinateSpace3 expects:
//   right, up, ahead, origin (where right x up = -ahead, i.e. ahead x up = right).
inline IPLCoordinateSpace3 make_ipl_listener(
    const float origin[3],
    const float forward[3],
    const float right[3],
    const float up[3])
{
    IPLCoordinateSpace3 cs{};
    cs.origin = source_pos_to_ipl(origin);
    cs.ahead  = source_dir_to_ipl(forward);
    IPLVector3 raw_up = source_dir_to_ipl(up);
    IPLVector3 raw_right = source_dir_to_ipl(right);

    // Orthonormalize: right = normalize(ahead x up), up = normalize(right x ahead).
    IPLVector3 ortho_right = ipl_normalize(ipl_cross(cs.ahead, raw_up), raw_right);
    IPLVector3 ortho_up    = ipl_normalize(ipl_cross(ortho_right, cs.ahead), IPLVector3{ 0.0f, 1.0f, 0.0f });
    cs.right = ortho_right;
    cs.up    = ortho_up;
    return cs;
}

// Compute unit direction from listener to source in listener-local coordinates for IPLBinauralEffect,
// or in world space (Steam Audio's iplBinauralEffectApply expects direction relative to listener
// in listener-local space: +x right, +y up, -z ahead).
inline IPLVector3 world_dir_to_listener_local(
    const IPLCoordinateSpace3& listener,
    const IPLVector3& source_pos,
    float* out_distance_meters = nullptr)
{
    IPLVector3 delta{
        source_pos.x - listener.origin.x,
        source_pos.y - listener.origin.y,
        source_pos.z - listener.origin.z
    };
    const float dist = std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
    if (out_distance_meters) *out_distance_meters = dist;
    if (dist < 1e-5f) {
        return IPLVector3{ 0.0f, 0.0f, -1.0f };
    }
    const float inv = 1.0f / dist;
    delta.x *= inv;
    delta.y *= inv;
    delta.z *= inv;

    // Project onto listener basis (x = right, y = up, z = -ahead).
    return IPLVector3{
        ipl_dot(delta, listener.right),
        ipl_dot(delta, listener.up),
       -ipl_dot(delta, listener.ahead)
    };
}

// Local-to-world matrix (row-major, column vectors: world = M * local, meters, Steam Audio axes) of a brush entity at
// Source `origin` (inches) and `angles` (pitch, yaw, roll in degrees, engine.dll's AngleMatrix convention). Its mesh was
// converted with source_pos_to_ipl, so with C = that axis change: M = [ C * R * C^-1 | C * origin ].
inline IPLMatrix4x4 source_transform_to_ipl(const float origin[3], const float angles[3]) {
    constexpr double kDeg = 3.14159265358979323846 / 180.0;
    const double sp = std::sin(angles[0] * kDeg), cp = std::cos(angles[0] * kDeg);
    const double sy = std::sin(angles[1] * kDeg), cy = std::cos(angles[1] * kDeg);
    const double sr = std::sin(angles[2] * kDeg), cr = std::cos(angles[2] * kDeg);
    const double r[3][3] = { // columns: forward, left, up
        { cp * cy, sr * sp * cy - cr * sy, cr * sp * cy + sr * sy },
        { cp * sy, sr * sp * sy + cr * cy, cr * sp * sy - sr * cy },
        { -sp,     sr * cp,                cr * cp },
    };
    // C: ipl.x = -src.y, ipl.y = src.z, ipl.z = -src.x
    constexpr int kAxis[3] = { 1, 2, 0 };
    constexpr double kSign[3] = { -1.0, 1.0, -1.0 };
    const IPLVector3 t = source_pos_to_ipl(origin);
    IPLMatrix4x4 m{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            m.elements[i][j] = static_cast<float>(kSign[i] * kSign[j] * r[kAxis[i]][kAxis[j]]);
        }
    }
    m.elements[0][3] = t.x;
    m.elements[1][3] = t.y;
    m.elements[2][3] = t.z;
    m.elements[3][3] = 1.0f;
    return m;
}

inline IPLVector3 transform_point(const IPLMatrix4x4& m, IPLVector3 p) {
    return IPLVector3{
        m.elements[0][0] * p.x + m.elements[0][1] * p.y + m.elements[0][2] * p.z + m.elements[0][3],
        m.elements[1][0] * p.x + m.elements[1][1] * p.y + m.elements[1][2] * p.z + m.elements[1][3],
        m.elements[2][0] * p.x + m.elements[2][1] * p.y + m.elements[2][2] * p.z + m.elements[2][3]
    };
}

// Inverse of a rigid transform, for points and directions.
inline IPLVector3 inverse_transform_dir(const IPLMatrix4x4& m, IPLVector3 d) {
    return IPLVector3{
        m.elements[0][0] * d.x + m.elements[1][0] * d.y + m.elements[2][0] * d.z,
        m.elements[0][1] * d.x + m.elements[1][1] * d.y + m.elements[2][1] * d.z,
        m.elements[0][2] * d.x + m.elements[1][2] * d.y + m.elements[2][2] * d.z
    };
}

inline IPLVector3 inverse_transform_point(const IPLMatrix4x4& m, IPLVector3 p) {
    return inverse_transform_dir(m, IPLVector3{ p.x - m.elements[0][3], p.y - m.elements[1][3], p.z - m.elements[2][3] });
}

// The console variables engine.dll's SND_GetGain (RVA 0x11A0B0) uses, with the engine's own defaults.
// The Unofficial Patch's autoexec.cfg changes snd_refdist to 50, so they are read live.
struct GainModel {
    float snd_gain   = 1.0f;
    float gain_max   = 1.0f;
    float gain_min   = 0.01f;
    float foliage_db = 4.0f;  // snd_foliage_db_loss: extra dB lost per 1200 inches
    float ref_db     = 60.0f;
    float ref_dist   = 36.0f;
};

// engine.dll's own distance gain (SND_GetGain, RVA 0x11A0B0), without its line-of-sight "obscured" part, which
// Steam Audio's raytraced occlusion replaces. channel->dist_mult is NOT 1/max_radius: StartSound (RVA 0x11EF50)
// stores 10^(snd_refdb/20) / 10^(soundlevel_dB/20) / snd_refdist, so dist * dist_mult is the relative distance
// and the gain is its inverse, compressed above 0.5 and faded out below snd_gain_min.
inline float compute_source_distance_attenuation(
    float dist_meters,
    float dist_mult,
    const GainModel& m = GainModel())
{
    if (dist_mult <= 0.0f) return m.snd_gain;
    const float dist = dist_meters > 0.0f ? dist_meters * kMetersToSource : 0.0f;
    const float rel = std::pow(10.0f, dist * m.foliage_db / (1200.0f * 20.0f)) * dist * dist_mult;
    float gain = rel > 0.1f ? m.snd_gain / rel : m.snd_gain * 10.0f;
    if (gain > 0.5f) {
        float exponent = 2.5f;
        // In double like the x87 code: the engine truncates to int, and real levels sit right on integers.
        const double denom = static_cast<double>(m.ref_dist) * static_cast<double>(dist_mult);
        const double db = denom > 0.0 ? std::log10(std::pow(10.0, m.ref_db / 20.0) / denom) * 20.0 : 0.0;
        const int sound_level = static_cast<int>(db);
        if (sound_level > 90) exponent = 2.5f - static_cast<float>(sound_level - 90) * 1.7f / 50.0f;
        // 1 - 1 / (K * gain^exponent), K = 2 / 0.5^exponent: continuous with the 1/rel law at gain 0.5.
        gain = m.gain_max * (1.0f - 0.5f * std::pow(0.5f / gain, exponent));
    }
    if (gain < m.gain_min) gain = (2.0f - m.gain_min * rel) * m.gain_min;
    return gain < 0.0f ? 0.0f : gain;
}

// How strongly a source is localized (0 = in the head, 1 = fully binaural). Mirrors engine.dll's S_SpatializeChannel
// (RVA 0x11E4B0): inside channel->radius the direction fades out towards the inner half. Channels without a radius
// use a small head-sized one so walking through a source never flips its direction between two blocks.
inline float compute_spatial_blend(float dist_meters, float radius_units) {
    const float radius = radius_units > 24.0f ? radius_units : 24.0f;
    const float t = (dist_meters * kMetersToSource - radius * 0.5f) / (radius * 0.5f);
    return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
}

// Forward direction of an entity with these Source angles (pitch, yaw, roll in degrees), as engine.dll's AngleVectors.
inline void source_angles_forward(const float angles[3], float out[3]) {
    constexpr double kDeg = 3.14159265358979323846 / 180.0;
    const double sp = std::sin(angles[0] * kDeg), cp = std::cos(angles[0] * kDeg);
    const double sy = std::sin(angles[1] * kDeg), cy = std::cos(angles[1] * kDeg);
    out[0] = static_cast<float>(cp * cy);
    out[1] = static_cast<float>(cp * sy);
    out[2] = static_cast<float>(-sp);
}

// A talking head, as the direct path hears it: factors for all of the sound and for its middle and high band.
struct VoiceDirectivity {
    float gain = 1.0f;
    float mid  = 1.0f;
    float high = 1.0f;
};

// The weighted dipole Steam Audio calculates directivity with (iplDirectivityCalculate), for the cosine between where
// the source faces and where the listener is.
constexpr float kVoiceDipoleWeight = 0.40f;
constexpr float kVoiceDipolePower  = 1.75f;
inline float dipole_directivity(float cosine) {
    return std::pow(std::fabs((1.0f - kVoiceDipoleWeight) + kVoiceDipoleWeight * cosine), kVoiceDipolePower);
}

// The plain dipole would take 24 dB off a character from behind. A real head, and what the room adds, leave most of it:
// -3 dB behind, -4 dB in the middle and -6 dB in the high band. Close to the mouth direction means nothing.
inline VoiceDirectivity voice_directivity(IPLVector3 forward, IPLVector3 source, IPLVector3 listener) {
    VoiceDirectivity v;
    const IPLVector3 to{ listener.x - source.x, listener.y - source.y, listener.z - source.z };
    const float dist = std::sqrt(ipl_dot(to, to));
    if (dist < 1e-3f) return v;
    const float cosine = ipl_dot(forward, to) / dist;
    const float near_t = (dist - 0.4f) / 0.6f; // none within 0.4 m, all of it from 1 m on
    const float depth = near_t < 0.0f ? 0.0f : (near_t > 1.0f ? 1.0f : near_t);
    const float away = (1.0f - dipole_directivity(cosine > 1.0f ? 1.0f : (cosine < -1.0f ? -1.0f : cosine))) * depth;
    v.gain = 1.0f - 0.31f * away;
    v.mid  = 1.0f - 0.12f * away;
    v.high = 1.0f - 0.30f * away;
    return v;
}

} // namespace sa
