#include "sky_exposure.h"
#include "coord_util.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace sa {
namespace {

constexpr uint32_t kLeafTriangles = 4;
constexpr float kMaxRay = 400.0f; // meters: the longest map diagonal (32768 inches) is 830 m, skies are far closer

float axis(const IPLVector3& v, int a) { return a == 0 ? v.x : (a == 1 ? v.y : v.z); }

constexpr float kMaxWall = 2.5f; // meters: how deep in a wall a sound may sit and still come out of it

float intersect(const IPLVector3& o, const IPLVector3& d, const IPLVector3& a, const IPLVector3& b, const IPLVector3& c,
                bool& back_face) {
    const IPLVector3 e1{ b.x - a.x, b.y - a.y, b.z - a.z };
    const IPLVector3 e2{ c.x - a.x, c.y - a.y, c.z - a.z };
    const IPLVector3 p = ipl_cross(d, e2);
    const float det = ipl_dot(e1, p);
    if (std::fabs(det) < 1e-9f) return -1.0f;
    back_face = det < 0.0f;
    const float inv = 1.0f / det;
    const IPLVector3 s{ o.x - a.x, o.y - a.y, o.z - a.z };
    const float u = ipl_dot(s, p) * inv;
    if (u < 0.0f || u > 1.0f) return -1.0f;
    const IPLVector3 q = ipl_cross(s, e1);
    const float v = ipl_dot(d, q) * inv;
    if (v < 0.0f || u + v > 1.0f) return -1.0f;
    return ipl_dot(e2, q) * inv;
}

} // namespace

void TriangleBvh::clear() {
    vertices_.clear();
    triangles_.clear();
    order_.clear();
    nodes_.clear();
    first_sky_ = 0;
}

void TriangleBvh::build(std::vector<IPLVector3> vertices, std::vector<IPLTriangle> triangles, std::size_t first_sky_triangle) {
    clear();
    vertices_ = std::move(vertices);
    triangles_ = std::move(triangles);
    first_sky_ = first_sky_triangle;
    if (triangles_.empty()) return;
    order_.resize(triangles_.size());
    std::vector<IPLVector3> centers(triangles_.size());
    for (std::size_t i = 0; i < triangles_.size(); ++i) {
        order_[i] = static_cast<uint32_t>(i);
        const IPLTriangle& t = triangles_[i];
        const IPLVector3& a = vertices_[static_cast<std::size_t>(t.indices[0])];
        const IPLVector3& b = vertices_[static_cast<std::size_t>(t.indices[1])];
        const IPLVector3& c = vertices_[static_cast<std::size_t>(t.indices[2])];
        centers[i] = { (a.x + b.x + c.x) / 3.0f, (a.y + b.y + c.y) / 3.0f, (a.z + b.z + c.z) / 3.0f };
    }
    nodes_.reserve(triangles_.size());
    build_node(0, static_cast<uint32_t>(triangles_.size()), centers);
}

uint32_t TriangleBvh::build_node(uint32_t first, uint32_t count, std::vector<IPLVector3>& centers) {
    const uint32_t index = static_cast<uint32_t>(nodes_.size());
    nodes_.emplace_back();
    Node node;
    node.lo = { 1e30f, 1e30f, 1e30f };
    node.hi = { -1e30f, -1e30f, -1e30f };
    IPLVector3 clo = node.lo, chi = node.hi;
    for (uint32_t i = first; i < first + count; ++i) {
        for (IPLint32 vi : triangles_[order_[i]].indices) {
            const IPLVector3& v = vertices_[static_cast<std::size_t>(vi)];
            node.lo = { std::min(node.lo.x, v.x), std::min(node.lo.y, v.y), std::min(node.lo.z, v.z) };
            node.hi = { std::max(node.hi.x, v.x), std::max(node.hi.y, v.y), std::max(node.hi.z, v.z) };
        }
        const IPLVector3& c = centers[order_[i]];
        clo = { std::min(clo.x, c.x), std::min(clo.y, c.y), std::min(clo.z, c.z) };
        chi = { std::max(chi.x, c.x), std::max(chi.y, c.y), std::max(chi.z, c.z) };
    }
    int split_axis = 0;
    for (int a = 1; a < 3; ++a) {
        if (axis(chi, a) - axis(clo, a) > axis(chi, split_axis) - axis(clo, split_axis)) split_axis = a;
    }
    if (count <= kLeafTriangles || axis(chi, split_axis) - axis(clo, split_axis) < 1e-5f) {
        node.first = first;
        node.count = count;
        nodes_[index] = node;
        return index;
    }
    const uint32_t mid = count / 2;
    std::nth_element(order_.begin() + first, order_.begin() + first + mid, order_.begin() + first + count,
                     [&](uint32_t l, uint32_t r) { return axis(centers[l], split_axis) < axis(centers[r], split_axis); });
    build_node(first, mid, centers); // == index + 1
    node.first = build_node(first + mid, count - mid, centers);
    node.count = 0;
    nodes_[index] = node;
    return index;
}

