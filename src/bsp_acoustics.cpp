#include "bsp_acoustics.h"
#include "coord_util.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace sa {
namespace {

struct Vec3d {
    double x, y, z;
};

inline Vec3d v_add(Vec3d a, Vec3d b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
inline Vec3d v_sub(Vec3d a, Vec3d b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
inline Vec3d v_scale(Vec3d a, double s) { return { a.x * s, a.y * s, a.z * s }; }
inline double v_dot(Vec3d a, Vec3d b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3d v_cross(Vec3d a, Vec3d b) {
    return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}
inline double v_len(Vec3d a) { return std::sqrt(v_dot(a, a)); }

void make_base_winding(const AcousticBrushPlane& p, std::vector<Vec3d>& poly) {
    poly.clear();
    const Vec3d n = { p.nx, p.ny, p.nz };
    const double nlen = v_len(n);
    if (!(nlen >= 1e-6) || !std::isfinite(nlen) || !std::isfinite(p.dist)) return; // a broken plane bounds nothing
    const Vec3d nn = v_scale(n, 1.0 / nlen);

    Vec3d up = (std::fabs(nn.z) < 0.9) ? Vec3d{ 0.0, 0.0, 1.0 } : Vec3d{ 1.0, 0.0, 0.0 };
    Vec3d right = v_cross(up, nn);
    const double rlen = v_len(right);
    if (rlen < 1e-6) return;
    right = v_scale(right, 1.0 / rlen);
    up = v_cross(nn, right);

    constexpr double kMaxCoord = 32768.0;
    const Vec3d org = v_scale(nn, static_cast<double>(p.dist) / nlen);
    const Vec3d rx = v_scale(right, kMaxCoord);
    const Vec3d ry = v_scale(up, kMaxCoord);

    poly.push_back(v_sub(v_sub(org, rx), ry));
    poly.push_back(v_sub(v_add(org, rx), ry));
    poly.push_back(v_add(v_add(org, rx), ry));
    poly.push_back(v_add(v_sub(org, rx), ry));
}

void clip_polygon(std::vector<Vec3d>& poly, const AcousticBrushPlane& clip) {
    if (poly.size() < 3) { poly.clear(); return; }
    constexpr double kEps = 0.01;
    const Vec3d n = { clip.nx, clip.ny, clip.nz };
    const double d = clip.dist;
    if (!std::isfinite(v_dot(n, n)) || !std::isfinite(d)) return; // NaN distances would keep or drop the polygon at random

    std::vector<double> dists(poly.size());
    int front = 0, back = 0;
    for (std::size_t i = 0; i < poly.size(); ++i) {
        const double dist = v_dot(poly[i], n) - d;
        dists[i] = dist;
        if (dist > kEps) ++front;
        else if (dist < -kEps) ++back;
    }
    if (front == 0) return;
    if (back == 0) { poly.clear(); return; }

    std::vector<Vec3d> out;
    out.reserve(poly.size() + 2);
    for (std::size_t i = 0; i < poly.size(); ++i) {
        const std::size_t j = (i + 1) % poly.size();
        const Vec3d p0 = poly[i], p1 = poly[j];
        const double d0 = dists[i], d1 = dists[j];
        const bool in0 = d0 <= kEps;
        const bool in1 = d1 <= kEps;
        if (in0) out.push_back(p0);
        if (in0 != in1) {
            const double denom = d0 - d1;
            if (std::fabs(denom) > 1e-12) {
                const double t = d0 / denom;
                out.push_back(v_add(p0, v_scale(v_sub(p1, p0), t)));
            }
        }
    }
    poly.swap(out);
}

bool contains_ci(const char* haystack, const char* needle) {
    if (!haystack || !needle || !*needle) return false;
    const std::size_t nlen = std::strlen(needle);
    for (const char* p = haystack; *p; ++p) {
        std::size_t k = 0;
        while (k < nlen && p[k] &&
               std::tolower(static_cast<unsigned char>(p[k])) ==
               std::tolower(static_cast<unsigned char>(needle[k]))) {
            ++k;
        }
        if (k == nlen) return true;
    }
    return false;
}

// Verified engine.dll offsets for VTMB build vtmb-x86 (image base 0x20000000; g_CollisionBSPData at 0x2034AAF0).
constexpr std::uintptr_t kRvaCollisionBspData = 0x34AAF0;
constexpr std::size_t kOffMapName       = 0x04;
constexpr std::size_t kOffNumBrushSides = 0x84;
constexpr std::size_t kOffBrushSides    = 0x88;      // 12 bytes: +0 cplane_t*, +4 csurface_t*, +8 u16 bBevel
constexpr std::size_t kOffNumNodes      = 0x200104;
constexpr std::size_t kOffNodes         = 0x200108;  // 12 bytes: +0 cplane_t*, +4 int children[2]
constexpr std::size_t kOffNumLeafs      = 0x2C0090;
constexpr std::size_t kOffLeafs         = 0x2C0094;  // 20 bytes: +12 u16 firstleafbrush, +14 u16 numleafbrushes
constexpr std::size_t kOffNumLeafBrushes = 0x4001FC;
constexpr std::size_t kOffLeafBrushes   = 0x400200;  // u16[65536]
constexpr std::size_t kOffNumCModels    = 0x420160;
constexpr std::size_t kOffCModels       = 0x420164;  // 52 bytes: +36 int headnode
constexpr std::size_t kOffNumBrushes    = 0x42D164;
constexpr std::size_t kOffBrushes       = 0x42D168;  // 24 bytes: +0 contents, +4 numsides, +8 firstbrushside

constexpr int kContentsSolid  = 0x1;
constexpr int kContentsWindow = 0x2; // glass: solid to the player and to sound, but not "solid" to the engine
constexpr int kSurfSky2D = 0x2;
constexpr int kSurfSky   = 0x4;

struct RawBrush { int contents; int numsides; int firstbrushside; };
struct RawNode  { int child0; int child1; };
struct RawLeaf  { std::uint16_t firstleafbrush; std::uint16_t numleafbrushes; };

enum SideFlags : std::uint8_t { kSideValid = 1, kSideOpen = 2, kSidePane = 4 };

// The parts of a BSP that matter here, from engine.dll's memory or from a .bsp file.
struct BspGeometry {
    std::string map_name;
    std::vector<RawBrush> brushes;
    std::vector<AcousticBrushPlane> side_planes;
    std::vector<std::uint8_t> side_flags;
    std::vector<RawNode> nodes;
    std::vector<RawLeaf> leafs;
    std::vector<std::uint16_t> leafbrushes;
    std::vector<int> headnodes; // one per brush model, [0] = world
};

struct BspCounts {
    char map_name[128] = {};
    int brushes = 0, brushsides = 0, nodes = 0, leafs = 0, leafbrushes = 0, cmodels = 0;
};

bool read_bsp_counts_seh(const unsigned char* bsp, BspCounts& out) {
    __try {
        std::memcpy(out.map_name, bsp + kOffMapName, sizeof(out.map_name) - 1);
        out.map_name[sizeof(out.map_name) - 1] = '\0';
        if (out.map_name[0] == '\0') return false;
        out.brushes     = *reinterpret_cast<const int*>(bsp + kOffNumBrushes);
        out.brushsides  = *reinterpret_cast<const int*>(bsp + kOffNumBrushSides);
        out.nodes       = *reinterpret_cast<const int*>(bsp + kOffNumNodes);
        out.leafs       = *reinterpret_cast<const int*>(bsp + kOffNumLeafs);
        out.leafbrushes = *reinterpret_cast<const int*>(bsp + kOffNumLeafBrushes);
        out.cmodels     = *reinterpret_cast<const int*>(bsp + kOffNumCModels);
        return out.brushes > 0 && out.brushes <= 8192 && out.brushsides > 0 && out.brushsides <= 65536 &&
               out.nodes > 0 && out.nodes <= 65536 && out.leafs > 0 && out.leafs <= 65536 &&
               out.leafbrushes > 0 && out.leafbrushes <= 65536 && out.cmodels > 0 && out.cmodels <= 1024;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool read_bsp_arrays_seh(
    const unsigned char* bsp,
    const BspCounts& n,
    RawBrush* brushes,
    AcousticBrushPlane* side_planes,
    std::uint8_t* side_flags,
    RawNode* nodes,
    RawLeaf* leafs,
    std::uint16_t* leafbrushes,
    int* headnodes)
{
    __try {
        for (int i = 0; i < n.brushes; ++i) {
            const unsigned char* b = bsp + kOffBrushes + static_cast<std::size_t>(i) * 24;
            brushes[i].contents       = *reinterpret_cast<const int*>(b + 0);
            brushes[i].numsides       = *reinterpret_cast<const int*>(b + 4);
            brushes[i].firstbrushside = *reinterpret_cast<const int*>(b + 8);
        }
        for (int i = 0; i < n.brushsides; ++i) {
            const unsigned char* s = bsp + kOffBrushSides + static_cast<std::size_t>(i) * 12;
            const auto* plane = *reinterpret_cast<const float* const*>(s + 0);
            const auto* surf  = *reinterpret_cast<const char* const* const*>(s + 4);
            const std::uint16_t bevel = *reinterpret_cast<const std::uint16_t*>(s + 8);
            if (!plane || bevel != 0) continue;
            const float nx = plane[0], ny = plane[1], nz = plane[2], dist = plane[3];
            if (!std::isfinite(nx) || !std::isfinite(ny) || !std::isfinite(nz) || !std::isfinite(dist)) continue;
            IPLint32 mat = kMatDefault;
            bool sky = false;
            side_flags[i] = kSideValid;
            if (surf && *surf) {
                // csurface_t begins with const char* name; copy safely to stack for classification.
                char sname[128];
                const char* src = *surf;
                std::size_t k = 0;
                for (; k + 1 < sizeof(sname) && src[k] != '\0'; ++k) sname[k] = src[k];
                sname[k] = '\0';
                mat = classify_surface_material(sname);
                // csurface_t (CMod_LoadTextures 0x32E10 / LoadTexinfo 0x33050): +0 name, +4 u16 surfaceprop, +6 u16 flags.
                const int flags = *reinterpret_cast<const std::uint16_t*>(reinterpret_cast<const unsigned char*>(surf) + 6);
                if (surface_lets_sound_through(sname, flags)) side_flags[i] |= kSideOpen;
                if (surface_is_pane(sname)) side_flags[i] |= kSidePane;
                sky = surface_is_sky(sname, flags);
            }
            side_planes[i].nx = nx;
            side_planes[i].ny = ny;
            side_planes[i].nz = nz;
            side_planes[i].dist = dist;
            side_planes[i].material_index = mat;
            side_planes[i].sky = sky;
        }
        for (int i = 0; i < n.nodes; ++i) {
            const unsigned char* node = bsp + kOffNodes + static_cast<std::size_t>(i) * 12;
            nodes[i].child0 = *reinterpret_cast<const int*>(node + 4);
            nodes[i].child1 = *reinterpret_cast<const int*>(node + 8);
        }
        for (int i = 0; i < n.leafs; ++i) {
            const unsigned char* l = bsp + kOffLeafs + static_cast<std::size_t>(i) * 20;
            leafs[i].firstleafbrush = *reinterpret_cast<const std::uint16_t*>(l + 12);
            leafs[i].numleafbrushes = *reinterpret_cast<const std::uint16_t*>(l + 14);
        }
        std::memcpy(leafbrushes, bsp + kOffLeafBrushes, static_cast<std::size_t>(n.leafbrushes) * sizeof(std::uint16_t));
        // cmodel_t (CMod_LoadSubmodels 0x33640, CM_InlineModel 0x2F0B0): 52 bytes, +36 headnode. Entry N is "*N".
        for (int i = 0; i < n.cmodels; ++i) {
            headnodes[i] = *reinterpret_cast<const int*>(bsp + kOffCModels + static_cast<std::size_t>(i) * 52 + 36);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Every brush below a head node, once, in index order.
void collect_model_brushes(const BspGeometry& g, int headnode, std::vector<std::uint8_t>& seen, std::vector<int>& out) {
    out.clear();
    std::vector<int> stack{ headnode };
    int visited = 0;
    while (!stack.empty() && visited < 200000) {
        const int idx = stack.back();
        stack.pop_back();
        ++visited;
        if (idx < 0) {
            const std::size_t leaf = static_cast<std::size_t>(~idx);
            if (leaf >= g.leafs.size()) continue;
            const RawLeaf& lf = g.leafs[leaf];
            for (std::size_t k = 0; k < lf.numleafbrushes; ++k) {
                const std::size_t lb = lf.firstleafbrush + k;
                if (lb >= g.leafbrushes.size()) break;
                const std::size_t b = g.leafbrushes[lb];
                if (b < g.brushes.size() && !seen[b]) {
                    seen[b] = 1;
                    out.push_back(static_cast<int>(b));
                }
            }
        } else if (static_cast<std::size_t>(idx) < g.nodes.size()) {
            stack.push_back(g.nodes[static_cast<std::size_t>(idx)].child0);
            stack.push_back(g.nodes[static_cast<std::size_t>(idx)].child1);
        }
    }
    for (int b : out) seen[static_cast<std::size_t>(b)] = 0;
    std::sort(out.begin(), out.end());
}

// The planes of a brush. False if it is no obstacle and shows no sky. `open`: no obstacle for sound as a whole (sky
// boxes, triggers, hints...: their nodraw back sides are none either).
bool brush_planes(const BspGeometry& g, int brush, std::vector<AcousticBrushPlane>& planes, bool& open, bool& sky) {
    const RawBrush& b = g.brushes[static_cast<std::size_t>(brush)];
    planes.clear();
    open = sky = false;
    if ((b.contents & (kContentsSolid | kContentsWindow)) == 0) return false;
    if (b.numsides < 4 || b.firstbrushside < 0 ||
        static_cast<std::size_t>(b.firstbrushside) + static_cast<std::size_t>(b.numsides) > g.side_planes.size()) return false;
    for (int s = 0; s < b.numsides; ++s) {
        const std::size_t si = static_cast<std::size_t>(b.firstbrushside + s);
        const std::uint8_t flags = g.side_flags[si];
        if ((flags & kSideValid) == 0) continue;
        if ((flags & kSideOpen) != 0) open = true;
        if (g.side_planes[si].sky) sky = true;
        planes.push_back(g.side_planes[si]);
    }
    return planes.size() >= 4 && (!open || sky);
}

// A see-through brush with glass on it. Grates and nets are see-through too, but of other materials.
bool brush_is_pane(const BspGeometry& g, int brush) {
    const RawBrush& b = g.brushes[static_cast<std::size_t>(brush)];
    if ((b.contents & kContentsWindow) == 0) return false;
    for (int s = 0; s < b.numsides; ++s) {
        const std::uint8_t flags = g.side_flags[static_cast<std::size_t>(b.firstbrushside + s)];
        if ((flags & kSideValid) != 0 && (flags & kSidePane) != 0) return true;
    }
    return false;
}

void build_acoustic_mesh(const BspGeometry& g, AcousticMeshData& out, int& world_brushes) {
    out.materials = default_acoustic_materials();
    out.map_name = g.map_name;
    out.vertices.reserve(32768);
    out.triangles.reserve(24576);
    out.material_indices.reserve(24576);
    world_brushes = 0;

    std::vector<std::uint8_t> seen(g.brushes.size(), 0);
    std::vector<int> brushes;
    std::vector<AcousticBrushPlane> planes;
    bool open = false, sky = false;
    for (std::size_t m = 0; m < g.headnodes.size(); ++m) {
        collect_model_brushes(g, g.headnodes[m], seen, brushes);
        AcousticSubModel model;
        model.model_index = static_cast<int>(m);
        for (int b : brushes) {
            if (!brush_planes(g, b, planes, open, sky)) continue;
            const int count = static_cast<int>(planes.size());
            if (open) {
                if (m == 0) clip_sky_brush(planes.data(), count, out);
            } else if (m == 0) {
                clip_brush_to_acoustic_mesh(planes.data(), count, out);
                if (brush_is_pane(g, b)) clip_glass_brush(planes.data(), count, out.glass_vertices, out.glass_triangles);
                ++world_brushes;
            } else {
                clip_brush_to_acoustic_mesh(planes.data(), count, model);
                if (brush_is_pane(g, b)) clip_glass_brush(planes.data(), count, model.glass_vertices, model.glass_triangles);
            }
        }
        if (!model.triangles.empty()) out.submodels.push_back(std::move(model));
    }
}

std::string mesh_summary(const char* source, const AcousticMeshData& mesh, int world_brushes) {
    char buf[320];
    std::snprintf(buf, sizeof(buf), "%s BSP '%s': %d solid brushes -> %u tris, %u brush models, %u sky tris",
                  source, mesh.map_name.c_str(), world_brushes, static_cast<unsigned>(mesh.triangles.size()),
                  static_cast<unsigned>(mesh.submodels.size()), static_cast<unsigned>(mesh.sky_triangles.size()));
    return buf;
}

} // namespace

std::vector<IPLMaterial> default_acoustic_materials() {
    std::vector<IPLMaterial> m(kMatCount);
    // { absorption[low, mid, high], scattering, transmission[low, mid, high] }
    // Tuned for moody VTMB urban interiors (clubs, sewers, alleys, apartments).
    m[kMatDefault]  = { { 0.12f, 0.08f, 0.06f }, 0.08f, { 0.22f, 0.12f, 0.05f } };  // plaster / drywall
    m[kMatConcrete] = { { 0.05f, 0.07f, 0.08f }, 0.05f, { 0.15f, 0.06f, 0.025f } }; // concrete / brick / sewer
    m[kMatWood]     = { { 0.15f, 0.11f, 0.10f }, 0.10f, { 0.36f, 0.20f, 0.08f } };  // wood / doors (bass bleeds!)
    m[kMatMetal]    = { { 0.04f, 0.05f, 0.07f }, 0.05f, { 0.20f, 0.10f, 0.05f } };  // metal / grates / vents
    m[kMatGlass]    = { { 0.06f, 0.04f, 0.03f }, 0.05f, { 0.45f, 0.35f, 0.22f } };  // glass / windows
    m[kMatCarpet]   = { { 0.24f, 0.69f, 0.73f }, 0.15f, { 0.30f, 0.14f, 0.05f } };  // carpet / cloth / dirt
    m[kMatTile]     = { { 0.02f, 0.03f, 0.04f }, 0.05f, { 0.15f, 0.07f, 0.03f } };  // ceramic tile / marble
    return m;
}

// VTMB materials are "<category>/<name>", and "maps/<map>/<category>/<name>_<x>_<y>_<z>" where the compiler baked a
// cubemap in. The category decides. Searching the whole path instead made every baked wall of "la_VENTruetower" a vent.
IPLint32 classify_surface_material(const char* s) {
    if (!s || !*s) return kMatDefault;
    const auto is_sep = [](char c) { return c == '/' || c == '\\'; };
    while (is_sep(*s)) ++s;
    if (_strnicmp(s, "materials", 9) == 0 && is_sep(s[9])) s += 10;
    if (_strnicmp(s, "maps", 4) == 0 && is_sep(s[4])) {
        const char* p = s + 5;
        while (*p && !is_sep(*p)) ++p;
        if (*p) s = p + 1;
    }
    std::size_t len = 0;
    while (s[len] && !is_sep(s[len])) ++len;
    if (s[len]) {
        struct Category { const char* name; IPLint32 mat; };
        static const Category kCategories[] = {
            { "plaster", kMatDefault },   { "concrete", kMatConcrete }, { "stone", kMatConcrete },
            { "brick", kMatConcrete },    { "asphalt", kMatConcrete },  { "building", kMatConcrete },
            { "wood", kMatWood },         { "metal", kMatMetal },       { "cable", kMatMetal },
            { "lights", kMatGlass },      { "glass", kMatGlass },       { "tile", kMatTile },
            { "carpet", kMatCarpet },     { "drapery", kMatCarpet },    { "cloth", kMatCarpet },
            { "grass", kMatCarpet },      { "dirt", kMatCarpet },       { "ground", kMatCarpet },
            { "blends", kMatCarpet },
        };
        for (const Category& c : kCategories) {
            if (std::strlen(c.name) != len || _strnicmp(s, c.name, len) != 0) continue;
            // A door or window painted on a plaster or brick texture is still a door or window.
            if (c.mat == kMatDefault || c.mat == kMatConcrete) {
                if (contains_ci(s + len, "window")) return kMatGlass;
                if (contains_ci(s + len, "door") && !contains_ci(s + len, "outdoor") && !contains_ci(s + len, "indoor")) {
                    return kMatWood;
                }
            }
            return c.mat;
        }
    }
    // Other folders (objects, models, effects, ...): by the words in the name.
    if (contains_ci(s, "glass") || contains_ci(s, "window")) return kMatGlass;
    if (contains_ci(s, "wood") || contains_ci(s, "door") || contains_ci(s, "plank") || contains_ci(s, "crate")) return kMatWood;
    if (contains_ci(s, "metal") || contains_ci(s, "steel") || contains_ci(s, "grate") || contains_ci(s, "cabinet") ||
        contains_ci(s, "oven") || contains_ci(s, "locker")) return kMatMetal;
    if (contains_ci(s, "carpet") || contains_ci(s, "cloth") || contains_ci(s, "dirt") || contains_ci(s, "grass") ||
        contains_ci(s, "foliage")) return kMatCarpet;
    if (contains_ci(s, "marble") || contains_ci(s, "ceramic") || contains_ci(s, "tile")) return kMatTile;
    if (contains_ci(s, "concrete") || contains_ci(s, "brick") || contains_ci(s, "stone") || contains_ci(s, "asphalt") ||
        contains_ci(s, "sewer") || contains_ci(s, "cement") || contains_ci(s, "rock")) return kMatConcrete;
    return kMatDefault;
}

bool surface_is_pane(const char* s) {
    return classify_surface_material(s) == kMatGlass && !contains_ci(s, "lights/") && !contains_ci(s, "lights\\");
}

bool surface_is_sky(const char* s, int surface_flags) {
    if ((surface_flags & (kSurfSky | kSurfSky2D)) != 0) return true;
    return s && (_strnicmp(s, "tools/toolsskybox", 17) == 0 || _strnicmp(s, "tools\\toolsskybox", 17) == 0);
}

bool surface_lets_sound_through(const char* s, int surface_flags) {
    if ((surface_flags & (kSurfSky | kSurfSky2D)) != 0) return true;
    if (!s) return false;
    // Tool textures only: "nodraw" is the unseen side of real walls and stays.
    if (_strnicmp(s, "tools/", 6) != 0 && _strnicmp(s, "tools\\", 6) != 0) return false;
    static const char* const kOpen[] = {
        "skybox", "trigger", "hint", "toolsskip", "areaportal", "occluder", "fog", "dust", "clip", "invisible",
        "novis", "visonly", "shadow", "blocklight", "stealth", "origin", "ladder",
    };
    for (const char* word : kOpen) {
        if (contains_ci(s, word)) return true;
    }
    return false;
}

namespace {

// Clips a convex brush; `sky_only` emits only the faces that show the sky. out_materials may be null.
void clip_brush(
    const AcousticBrushPlane* planes,
    int plane_count,
    bool sky_only,
    std::vector<IPLVector3>& out_vertices,
    std::vector<IPLTriangle>& out_triangles,
    std::vector<IPLint32>* out_materials)
{
    if (!planes || plane_count < 4) return;
    std::vector<Vec3d> poly;
    std::vector<IPLVector3> verts;
    poly.reserve(32);

    for (int i = 0; i < plane_count; ++i) {
        if (sky_only && !planes[i].sky) continue;
        make_base_winding(planes[i], poly);
        for (int j = 0; j < plane_count && poly.size() >= 3; ++j) {
            if (j == i) continue;
            clip_polygon(poly, planes[j]);
        }
        if (poly.size() < 3) continue;

        const IPLint32 base_vert = static_cast<IPLint32>(out_vertices.size());
        const IPLint32 mat_idx = (planes[i].material_index >= 0 && planes[i].material_index < kMatCount)
                                     ? planes[i].material_index
                                     : kMatDefault;
        // Steam Audio Y-up meters.
        verts.clear();
        for (const Vec3d& p : poly) {
            verts.push_back(source_pos_to_ipl(static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z)));
        }
        bool any_valid = false;
        for (std::size_t k = 1; k + 1 < verts.size(); ++k) {
            const IPLVector3 e1{ verts[k].x - verts[0].x, verts[k].y - verts[0].y, verts[k].z - verts[0].z };
            const IPLVector3 e2{ verts[k + 1].x - verts[0].x, verts[k + 1].y - verts[0].y, verts[k + 1].z - verts[0].z };
            const IPLVector3 c = ipl_cross(e1, e2);
            if (ipl_dot(c, c) < 1e-8f) continue; // degenerate sliver
            if (!any_valid) {
                out_vertices.insert(out_vertices.end(), verts.begin(), verts.end());
                any_valid = true;
            }
            IPLTriangle tri;
            tri.indices[0] = base_vert;
            tri.indices[1] = base_vert + static_cast<IPLint32>(k);
            tri.indices[2] = base_vert + static_cast<IPLint32>(k + 1);
            out_triangles.push_back(tri);
            if (out_materials) out_materials->push_back(mat_idx);
        }
    }
}

} // namespace

void clip_brush_to_acoustic_mesh(const AcousticBrushPlane* planes, int plane_count, AcousticMeshData& out_mesh) {
    clip_brush(planes, plane_count, false, out_mesh.vertices, out_mesh.triangles, &out_mesh.material_indices);
}

void clip_brush_to_acoustic_mesh(const AcousticBrushPlane* planes, int plane_count, AcousticSubModel& out_model) {
    clip_brush(planes, plane_count, false, out_model.vertices, out_model.triangles, &out_model.material_indices);
}

void clip_sky_brush(const AcousticBrushPlane* planes, int plane_count, AcousticMeshData& out_mesh) {
    clip_brush(planes, plane_count, true, out_mesh.sky_vertices, out_mesh.sky_triangles, nullptr);
}

void clip_glass_brush(const AcousticBrushPlane* planes, int plane_count, std::vector<IPLVector3>& vertices,
                      std::vector<IPLTriangle>& triangles) {
    clip_brush(planes, plane_count, false, vertices, triangles, nullptr);
}

bool extract_acoustic_mesh_from_engine(
    AcousticMeshData& out_mesh,
    std::string& out_summary)
{
    out_mesh.clear();
    out_summary.clear();

    HMODULE engine = GetModuleHandleW(L"engine.dll");
    if (!engine) {
        out_summary = "engine.dll not loaded";
        return false;
    }
    const auto* bsp = reinterpret_cast<const unsigned char*>(engine) + kRvaCollisionBspData;
    BspCounts n;
    if (!read_bsp_counts_seh(bsp, n)) {
        out_summary = "g_CollisionBSPData empty or unrecognized";
        return false;
    }

    BspGeometry g;
    g.map_name = n.map_name;
    g.brushes.resize(static_cast<std::size_t>(n.brushes));
    g.side_planes.resize(static_cast<std::size_t>(n.brushsides));
    g.side_flags.assign(static_cast<std::size_t>(n.brushsides), 0);
    g.nodes.resize(static_cast<std::size_t>(n.nodes));
    g.leafs.resize(static_cast<std::size_t>(n.leafs));
    g.leafbrushes.resize(static_cast<std::size_t>(n.leafbrushes));
    g.headnodes.resize(static_cast<std::size_t>(n.cmodels));
    if (!read_bsp_arrays_seh(bsp, n, g.brushes.data(), g.side_planes.data(), g.side_flags.data(), g.nodes.data(),
                             g.leafs.data(), g.leafbrushes.data(), g.headnodes.data())) {
        out_summary = "failed reading CCollisionBSPData arrays";
        return false;
    }

    int world_brushes = 0;
    build_acoustic_mesh(g, out_mesh, world_brushes);
    out_summary = mesh_summary("engine", out_mesh, world_brushes);
    return !out_mesh.empty();
}

bool extract_acoustic_mesh_from_bsp_file(
    const char* bsp_path,
    AcousticMeshData& out_mesh,
    std::string& out_summary)
{
    out_mesh.clear();
    out_summary.clear();
    if (!bsp_path || !*bsp_path) { out_summary = "empty bsp path"; return false; }

    std::FILE* f = nullptr;
    if (fopen_s(&f, bsp_path, "rb") != 0 || !f) { out_summary = std::string("cannot open ") + bsp_path; return false; }

    struct LumpEntry { std::int32_t fileofs, filelen, version; char fourcc[4]; };
    struct BspHeader { char ident[4]; std::int32_t version; LumpEntry lumps[64]; };
    BspHeader hdr = {};
    if (std::fread(&hdr, sizeof(hdr), 1, f) != 1 ||
        std::memcmp(hdr.ident, "VBSP", 4) != 0 || hdr.version != 17) {
        std::fclose(f);
        out_summary = std::string("not a VTMB BSP v17 file: ") + bsp_path;
        return false;
    }

    auto read_lump = [&](int idx, std::vector<std::uint8_t>& buf) -> bool {
        const LumpEntry& l = hdr.lumps[idx];
        if (l.fileofs < 0 || l.filelen <= 0 || l.filelen > 64 * 1024 * 1024) return false;
        buf.resize(static_cast<std::size_t>(l.filelen));
        if (std::fseek(f, l.fileofs, SEEK_SET) != 0) return false;
        return std::fread(buf.data(), 1, buf.size(), f) == buf.size();
    };

    std::vector<std::uint8_t> b_planes, b_nodes, b_leafs, b_models, b_leafbrushes, b_brushes, b_brushsides;
    std::vector<std::uint8_t> b_texdata, b_texinfo, b_strings, b_strtable;
    const bool ok =
        read_lump(1, b_planes) &&
        read_lump(5, b_nodes) &&
        read_lump(10, b_leafs) &&
        read_lump(14, b_models) &&
        read_lump(17, b_leafbrushes) &&
        read_lump(18, b_brushes) &&
        read_lump(19, b_brushsides);
    const bool have_textures =
        read_lump(2, b_texdata) && read_lump(6, b_texinfo) && read_lump(43, b_strings) && read_lump(44, b_strtable);
    std::fclose(f);
    if (have_textures) b_strings.push_back(0);
    // brush side texinfo (72 bytes: +64 flags, +68 texdata) -> texdata (32 bytes: +12 string id) -> name
    auto side_texture = [&](int texinfo, const char*& name, std::int32_t& flags) -> bool {
        if (!have_textures || texinfo < 0 || static_cast<std::size_t>(texinfo + 1) * 72 > b_texinfo.size()) return false;
        std::int32_t texdata = 0, id = 0, ofs = 0;
        std::memcpy(&flags,   b_texinfo.data() + static_cast<std::size_t>(texinfo) * 72 + 64, 4);
        std::memcpy(&texdata, b_texinfo.data() + static_cast<std::size_t>(texinfo) * 72 + 68, 4);
        if (texdata < 0 || static_cast<std::size_t>(texdata + 1) * 32 > b_texdata.size()) return false;
        std::memcpy(&id, b_texdata.data() + static_cast<std::size_t>(texdata) * 32 + 12, 4);
        if (id < 0 || static_cast<std::size_t>(id + 1) * 4 > b_strtable.size()) return false;
        std::memcpy(&ofs, b_strtable.data() + static_cast<std::size_t>(id) * 4, 4);
        if (ofs < 0 || static_cast<std::size_t>(ofs) >= b_strings.size()) return false;
        name = reinterpret_cast<const char*>(b_strings.data()) + ofs;
        return true;
    };
    if (!ok) {
        out_summary = std::string("missing required BSP lumps in ") + bsp_path;
        return false;
    }

    BspGeometry g;
    g.map_name = bsp_path;
    const std::size_t num_planes = b_planes.size() / 20;
    g.brushes.resize(b_brushes.size() / 12);
    for (std::size_t i = 0; i < g.brushes.size(); ++i) {
        std::memcpy(&g.brushes[i].firstbrushside, b_brushes.data() + i * 12 + 0, 4);
        std::memcpy(&g.brushes[i].numsides,       b_brushes.data() + i * 12 + 4, 4);
        std::memcpy(&g.brushes[i].contents,       b_brushes.data() + i * 12 + 8, 4);
    }
    g.side_planes.resize(b_brushsides.size() / 8);
    g.side_flags.assign(g.side_planes.size(), 0);
    for (std::size_t i = 0; i < g.side_planes.size(); ++i) {
        std::uint16_t pnum = 0;
        std::int16_t texinfo = -1, bevel = 0;
        std::memcpy(&pnum,    b_brushsides.data() + i * 8 + 0, 2);
        std::memcpy(&texinfo, b_brushsides.data() + i * 8 + 2, 2);
        std::memcpy(&bevel,   b_brushsides.data() + i * 8 + 6, 2);
        if (bevel != 0 || pnum >= num_planes) continue;
        float p[4];
        std::memcpy(p, b_planes.data() + static_cast<std::size_t>(pnum) * 20, 16);
        if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2]) || !std::isfinite(p[3])) continue; // as from engine.dll
        AcousticBrushPlane& plane = g.side_planes[i];
        plane = { p[0], p[1], p[2], p[3], kMatDefault };
        g.side_flags[i] = kSideValid;
        const char* name = nullptr;
        std::int32_t flags = 0;
        if (side_texture(texinfo, name, flags)) {
            plane.material_index = classify_surface_material(name);
            plane.sky = surface_is_sky(name, flags);
            if (surface_lets_sound_through(name, flags)) g.side_flags[i] |= kSideOpen;
            if (surface_is_pane(name)) g.side_flags[i] |= kSidePane;
        }
    }
    g.nodes.resize(b_nodes.size() / 32);
    for (std::size_t i = 0; i < g.nodes.size(); ++i) {
        std::memcpy(&g.nodes[i].child0, b_nodes.data() + i * 32 + 4, 4);
        std::memcpy(&g.nodes[i].child1, b_nodes.data() + i * 32 + 8, 4);
    }
    g.leafs.resize(b_leafs.size() / 32);
    for (std::size_t i = 0; i < g.leafs.size(); ++i) {
        std::memcpy(&g.leafs[i].firstleafbrush, b_leafs.data() + i * 32 + 24, 2);
        std::memcpy(&g.leafs[i].numleafbrushes, b_leafs.data() + i * 32 + 26, 2);
    }
    g.leafbrushes.resize(b_leafbrushes.size() / 2);
    if (!g.leafbrushes.empty()) std::memcpy(g.leafbrushes.data(), b_leafbrushes.data(), g.leafbrushes.size() * 2);
    g.headnodes.resize(b_models.size() / 48); // dmodel_t: +36 headnode
    for (std::size_t i = 0; i < g.headnodes.size(); ++i) {
        std::memcpy(&g.headnodes[i], b_models.data() + i * 48 + 36, 4);
    }
    if (num_planes == 0 || g.nodes.empty() || g.leafs.empty() || g.headnodes.empty() || g.brushes.empty() ||
        g.side_planes.empty()) {
        out_summary = "empty BSP lump counts";
        return false;
    }

    int world_brushes = 0;
    build_acoustic_mesh(g, out_mesh, world_brushes);
    out_summary = mesh_summary("file", out_mesh, world_brushes);
    return !out_mesh.empty();
}

bool extract_acoustic_mesh_for_map_name(
    const char* map_name,
    AcousticMeshData& out_mesh,
    std::string& out_summary)
{
    if (!map_name || !*map_name) {
        out_summary = "empty map name";
        return false;
    }
    char exe_path[MAX_PATH] = {};
    if (GetModuleFileNameA(nullptr, exe_path, MAX_PATH) == 0) {
        out_summary = "GetModuleFileNameA failed";
        return false;
    }
    std::string root(exe_path);
    const std::size_t slash = root.find_last_of("\\/");
    if (slash != std::string::npos) root.resize(slash);

    std::string base = map_name;
    if (base.size() > 4 && _stricmp(base.c_str() + base.size() - 4, ".bsp") == 0) {
        base.resize(base.size() - 4);
    }

    const char* dirs[] = { "Unofficial_Patch", "Vampire" };
    for (const char* d : dirs) {
        std::string candidate = root + "\\" + d + "\\maps\\" + base + ".bsp";
        if (GetFileAttributesA(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) {
            return extract_acoustic_mesh_from_bsp_file(candidate.c_str(), out_mesh, out_summary);
        }
    }
    out_summary = "BSP file not found on disk (likely packed in VPK)";
    return false;
}

} // namespace sa
