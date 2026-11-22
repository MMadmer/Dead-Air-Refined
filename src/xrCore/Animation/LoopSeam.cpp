// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#include "stdafx.h"
#pragma hdrstop

#include "LoopSeam.h"
#include "Motion.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace anim_loop
{
namespace
{
using V3 = std::array<double, 3>;
using Series = xr_vector<V3>;

// A seam kink below this many times the clip's own is left as it is.
constexpr double kink_limit = 2.0;
// The rebuilt window may curve this much harder than the clip's 90th percentile.
constexpr double window_curve_limit = 1.5;
// A clip whose end is farther from its start than this share of its swing is no loop.
constexpr double open_gap_share = 0.5;
// More of the moving channels than this open at the seam: a one-shot marked as a cycle.
constexpr double open_channels_share = 0.3;
// Curvature floors, below the 16-bit key step: radians of a rotation vector, metres.
constexpr double floor_rot = 2e-4, floor_pos = 2e-5;
constexpr u32 min_frames = 8;

double norm(const V3& v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

// Hamilton product and friends on (x, y, z, w); only ever used in matched pairs here, so the
// handedness of the engine's own quaternion code does not matter.
using Q4 = std::array<double, 4>;
Q4 qmul(const Q4& a, const Q4& b)
{
    return {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1], a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
        a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3], a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
}
Q4 qconj(const Q4& q) { return {-q[0], -q[1], -q[2], q[3]}; }
Q4 qnorm(Q4 q)
{
    const double n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    for (double& c : q)
        c /= n > 0.0 ? n : 1.0;
    return q;
}
V3 qlog(Q4 q)
{
    if (q[3] < 0.0)
        for (double& c : q)
            c = -c;
    const double s = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
    if (s < 1e-12)
        return {2.0 * q[0], 2.0 * q[1], 2.0 * q[2]};
    const double k = 2.0 * std::atan2(s, q[3]) / s;
    return {q[0] * k, q[1] * k, q[2] * k};
}
Q4 qexp(const V3& r)
{
    const double a = norm(r);
    if (a < 1e-12)
        return qnorm({r[0] * 0.5, r[1] * 0.5, r[2] * 0.5, 1.0});
    const double k = std::sin(a * 0.5) / a;
    return {r[0] * k, r[1] * k, r[2] * k, std::cos(a * 0.5)};
}
Q4 to_q4(const CKeyQR& k) { return qnorm({k.x * KEY_QuantI, k.y * KEY_QuantI, k.z * KEY_QuantI, k.w * KEY_QuantI}); }
CKeyQR to_key(const Q4& q)
{
    const auto c = [](double v) { return s16(clampr(iFloor(float(v * KEY_Quant) + 0.5f), -32767, 32767)); };
    return {c(q[0]), c(q[1]), c(q[2]), c(q[3])};
}

V3 second_diff(const Series& y, u32 i)
{
    const u32 n = u32(y.size());
    const V3& a = y[(i + n - 1) % n];
    const V3& b = y[i];
    const V3& c = y[(i + 1) % n];
    return {c[0] - 2.0 * b[0] + a[0], c[1] - 2.0 * b[1] + a[1], c[2] - 2.0 * b[2] + a[2]};
}

// The clip's own curvature (90th percentile away from the seam) and the seam's.
struct Kink
{
    double seam{}, own{};
    double ratio() const { return seam / own; }
};
Kink kink(const Series& y, double floor)
{
    const u32 n = u32(y.size());
    xr_vector<double> inner;
    for (u32 i = 3; i + 3 < n; ++i)
        inner.push_back(norm(second_diff(y, i)));
    double own = floor;
    if (!inner.empty())
    {
        const size_t at = size_t(double(inner.size() - 1) * 0.9);
        std::nth_element(inner.begin(), inner.begin() + at, inner.end());
        own = std::max(own, inner[at]);
    }
    return {std::max(norm(second_diff(y, 0)), norm(second_diff(y, n - 1))), own};
}

double swing(const Series& y)
{
    double s = 0.0;
    for (u32 c = 0; c < 3; ++c)
    {
        double lo = y[0][c], hi = y[0][c];
        for (const V3& v : y)
            lo = std::min(lo, v[c]), hi = std::max(hi, v[c]);
        s = std::max(s, hi - lo);
    }
    return s;
}

double gap(const Series& y)
{
    const V3& a = y.front();
    const V3& b = y.back();
    return norm({a[0] - b[0], a[1] - b[1], a[2] - b[2]});
}

// Burg's method: x[t] ~ sum coef[j] * x[t-1-j] for a mean-removed series.
xr_vector<double> burg(const xr_vector<double>& x, u32 order)
{
    const size_t n = x.size();
    xr_vector<double> f(x), b(x), a;
    for (u32 k = 0; k < order; ++k)
    {
        double num = 0.0, den = 0.0;
        for (size_t t = k + 1; t < n; ++t)
        {
            num += f[t] * b[t - 1];
            den += f[t] * f[t] + b[t - 1] * b[t - 1];
        }
        if (den < 1e-30)
            break;
        const double kr = -2.0 * num / den;
        xr_vector<double> next(a.size() + 1);
        for (size_t j = 0; j < a.size(); ++j)
            next[j] = a[j] + kr * a[a.size() - 1 - j];
        next[a.size()] = kr;
        a.swap(next);
        for (size_t t = n - 1; t > k; --t)
        {
            const double ft = f[t], bt = b[t - 1];
            f[t] = ft + kr * bt;
            b[t] = bt + kr * ft;
        }
    }
    for (double& c : a)
        c = -c;
    return a;
}

double lpc_step(const xr_vector<double>& h, size_t end, const xr_vector<double>& coef)
{
    double v = 0.0;
    for (size_t j = 0; j < coef.size(); ++j)
        v += coef[j] * h[end - 1 - j];
    return v;
}

// A parabola (a line with dim 2) through the last few samples (least squares), carried `steps`
// frames on.
void parabola(const xr_vector<double>& y, size_t end, u32 steps, double* out, int dim = 3)
{
    const size_t ctx = std::min<size_t>(6, end);
    double s[5]{}, r[3]{};
    for (size_t i = 0; i < ctx; ++i)
    {
        const double t = -double(ctx - 1 - i);
        const double v = y[end - ctx + i];
        double p = 1.0;
        for (int k = 0; k < 5; ++k, p *= t)
        {
            s[k] += p;
            if (k < 3)
                r[k] += v * p;
        }
    }
    // normal equations for c0 + c1 t + c2 t^2; a line when the points cannot hold a parabola
    double m[3][4] = {{s[0], s[1], s[2], r[0]}, {s[1], s[2], s[3], r[1]}, {s[2], s[3], s[4], r[2]}};
    if (ctx < 4)
        dim = 2;
    double c[3]{};
    for (int i = 0; i < dim; ++i)
    {
        int piv = i;
        for (int j = i + 1; j < dim; ++j)
            if (std::abs(m[j][i]) > std::abs(m[piv][i]))
                piv = j;
        for (int k = 0; k < 4; ++k)
            std::swap(m[i][k], m[piv][k]);
        if (std::abs(m[i][i]) < 1e-18)
            continue;
        for (int j = 0; j < dim; ++j)
        {
            if (j == i)
                continue;
            const double f = m[j][i] / m[i][i];
            for (int k = 0; k < 4; ++k)
                m[j][k] -= f * m[i][k];
        }
    }
    // the right-hand side is column 3 in either size; a line leaves c2 at zero
    for (int i = 0; i < dim; ++i)
        c[i] = std::abs(m[i][i]) < 1e-18 ? 0.0 : m[i][3] / m[i][i];
    for (u32 k = 1; k <= steps; ++k)
    {
        const double t = double(k);
        out[k - 1] = c[0] + c[1] * t + c[2] * t * t;
    }
}

// One channel's continuations: `fwd` the W frames after the end, `bwd` the W frames before the start.
void continuations(const xr_vector<double>& y, u32 W, double* fwd, double* bwd)
{
    const size_t P = y.size();
    double mean = 0.0, lo = y[0], hi = y[0];
    for (double v : y)
        mean += v, lo = std::min(lo, v), hi = std::max(hi, v);
    mean /= double(P);
    const double range = hi - lo;
    xr_vector<double> rev(y.rbegin(), y.rend());

    bool lpc = false;
    if (P >= 12 && range > 1e-9)
    {
        const u32 order = u32(clampr<size_t>(P / 3, 2, 12));
        xr_vector<double> x(P);
        for (size_t i = 0; i < P; ++i)
            x[i] = y[i] - mean;
        const xr_vector<double> coef = burg(x, order);
        if (!coef.empty())
        {
            // Trust the clip's own dynamics only where they predict it at least nearly as well as a
            // parabola does.
            double e_lpc = 0.0, e_par = 0.0;
            for (size_t t = std::max<size_t>(coef.size(), 4); t < P; ++t)
            {
                const double a = x[t] - lpc_step(x, t, coef);
                double p;
                parabola(x, t, 1, &p);
                const double b = x[t] - p;
                e_lpc += a * a;
                e_par += b * b;
            }
            if (e_lpc <= 1.5 * e_par)
            {
                xr_vector<double> h(x), hr(P);
                for (size_t i = 0; i < P; ++i)
                    hr[i] = x[P - 1 - i];
                lpc = true;
                for (u32 k = 0; k < W && lpc; ++k)
                {
                    const double f = lpc_step(h, h.size(), coef);
                    const double b = lpc_step(hr, hr.size(), coef);
                    h.push_back(f);
                    hr.push_back(b);
                    fwd[k] = f + mean;
                    bwd[W - 1 - k] = b + mean;
                    lpc = std::abs(f) < 1.5 * range && std::abs(b) < 1.5 * range;
                }
            }
        }
    }
    if (!lpc)
    {
        // a parabola carried a window on can run far off: then a line, and the clip's band at most
        const double lo_band = mean - 1.5 * range, hi_band = mean + 1.5 * range;
        const auto within = [&](const double* v) {
            for (u32 k = 0; k < W; ++k)
                if (v[k] < lo_band || v[k] > hi_band)
                    return false;
            return true;
        };
        xr_vector<double> back(W);
        for (int dim : {3, 2})
        {
            parabola(y, P, W, fwd, dim);
            parabola(rev, P, W, back.data(), dim);
            if (within(fwd) && within(back.data()))
                break;
        }
        for (u32 k = 0; k < W; ++k)
        {
            fwd[k] = clampr(fwd[k], lo_band, hi_band);
            bwd[W - 1 - k] = clampr(back[k], lo_band, hi_band);
        }
    }
}

// A one-shot marked as a cycle rests at one end and moves at the other; a loop moves on both
// sides of its seam, a hold there (an export plateau) aside. Speeds are taken where the motion
// leaves the hold on either side; the seam step itself (the jump being judged) counts for neither.
bool rests_on_one_side(const Series& y, double floor)
{
    const u32 P = u32(y.size());
    const auto step = [&](u32 i) {
        const V3& a = y[i];
        const V3& b = y[(i + 1) % P];
        return norm({b[0] - a[0], b[1] - a[1], b[2] - a[2]});
    };
    u32 back = P - 2, fwd = 0;
    while (back > P / 2 && step(back) <= floor)
        --back;
    while (fwd < P / 2 && step(fwd) <= floor)
        ++fwd;
    const double v_in = step(back), v_out = step(fwd);
    return std::min(v_in, v_out) < 0.25 * std::max(v_in, v_out) || std::max(v_in, v_out) <= floor;
}

double smootherstep(double u)
{
    u = clampr(u, 0.0, 1.0);
    return u * u * u * (u * (u * 6.0 - 15.0) + 10.0);
}

// Dense solve with partial pivoting; the systems here are at most 30 unknowns.
void solve(xr_vector<double>& a, xr_vector<double>& b, u32 n)
{
    for (u32 i = 0; i < n; ++i)
    {
        u32 piv = i;
        for (u32 j = i + 1; j < n; ++j)
            if (std::abs(a[j * n + i]) > std::abs(a[piv * n + i]))
                piv = j;
        if (piv != i)
        {
            for (u32 k = 0; k < n; ++k)
                std::swap(a[i * n + k], a[piv * n + k]);
            std::swap(b[i], b[piv]);
        }
        const double d = a[i * n + i];
        if (std::abs(d) < 1e-30)
            continue;
        for (u32 j = i + 1; j < n; ++j)
        {
            const double f = a[j * n + i] / d;
            if (f == 0.0)
                continue;
            for (u32 k = i; k < n; ++k)
                a[j * n + k] -= f * a[i * n + k];
            b[j] -= f * b[i];
        }
    }
    for (int i = int(n) - 1; i >= 0; --i)
    {
        double v = b[i];
        for (u32 k = u32(i) + 1; k < n; ++k)
            v -= a[i * n + k] * b[k];
        b[i] = std::abs(a[i * n + i]) < 1e-30 ? 0.0 : v / a[i * n + i];
    }
}

// The window's frames pulled toward a second-difference-smooth curve; two frames on each side
// beyond the window stay fixed, so position and speed come in from the original unchanged.
Series smooth_window(const Series& z, u32 W, double lambda)
{
    const u32 P = u32(z.size());
    const u32 n = 2 * W + 4, nf = 2 * W;
    xr_vector<u32> order(n);
    for (u32 i = 0; i < n; ++i)
        order[i] = u32((int(P) + int(i) - int(W) - 2) % int(P));
    // D2 rows over the sequence; free columns 2..n-3
    Series out(z);
    xr_vector<double> m(nf * nf), rhs(nf);
    for (u32 c = 0; c < 3; ++c)
    {
        std::fill(m.begin(), m.end(), 0.0);
        for (u32 i = 0; i < nf; ++i)
        {
            m[i * nf + i] = 1.0;
            rhs[i] = z[order[i + 2]][c];
        }
        for (u32 r = 0; r + 2 < n; ++r)
        {
            // row r covers sequence entries r, r+1, r+2 with weights 1, -2, 1
            const double w[3] = {1.0, -2.0, 1.0};
            double fixed = 0.0;
            int fi[3];
            for (int k = 0; k < 3; ++k)
            {
                const u32 s = r + u32(k);
                fi[k] = (s >= 2 && s < n - 2) ? int(s - 2) : -1;
                if (fi[k] < 0)
                    fixed += w[k] * z[order[s]][c];
            }
            for (int a = 0; a < 3; ++a)
            {
                if (fi[a] < 0)
                    continue;
                for (int b = 0; b < 3; ++b)
                    if (fi[b] >= 0)
                        m[fi[a] * nf + fi[b]] += lambda * w[a] * w[b];
                rhs[fi[a]] -= lambda * w[a] * fixed;
            }
        }
        solve(m, rhs, nf);
        for (u32 i = 0; i < nf; ++i)
            out[order[i + 2]][c] = rhs[i];
    }
    return out;
}

// The window's worst curvature; with `inner`, away from the two seam frames.
double window_peak(const Series& y, u32 W, bool inner = false)
{
    const u32 P = u32(y.size());
    double peak = 0.0;
    for (int s = -int(W) - 1; s <= int(W); ++s)
        if (!inner || (s != -1 && s != 0))
            peak = std::max(peak, norm(second_diff(y, u32((int(P) + s) % int(P)))));
    return peak;
}

// The seam of one channel rebuilt; returns false when the result is no better than the original.
bool rebuild(Series& y, double floor, Kink& before, Kink& after)
{
    const u32 P = u32(y.size());
    const u32 W = u32(clampr(iFloor(float(P) * 0.2f + 0.5f), 3, 15));
    if (2 * W + 4 > P)
        return false;
    xr_vector<double> chan(P), fwd(W), bwd(W);
    Series f(W), b(W);
    for (u32 c = 0; c < 3; ++c)
    {
        for (u32 i = 0; i < P; ++i)
            chan[i] = y[i][c];
        continuations(chan, W, fwd.data(), bwd.data());
        for (u32 k = 0; k < W; ++k)
            f[k][c] = fwd[k], b[k][c] = bwd[k];
    }
    Series z(y);
    for (int s = -int(W); s < int(W); ++s)
    {
        const double w = smootherstep((double(s) + double(W) + 0.5) / (2.0 * double(W)));
        for (u32 c = 0; c < 3; ++c)
        {
            if (s < 0)
                z[P + s][c] = (1.0 - w) * y[P + s][c] + w * b[W + s][c];
            else
                z[s][c] = (1.0 - w) * f[s][c] + w * y[s][c];
        }
    }
    Series best = z;
    for (double lambda : {1.0, 4.0, 16.0, 64.0})
    {
        if (window_peak(best, W) <= window_curve_limit * before.own)
            break;
        best = smooth_window(z, W, lambda);
    }
    // Better at the seam, and no new hump inside the window: its worst curvature no more than the
    // clip's own allowance, what the window already had away from the seam, or half the seam kink.
    after = kink(best, floor);
    const double allowed = std::max({window_curve_limit * before.own, window_peak(y, W, true), 0.5 * before.seam});
    if (after.seam >= before.seam || window_peak(best, W) > allowed)
        return false;
    y.swap(best);
    return true;
}
} // namespace

bool Enabled()
{
    static const bool on = !strstr(Core.Params, "-anim_raw_loops");
    return on;
}

Result Close(const xr_vector<RawTrack>& tracks, u32 count, u16 def_flags, xr_vector<BuiltTrack>& built)
{
    Result res;
    built.assign(tracks.size(), {});
    // A root mover carries the body forward on purpose and comes back by moving the object: its
    // seam is the design, not a defect.
    if ((def_flags & (esmStopAtEnd | esmFX | esmRootMover)) || count < min_frames + 1)
        return res;

    // Every key of the moving tracks, decoded: rotations as rotation vectors from the first key,
    // unwrapped along the clip, so a bone turning past half a turn stays one continuous curve.
    struct Chan
    {
        Series rot, pos;
        Q4 ref{};
    };
    xr_vector<Chan> chans(tracks.size());
    bool moving = false;
    for (size_t i = 0; i < tracks.size(); ++i)
    {
        const RawTrack& t = tracks[i];
        Chan& c = chans[i];
        if (!(t.flags & flRKeyAbsent))
        {
            moving = true;
            c.ref = to_q4(t.r[0]);
            const Q4 inv = qconj(c.ref);
            c.rot.resize(count);
            for (u32 k = 0; k < count; ++k)
            {
                V3 r = qlog(qmul(inv, to_q4(t.r[k])));
                const double a = norm(r);
                if (k && a > 1e-9)
                {
                    // the same rotation is every angle a + 2 pi m about the axis: the one nearest
                    // the previous key
                    const V3& prev = c.rot[k - 1];
                    V3 best = r;
                    double best_d = DBL_MAX;
                    for (int m = -3; m <= 3; ++m)
                    {
                        const double s = (a + double(PI_MUL_2) * m) / a;
                        const V3 v{r[0] * s, r[1] * s, r[2] * s};
                        const double d = norm({v[0] - prev[0], v[1] - prev[1], v[2] - prev[2]});
                        if (d < best_d)
                            best_d = d, best = v;
                    }
                    r = best;
                }
                c.rot[k] = r;
            }
        }
        if (t.flags & flTKeyPresent)
        {
            moving = true;
            c.pos.resize(count);
            for (u32 k = 0; k < count; ++k)
            {
                Fvector v;
                if (t.flags & flTKey16IsBit)
                {
                    const CKeyQT16& q = static_cast<const CKeyQT16*>(t.t)[k];
                    v.set(q.x1 * t.size_t_.x + t.init_t.x, q.y1 * t.size_t_.y + t.init_t.y, q.z1 * t.size_t_.z + t.init_t.z);
                }
                else
                {
                    const CKeyQT8& q = static_cast<const CKeyQT8*>(t.t)[k];
                    v.set(q.x1 * t.size_t_.x + t.init_t.x, q.y1 * t.size_t_.y + t.init_t.y, q.z1 * t.size_t_.z + t.init_t.z);
                }
                c.pos[k] = {v.x, v.y, v.z};
            }
        }
    }
    if (!moving)
        return res;

    // A loop comes back near where it began at all - a one-shot marked as a cycle does not, and
    // is left alone.
    const auto floor_of = [](const Chan& c, const Series* s) { return s == &c.rot ? floor_rot : floor_pos; };
    const auto closes = [](const Series& y) { return gap(y) <= open_gap_share * swing(y); };
    u32 swinging = 0, open = 0;
    for (const Chan& c : chans)
        for (const Series* s : {&c.rot, &c.pos})
        {
            if (s->empty() || swing(*s) < floor_of(c, s))
                continue;
            ++swinging;
            open += closes(*s) ? 0 : 1;
        }
    if (!swinging || double(open) > open_channels_share * double(swinging))
        return res;

    // The engine's cycle runs over count-1 frames and shows the last key at the instant time
    // turns back to the first, so the seam runs from key count-2 to key 0, and the last key of a
    // rebuilt channel is its first one. A clip that ends a step or more short of its start gets
    // there in a jump, a kink like any other.
    const u32 period = count - 1;
    for (size_t i = 0; i < tracks.size(); ++i)
    {
        Chan& c = chans[i];
        const RawTrack& t = tracks[i];
        BuiltTrack& out = built[i];
        const auto seam = [&](const Series& full, Series& y, double floor) {
            if (swing(y) < floor || !closes(full))
                return false;
            Kink before = kink(y, floor), after;
            if (before.ratio() <= kink_limit)
                return false;
            if (rests_on_one_side(y, floor))
            {
                ++res.resting;
                return false;
            }
            if (!rebuild(y, floor, before, after))
                return false;
            ++res.rebuilt;
            res.worst_before = std::max(res.worst_before, float(before.ratio()));
            res.worst_after = std::max(res.worst_after, float(after.ratio()));
            return true;
        };

        if (!c.rot.empty())
        {
            Series y(c.rot.begin(), c.rot.begin() + period);
            const Series original = y;
            if (seam(c.rot, y, floor_rot))
            {
                // the frames the window left alone keep their keys bit for bit
                out.rot = true;
                out.r.resize(count);
                for (u32 k = 0; k < period; ++k)
                    out.r[k] = y[k] == original[k] ? t.r[k] : to_key(qnorm(qmul(c.ref, qexp(y[k]))));
                out.r[period] = out.r[0];
            }
        }

        if (!c.pos.empty())
        {
            Series y(c.pos.begin(), c.pos.begin() + period);
            if (seam(c.pos, y, floor_pos))
            {
                out.pos = true;
                y.push_back(y[0]);
                Fvector lo{float(y[0][0]), float(y[0][1]), float(y[0][2])}, hi = lo;
                for (const V3& v : y)
                {
                    const Fvector f{float(v[0]), float(v[1]), float(v[2])};
                    lo.min(f);
                    hi.max(f);
                }
                out.init_t.add(lo, hi).mul(0.5f);
                out.size_t_.sub(hi, lo).mul(0.5f / 32767.f);
                for (u32 a = 0; a < 3; ++a)
                    if (out.size_t_[a] < 1e-9f)
                        out.size_t_[a] = 1e-9f;
                out.t.resize(count);
                for (u32 k = 0; k < count; ++k)
                {
                    const auto q = [&](u32 a) {
                        return s16(clampr(iFloor(float((y[k][a] - out.init_t[a]) / out.size_t_[a]) + 0.5f), -32767, 32767));
                    };
                    out.t[k] = {q(0), q(1), q(2)};
                }
            }
        }
    }
    return res;
}
} // namespace anim_loop