float TriangleBvh::closest_hit(IPLVector3 o, IPLVector3 d, float max_distance, bool& hit_sky, bool* back_face) const {
    hit_sky = false;
    if (back_face) *back_face = false;
    if (nodes_.empty()) return -1.0f;
    const auto inv = [](float v) { return 1.0f / (std::fabs(v) > 1e-12f ? v : (v < 0.0f ? -1e-12f : 1e-12f)); };
    const IPLVector3 id{ inv(d.x), inv(d.y), inv(d.z) };
    float best = max_distance;
    float found = -1.0f;
    uint32_t stack[64];
    int top = 0;
    stack[top++] = 0;
    while (top > 0) {
        const uint32_t ni = stack[--top];
        const Node& n = nodes_[ni];
        float t0 = 0.0f, t1 = best;
        for (int a = 0; a < 3; ++a) {
            float x = (axis(n.lo, a) - axis(o, a)) * axis(id, a);
            float y = (axis(n.hi, a) - axis(o, a)) * axis(id, a);
            if (x > y) std::swap(x, y);
            t0 = std::max(t0, x);
            t1 = std::min(t1, y);
        }
        if (t0 > t1 + 1e-4f) continue;
        if (n.count > 0) {
            for (uint32_t i = n.first; i < n.first + n.count; ++i) {
                const IPLTriangle& t = triangles_[order_[i]];
                bool back = false;
                const float hit = intersect(o, d, vertices_[static_cast<std::size_t>(t.indices[0])],
                                            vertices_[static_cast<std::size_t>(t.indices[1])],
                                            vertices_[static_cast<std::size_t>(t.indices[2])], back);
                // Where two brushes touch, the one that is left counts.
                if (hit > 1e-3f && (hit < best || (back_face && back && hit < best + 1e-4f && found >= 0.0f))) {
                    best = found = hit;
                    if (back_face) *back_face = back;
                    hit_sky = order_[i] >= first_sky_;
                }
            }
        } else if (top + 2 <= 64) {
            stack[top++] = n.first;
            stack[top++] = ni + 1;
        }
    }
    return found;
}

void SkyProbe::build(const AcousticMeshData& mesh) {
    has_sky_ = !mesh.sky_triangles.empty();
    world_triangles_ = mesh.triangles.size();
    std::vector<IPLVector3> vertices = mesh.vertices;
    std::vector<IPLTriangle> triangles = mesh.triangles;
    const IPLint32 base = static_cast<IPLint32>(vertices.size());
    vertices.insert(vertices.end(), mesh.sky_vertices.begin(), mesh.sky_vertices.end());
    for (IPLTriangle t : mesh.sky_triangles) {
        for (IPLint32& i : t.indices) i += base;
        triangles.push_back(t);
    }
    bvh_.build(std::move(vertices), std::move(triangles), mesh.triangles.size());
}

