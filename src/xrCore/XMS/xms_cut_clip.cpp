// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#include "xms_cut_clip.h"

#include <cmath>
#include <cstddef>

namespace XMS::Cut
{
namespace
{
constexpr float on_line = 1e-5f; // metres: a point this near a line is inside
constexpr float min_area = 1e-8f; // square metres
constexpr int max_polygon = 12;

// x and z are the region's local axes, y stays the world height; w are the triangle's weights.
struct Point
{
    float x, y, z;
    float w[3];
};

struct Polygon
{
    int count;
    Point p[max_polygon];
};

Point lerp(const Point& a, const Point& b, float t)
{
    Point m;
    m.x = a.x + (b.x - a.x) * t;
    m.y = a.y + (b.y - a.y) * t;
    m.z = a.z + (b.z - a.z) * t;
    for (int k = 0; k < 3; ++k)
        m.w[k] = a.w[k] + (b.w[k] - a.w[k]) * t;
    return m;
}

bool push(Polygon& poly, const Point& p)
{
    if (poly.count == max_polygon)
        return false;
    poly.p[poly.count++] = p;
    return true;
}

// Splits `poly` by the line where `bound + sign * coordinate` is zero: the part on the side
// where it is not negative (within on_line) stays in `poly`, the rest goes to `outside`.
bool split(Polygon& poly, bool along_z, float sign, float bound, Polygon& outside)
{
    Polygon inside{};
    outside.count = 0;
    for (int i = 0; i < poly.count; ++i)
    {
        const Point& a = poly.p[i];
        const Point& b = poly.p[(i + 1) % poly.count];
        const float da = bound + sign * (along_z ? a.z : a.x);
        const float db = bound + sign * (along_z ? b.z : b.x);
        const bool a_in = da >= -on_line;
        const bool b_in = db >= -on_line;
        if (!push(a_in ? inside : outside, a))
            return false;
        if (a_in != b_in)
        {
            float t = da / (da - db);
            t = t < 0.f ? 0.f : t > 1.f ? 1.f : t;
            const Point m = lerp(a, b, t);
            if (!push(inside, m) || !push(outside, m))
                return false;
        }
    }
    poly = inside;
    return true;
}

float area(const Polygon& poly)
{
    float nx = 0.f, ny = 0.f, nz = 0.f;
    const Point& o = poly.p[0];
    for (int i = 1; i + 1 < poly.count; ++i)
    {
        const float ax = poly.p[i].x - o.x, ay = poly.p[i].y - o.y, az = poly.p[i].z - o.z;
        const float bx = poly.p[i + 1].x - o.x, by = poly.p[i + 1].y - o.y, bz = poly.p[i + 1].z - o.z;
        nx += ay * bz - az * by;
        ny += az * bx - ax * bz;
        nz += ax * by - ay * bx;
    }
    return 0.5f * std::sqrt(nx * nx + ny * ny + nz * nz);
}

// The old ground's height at a local point: bilinear over the grid, clamped to its border.
float ground(const Region& r, float x, float z)
{
    float u = (x + r.half[0]) / (2.f * r.half[0]) * float(r.columns - 1);
    float v = (z + r.half[2]) / (2.f * r.half[2]) * float(r.rows - 1);
    u = u < 0.f ? 0.f : u > float(r.columns - 1) ? float(r.columns - 1) : u;
    v = v < 0.f ? 0.f : v > float(r.rows - 1) ? float(r.rows - 1) : v;
    const unsigned i = unsigned(u) < r.columns - 2 ? unsigned(u) : r.columns - 2;
    const unsigned j = unsigned(v) < r.rows - 2 ? unsigned(v) : r.rows - 2;
    const float fu = u - float(i), fv = v - float(j);
    const float* row0 = r.heights + size_t(j) * r.columns;
    const float* row1 = row0 + r.columns;
    return (row0[i] * (1.f - fu) + row0[i + 1] * fu) * (1.f - fv) + (row1[i] * (1.f - fu) + row1[i + 1] * fu) * fv;
}

bool removed(const Region& r, const Polygon& poly)
{
    for (int i = 0; i < poly.count; ++i)
    {
        const Point& p = poly.p[i];
        if (r.kind == 0 ? p.y < r.centre[1] - r.half[1] || p.y > r.centre[1] + r.half[1]
                        : std::fabs(p.y - ground(r, p.x, p.z)) > r.tolerance)
            return false;
    }
    return true;
}

int point_of(Result& out, const Point& p)
{
    for (int i = 0; i < out.points; ++i)
        if (out.weights[i][0] == p.w[0] && out.weights[i][1] == p.w[1] && out.weights[i][2] == p.w[2])
            return i;
    if (out.points == max_points)
        return -1;
    for (int k = 0; k < 3; ++k)
        out.weights[out.points][k] = p.w[k];
    return out.points++;
}
} // namespace

bool ClipTriangle(const Region& region, const float a[3], const float b[3], const float c[3], Result& out)
{
    out.points = 0;
    out.triangles = 0;
    const float hx = region.half[0], hz = region.half[2];
    if (!(hx > 0.f && hz > 0.f) || (region.kind == 1 && (region.columns < 2 || region.rows < 2 || !region.heights)) ||
        (region.kind != 0 && region.kind != 1))
        return false;

    // Local coordinates: p - centre turned by -yaw about +Y.
    const float cs = std::cos(region.yaw), sn = std::sin(region.yaw);
    const float* corners[3] = {a, b, c};
    Polygon poly{};
    poly.count = 3;
    for (int i = 0; i < 3; ++i)
    {
        const float dx = corners[i][0] - region.centre[0], dz = corners[i][2] - region.centre[2];
        Point& p = poly.p[i];
        p.x = dx * cs - dz * sn;
        p.y = corners[i][1];
        p.z = dx * sn + dz * cs;
        p.w[0] = p.w[1] = p.w[2] = 0.f;
        p.w[i] = 1.f;
    }

    const float min_x = std::fmin(poly.p[0].x, std::fmin(poly.p[1].x, poly.p[2].x));
    const float max_x = std::fmax(poly.p[0].x, std::fmax(poly.p[1].x, poly.p[2].x));
    const float min_z = std::fmin(poly.p[0].z, std::fmin(poly.p[1].z, poly.p[2].z));
    const float max_z = std::fmax(poly.p[0].z, std::fmax(poly.p[1].z, poly.p[2].z));
    if (max_x < -hx || min_x > hx || max_z < -hz || min_z > hz)
        return false;

    // x = -hx, x = +hx, z = -hz, z = +hz: what lies beyond each line is kept, `poly` ends inside.
    Polygon kept[4];
    if (!split(poly, false, 1.f, hx, kept[0]) || !split(poly, false, -1.f, hx, kept[1]) ||
        !split(poly, true, 1.f, hz, kept[2]) || !split(poly, true, -1.f, hz, kept[3]))
        return false;
    if (poly.count < 3 || area(poly) < min_area || !removed(region, poly))
        return false;

    for (const Polygon& k : kept)
    {
        if (k.count < 3 || area(k) < min_area)
            continue;
        for (int i = 1; i + 1 < k.count; ++i)
        {
            if (out.triangles == max_triangles)
                return false;
            const int ids[3] = {point_of(out, k.p[0]), point_of(out, k.p[i]), point_of(out, k.p[i + 1])};
            if (ids[0] < 0 || ids[1] < 0 || ids[2] < 0)
                return false;
            for (int v = 0; v < 3; ++v)
                out.indices[out.triangles][v] = (unsigned char)ids[v];
            ++out.triangles;
        }
    }
    return true;
}
} // namespace XMS::Cut
