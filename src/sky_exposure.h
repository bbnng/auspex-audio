#pragma once

#include "bsp_acoustics.h"

#include <phonon.h>

#include <cstdint>
#include <vector>

namespace sa {

// Closest-hit rays against a triangle soup (bounding volume hierarchy). Steam Audio's C API offers no ray queries, and
// the sky question needs to know *what* a ray hits: geometry of other parts of the map lies above the sky of this one.
class TriangleBvh {
public:
    // Triangles from `first_sky_triangle` on are the sky.
    void build(std::vector<IPLVector3> vertices, std::vector<IPLTriangle> triangles, std::size_t first_sky_triangle);
    void clear();
    bool empty() const { return nodes_.empty(); }

    // Distance along the (normalized) direction, or a negative value. `hit_sky`: the closest hit shows the sky.
    // `back_face`: the triangle was hit from behind, so the ray started inside a solid (brush sides face outwards).
    float closest_hit(IPLVector3 origin, IPLVector3 dir, float max_distance, bool& hit_sky, bool* back_face = nullptr) const;

    IPLVector3 bounds_min() const { return nodes_.empty() ? IPLVector3{} : nodes_[0].lo; }
    IPLVector3 bounds_max() const { return nodes_.empty() ? IPLVector3{} : nodes_[0].hi; }

private:
    struct Node {
        IPLVector3 lo, hi;
        uint32_t first = 0; // leaf: first entry of order_; inner: index of the right child (the left one follows)
        uint32_t count = 0; // > 0: leaf
    };
    uint32_t build_node(uint32_t first, uint32_t count, std::vector<IPLVector3>& centers);

    std::vector<IPLVector3> vertices_;
    std::vector<IPLTriangle> triangles_;
    std::vector<uint32_t> order_;
    std::vector<Node> nodes_;
    std::size_t first_sky_ = 0;
};

// How much open sky is above a point: which part of the rain's sound arrives unobstructed there.
class SkyProbe {
public:
    struct Mover { // a door or another moving brush model
        const TriangleBvh* bvh = nullptr;
        IPLMatrix4x4 local_to_world{};
    };

    void build(const AcousticMeshData& mesh);
    void clear() { bvh_.clear(); world_triangles_ = 0; has_sky_ = false; }
    bool has_sky() const { return has_sky_; }

    // 1 = open sky, 0 = completely covered. Maps without sky and points no ray leads anywhere from count as open:
    // nothing is muffled because of missing data.
    float exposure(IPLVector3 listener, const Mover* movers, std::size_t mover_count) const;

    // Some of the game's sounds sit inside a wall on purpose (club music in the wall next to the door, lasers, pipes).
    // engine.dll takes about 3 dB off those; to a raytracer they are buried. Returns where the sound comes out of that
    // wall, `clearance` meters in front of it, or `source` itself if it stands in the open (or too deep in).
    IPLVector3 leave_solid(IPLVector3 source, IPLVector3 listener, float clearance) const;

    // For street sounds the game throws somewhere on a circle around the listener. Returns the direction closest to
    // where it is that is open for `kOpenStreet` meters (or most of the way, or to the sky), at the same distance or in
    // front of what ends the street. Sounds far below the listener's feet, airplanes among them, go as far above. If
    // the listener is boxed in, `source` itself.
    static constexpr float kOpenStreet = 12.0f;
    IPLVector3 open_direction(IPLVector3 listener, IPLVector3 source) const;
    // Something of the map (not the sky) is between two points.
    bool blocked(IPLVector3 from, IPLVector3 to) const;

private:
    float solid_exit(IPLVector3 source, IPLVector3 dir, float max_distance) const;

    TriangleBvh bvh_;
    std::size_t world_triangles_ = 0;
    bool has_sky_ = false;
};

// Is there glass between a listener and somebody who talks to them? Sees nothing but panes of glass: bars, desks,
// chairs, pillars and corners never count.
class GlassProbe {
public:
    using Mover = SkyProbe::Mover; // doors with a window; their bvh holds the panes only

    void build(const AcousticMeshData& mesh);
    void clear() { bvh_.clear(); }
    bool empty() const { return bvh_.empty(); }

    // 1: the voice comes through glass, 0: past it. Several rays to around the mouth, so that the edge of a pane or the
    // slot in it is no switch.
    float between(IPLVector3 listener, IPLVector3 mouth, const Mover* movers, std::size_t mover_count) const;

private:
    bool hit(IPLVector3 from, IPLVector3 to, const Mover* movers, std::size_t mover_count) const;

    TriangleBvh bvh_;
};

} // namespace sa