float SkyProbe::exposure(IPLVector3 listener, const Mover* movers, std::size_t mover_count) const {
    if (!has_sky_) return 1.0f;

    // Closest hit against both static world brushes and dynamic brush entities (doors, garage shutters).
    const auto trace_solid = [&](const IPLVector3& o, const IPLVector3& d, float max_dist, bool& hit_sky) -> float {
        hit_sky = false;
        float t = bvh_.closest_hit(o, d, max_dist, hit_sky);
        const float limit = t >= 0.0f ? t : max_dist;
        for (std::size_t m = 0; m < mover_count; ++m) {
            if (!movers[m].bvh) continue;
            bool ignored = false;
            const float mt = movers[m].bvh->closest_hit(
                inverse_transform_point(movers[m].local_to_world, o),
                inverse_transform_dir(movers[m].local_to_world, d), limit, ignored);
            if (mt >= 0.0f && (t < 0.0f || mt < t)) {
                t = mt;
                hit_sky = false;
            }
        }
        return t;
    };

    // 1. Direct overhead sky: straight up and three rings.
    static const struct { float elevation; int rays; float weight; } kRings[] = {
        { 90.0f, 1, 3.0f }, { 70.0f, 8, 1.0f }, { 45.0f, 12, 0.75f }, { 22.0f, 12, 0.4f },
    };
    float open = 0.0f, total = 0.0f;
    for (const auto& ring : kRings) {
        const float el = ring.elevation * 0.017453293f;
        for (int k = 0; k < ring.rays; ++k) {
            const float az = 6.2831853f * (static_cast<float>(k) + 0.5f) / static_cast<float>(ring.rays);
            const IPLVector3 dir{ std::cos(el) * std::cos(az), std::sin(el), std::cos(el) * std::sin(az) };
            bool sky = false;
            const float t = trace_solid(listener, dir, kMaxRay, sky);
            if (t < 0.0f) continue; // left the map: says nothing
            total += ring.weight;
            if (sky) open += ring.weight;
        }
    }
    const float direct = total > 0.0f ? open / total : 1.0f;

    // 2. Nearby openings to the rain: under a small awning or right inside a parking garage exit, overhead sky is
    // blocked (and buildings across the street block shallow upward rays), yet rain falls openly a meter or two away
    // through the opening. Probe horizontally (and slightly upward for ramps) to find the nearest point in each
    // direction with open sky directly above it, and weight by opening width and distance.
    constexpr float kPortalWeight  = 0.85f;
    if (direct >= kPortalWeight) return direct; // horizontal portals cannot raise exposure further

    constexpr int   kPortalRays    = 16;
    constexpr float kMaxPortalDist = 16.0f;
    constexpr float kPortalRefDist = 5.5f;
    constexpr float kPortalPower   = 1.4f;
    static const float kProbeDists[] = { 0.6f, 1.2f, 2.0f, 3.0f, 4.5f, 6.5f, 9.5f, 13.0f, 15.5f };
    static const float kElevations[] = { 0.0f, 15.0f * 0.017453293f }; // eye level + slight tilt for sloping ramps

    const auto sky_above = [&](const IPLVector3& p) -> bool {
        bool sky = false;
        const float t = trace_solid(p, IPLVector3{ 0.0f, 1.0f, 0.0f }, kMaxRay, sky);
        return t >= 0.0f && sky;
    };

    const auto nearest_rain = [&](const IPLVector3& dir) -> float {
        bool hit_sky = false;
        const float t = trace_solid(listener, dir, kMaxPortalDist, hit_sky);
        const float clear_dist = (t >= 0.0f && !hit_sky) ? t : kMaxPortalDist;
        const float max_probe = clear_dist - 0.2f;
        if (max_probe < 0.25f) return -1.0f;
        const auto point_at = [&](float d) {
            return IPLVector3{ listener.x + dir.x * d, listener.y + dir.y * d, listener.z + dir.z * d };
        };
        float prev = 0.0f;
        for (float d : kProbeDists) {
            if (d > max_probe) break;
            if (sky_above(point_at(d))) {
                const float mid = 0.5f * (prev + d);
                return (mid >= 0.25f && sky_above(point_at(mid))) ? mid : d;
            }
            prev = d;
        }
        if (max_probe > prev + 0.25f && sky_above(point_at(max_probe))) {
            const float mid = 0.5f * (prev + max_probe);
            return (mid >= 0.25f && sky_above(point_at(mid))) ? mid : max_probe;
        }
        return -1.0f;
    };

    float portal = 0.0f;
    for (int k = 0; k < kPortalRays; ++k) {
        const float az = 6.2831853f * (static_cast<float>(k) + 0.5f) / static_cast<float>(kPortalRays);
        const float ca = std::cos(az), sa_az = std::sin(az);
        float best_d = -1.0f;
        for (float el : kElevations) {
            const float ce = std::cos(el), se = std::sin(el);
            const float d = nearest_rain(IPLVector3{ ce * ca, se, ce * sa_az });
            if (d >= 0.0f && (best_d < 0.0f || d < best_d)) best_d = d;
            if (best_d >= 0.0f && best_d <= 1.2f) break; // already right outside
        }
        if (best_d >= 0.0f && best_d < kMaxPortalDist) {
            const float fade = 1.0f - best_d / kMaxPortalDist;
            portal += fade / (1.0f + std::pow(best_d / kPortalRefDist, kPortalPower));
        }
    }
    portal /= static_cast<float>(kPortalRays);
    return std::max(direct, kPortalWeight * portal);
}

