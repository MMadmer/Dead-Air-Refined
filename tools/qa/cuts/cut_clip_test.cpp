// The exact cut's clip (src/xrCore/XMS/xms_cut_clip.cpp) without the game: tools\qa\cuts\Run-CutClipQa.ps1.
// The authoring tool's preview checks its own implementation against the same cases and numbers.

#include "xrCore/XMS/xms_cut_clip.h"

#include <cmath>
#include <cstdio>

namespace
{
int failures = 0;

void expect(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failures += ok ? 0 : 1;
}

// A ground region at the origin: 20 x 8 m turned a quarter about +Y, so it spans |x| <= 4, |z| <= 10 in the
// world; the old ground is flat at 0 and the band is half a metre.
const float flat[4] = {0.f, 0.f, 0.f, 0.f};
const XMS::Cut::Region ground{1, {0.f, 0.f, 0.f}, {10.f, 5.f, 4.f}, 1.5707964f, 0.5f, 2, 2, flat};

struct Tri
{
    float a[3], b[3], c[3];
};

// The point of a result in the world, from its weights.
void place(const Tri& t, const float w[3], float out[3])
{
    for (int k = 0; k < 3; ++k)
        out[k] = t.a[k] * w[0] + t.b[k] * w[1] + t.c[k] * w[2];
}

float area_y(const float p[3], const float q[3], const float r[3])
{
    return 0.5f * ((q[2] - p[2]) * (r[0] - p[0]) - (q[0] - p[0]) * (r[2] - p[2]));
}
} // namespace

int main()
{
    XMS::Cut::Result out;

    // 1. Straddling the rectangle's edge x = 4: the part beyond it stays, one triangle of 16/3 m2 with the winding.
    {
        const Tri t{{2.f, 0.f, -2.f}, {8.f, 0.f, 0.f}, {2.f, 0.f, 2.f}};
        const bool replaced = XMS::Cut::ClipTriangle(ground, t.a, t.b, t.c, out);
        expect(replaced && out.triangles == 1, "straddling: replaced by one triangle");
        if (replaced && out.triangles == 1)
        {
            float p[3][3];
            bool beyond = true;
            for (int k = 0; k < 3; ++k)
            {
                place(t, out.weights[out.indices[0][k]], p[k]);
                beyond = beyond && p[k][0] >= 4.f - 1e-5f;
            }
            const float area = area_y(p[0], p[1], p[2]);
            const float original = area_y(t.a, t.b, t.c);
            expect(beyond, "straddling: every point at x >= 4");
            expect(std::fabs(std::fabs(area) - 16.f / 3.f) < 1e-4f, "straddling: area 16/3");
            expect((area > 0.f) == (original > 0.f), "straddling: winding kept");
        }
    }

    // 2. Inside and on the old ground (0.2 m above it): gone.
    {
        const Tri t{{-1.f, 0.2f, -1.f}, {1.f, 0.2f, -1.f}, {-1.f, 0.2f, 1.f}};
        const bool replaced = XMS::Cut::ClipTriangle(ground, t.a, t.b, t.c, out);
        expect(replaced && out.triangles == 0, "inside: removed");
    }

    // 3. Outside the rectangle: kept whole.
    {
        const Tri t{{20.f, 0.f, 20.f}, {22.f, 0.f, 20.f}, {20.f, 0.f, 22.f}};
        expect(!XMS::Cut::ClipTriangle(ground, t.a, t.b, t.c, out), "outside: kept whole");
    }

    // 4. Inside but 2 m above the old ground, off its band: kept whole.
    {
        const Tri t{{-1.f, 2.f, -1.f}, {1.f, 2.f, -1.f}, {-1.f, 2.f, 1.f}};
        expect(!XMS::Cut::ClipTriangle(ground, t.a, t.b, t.c, out), "off the band: kept whole");
    }

    std::printf(failures ? "%d case(s) failed\n" : "all cases passed\n", failures);
    return failures ? 1 : 0;
}