// How far a point inside solid world brushes is from where it comes out of them in this direction; negative: too far.
// Walls are made of several brushes that touch or overlap.
float SkyProbe::solid_exit(IPLVector3 source, IPLVector3 dir, float max_distance) const {
    float pos = 0.0f, exit = -1.0f;
    int depth = 1;
    for (int i = 0; i < 16; ++i) {
        const IPLVector3 at{ source.x + dir.x * pos, source.y + dir.y * pos, source.z + dir.z * pos };
        bool sky = false, back = false;
        const float hit = pos < max_distance + 1.0f ? bvh_.closest_hit(at, dir, max_distance + 1.0f - pos, sky, &back) : -1.0f;
        if (hit < 0.0f || sky) break;
        pos += hit;
        if (back) {
            exit = pos;
            if (depth > 0) --depth;
        } else {
            if (depth == 0 && pos - exit > 0.02f) break; // the next wall
            ++depth;
        }
        pos += 0.003f;
    }
    return depth == 0 && exit <= max_distance ? exit : -1.0f;
}

bool SkyProbe::blocked(IPLVector3 from, IPLVector3 to) const {
    const IPLVector3 delta{ to.x - from.x, to.y - from.y, to.z - from.z };
    const float dist = std::sqrt(ipl_dot(delta, delta));
    if (!(dist > 1e-3f)) return false;
    bool sky = false;
    return bvh_.closest_hit(from, IPLVector3{ delta.x / dist, delta.y / dist, delta.z / dist }, dist, sky) >= 0.0f && !sky;
}

IPLVector3 SkyProbe::open_direction(IPLVector3 listener, IPLVector3 source) const {
    constexpr float kBelow = 2.5f;    // meters under the listener's ears: the ground is closer
    constexpr float kClearance = 0.5f; // in front of the wall at the end
    constexpr float kMinFree = 3.0f;
    if (world_triangles_ == 0 || bvh_.empty()) return source;
    IPLVector3 delta{ source.x - listener.x, source.y - listener.y, source.z - listener.z };
    if (delta.y < -kBelow) delta.y = -delta.y;
    const float dist = std::sqrt(ipl_dot(delta, delta));
    const float level = std::sqrt(delta.x * delta.x + delta.z * delta.z);
    if (!(dist > 1.0f)) return source;
    const float azimuth = std::atan2(delta.z, delta.x);
    const float wanted = std::min(dist, std::max(0.6f * dist, kOpenStreet)); // near sounds: all of the way
    const float needed = std::min(0.6f * dist, kOpenStreet);

    const auto direction = [&](float az) {
        return IPLVector3{ std::cos(az) * level / dist, delta.y / dist, std::sin(az) * level / dist };
    };
    const auto free_path = [&](const IPLVector3& dir) {
        bool sky = false;
        const float t = bvh_.closest_hit(listener, dir, wanted, sky);
        return t < 0.0f || sky ? wanted : t;
    };
    const auto place = [&](const IPLVector3& dir, float free) {
        const float at = free >= wanted ? dist : std::max(0.5f * free, free - kClearance);
        return IPLVector3{ listener.x + dir.x * at, listener.y + dir.y * at, listener.z + dir.z * at };
    };

    float best = 0.0f;
    IPLVector3 best_dir{};
    constexpr int kSteps = 12; // 15 degrees
    for (int k = 0; k <= kSteps; ++k) {
        for (int sign = 1; sign >= -1; sign -= 2) {
            if (sign < 0 && (k == 0 || k == kSteps)) continue;
            const IPLVector3 dir = direction(azimuth + static_cast<float>(sign * k) * 3.14159265f / kSteps);
            const float free = free_path(dir);
            if (free >= needed) return place(dir, free);
            if (free > best) {
                best = free;
                best_dir = dir;
            }
        }
    }
    return best >= kMinFree ? place(best_dir, best) : source;
}

void GlassProbe::build(const AcousticMeshData& mesh) {
    bvh_.build(mesh.glass_vertices, mesh.glass_triangles, mesh.glass_triangles.size());
}

bool GlassProbe::hit(IPLVector3 from, IPLVector3 to, const Mover* movers, std::size_t mover_count) const {
    const IPLVector3 delta{ to.x - from.x, to.y - from.y, to.z - from.z };
    const float dist = std::sqrt(ipl_dot(delta, delta));
    if (!(dist > 0.05f)) return false;
    const IPLVector3 dir{ delta.x / dist, delta.y / dist, delta.z / dist };
    bool sky = false;
    if (bvh_.closest_hit(from, dir, dist, sky) >= 0.0f) return true;
    for (std::size_t m = 0; m < mover_count; ++m) {
        if (movers[m].bvh && movers[m].bvh->closest_hit(inverse_transform_point(movers[m].local_to_world, from),
                                                        inverse_transform_dir(movers[m].local_to_world, dir), dist, sky) >= 0.0f) {
            return true;
        }
    }
    return false;
}

float GlassProbe::between(IPLVector3 listener, IPLVector3 mouth, const Mover* movers, std::size_t mover_count) const {
    constexpr float kSpread = 0.15f; // meters around the mouth
    if (bvh_.empty() && mover_count == 0) return 0.0f;
    const IPLVector3 delta{ mouth.x - listener.x, mouth.y - listener.y, mouth.z - listener.z };
    const float flat = std::sqrt(delta.x * delta.x + delta.z * delta.z);
    const IPLVector3 side = flat > 1e-3f ? IPLVector3{ -delta.z / flat * kSpread, 0.0f, delta.x / flat * kSpread }
                                         : IPLVector3{ kSpread, 0.0f, 0.0f };
    const IPLVector3 targets[5] = {
        mouth,
        { mouth.x + side.x, mouth.y, mouth.z + side.z },
        { mouth.x - side.x, mouth.y, mouth.z - side.z },
        { mouth.x, mouth.y + kSpread, mouth.z },
        { mouth.x, mouth.y - kSpread, mouth.z },
    };
    int hits = 0;
    for (const IPLVector3& target : targets) hits += hit(listener, target, movers, mover_count) ? 1 : 0;
    return std::clamp((static_cast<float>(hits) - 1.0f) / 2.0f, 0.0f, 1.0f); // one ray is a corner, three are a pane
}

IPLVector3 SkyProbe::leave_solid(IPLVector3 source, IPLVector3 listener, float clearance) const {
    if (world_triangles_ == 0 || bvh_.empty()) return source;
    const IPLVector3 delta{ listener.x - source.x, listener.y - source.y, listener.z - source.z };
    const float dist = std::sqrt(ipl_dot(delta, delta));
    if (dist < 1e-3f) return source;
    const IPLVector3 ahead{ delta.x / dist, delta.y / dist, delta.z / dist };
    bool sky = false, back = false;
    if (bvh_.closest_hit(source, ahead, dist, sky, &back) < 0.0f) return source; // nothing in between

    static const IPLVector3 kAxes[6] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
    int inside = back ? 1 : 0;
    for (const IPLVector3& d : kAxes) {
        back = false;
        if (bvh_.closest_hit(source, d, kMaxRay, sky, &back) >= 0.0f && back && !sky) ++inside;
    }
    if (inside < 4) return source;

    // Inside a wall. Out towards the listener if that is not far, else where the surface is nearest.
    IPLVector3 dir = ahead;
    float exit = solid_exit(source, ahead, std::min(kMaxWall, dist));
    if (exit >= 0.0f) {
        exit = std::min(exit + clearance, std::max(exit, 0.95f * dist));
        return IPLVector3{ source.x + dir.x * exit, source.y + dir.y * exit, source.z + dir.z * exit };
    }
    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            for (int z = -1; z <= 1; ++z) {
                if (x == 0 && y == 0 && z == 0) continue;
                const IPLVector3 d = ipl_normalize(IPLVector3{ static_cast<float>(x), static_cast<float>(y),
                                                               static_cast<float>(z) }, ahead);
                const float e = solid_exit(source, d, kMaxWall);
                if (e >= 0.0f && (exit < 0.0f || e < exit)) {
                    exit = e;
                    dir = d;
                }
            }
        }
    }
    if (exit < 0.0f) return source;
    exit += clearance;
    return IPLVector3{ source.x + dir.x * exit, source.y + dir.y * exit, source.z + dir.z * exit };
}

} // namespace sa
