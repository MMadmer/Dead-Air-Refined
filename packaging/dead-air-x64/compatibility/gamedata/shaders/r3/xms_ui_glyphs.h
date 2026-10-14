// XMS UI pictograms (UIShapeStyle.h, Kind::Trefoil and after): solid shapes from signed distances,
// in units of the glyph's half size with y up. Every function returns the distance to its outline,
// negative inside; xms_ui_shape.ps turns it into coverage at the screen's own resolution.
#ifndef XMS_UI_GLYPHS_H
#define XMS_UI_GLYPHS_H

float g_circle(float2 q, float2 c, float r) { return length(q - c) - r; }

float g_box(float2 q, float2 c, float2 b, float r)
{
	float2 d = abs(q - c) - b + r;
	return length(max(d, 0.0)) + min(max(d.x, d.y), 0.0) - r;
}

float g_seg(float2 q, float2 a, float2 b)
{
	float2 pa = q - a;
	float2 ba = b - a;
	float h = saturate(dot(pa, ba) / dot(ba, ba));
	return length(pa - ba * h);
}

float g_cap(float2 q, float2 a, float2 b, float r) { return g_seg(q, a, b) - r; }

float g_ellipse(float2 q, float2 c, float2 r)
{
	float2 p = q - c;
	float k0 = length(p / r);
	float k1 = length(p / (r * r));
	return k0 * (k0 - 1.0) / max(k1, 1e-5);
}

float g_smin(float a, float b, float k)
{
	float h = saturate(0.5 + 0.5 * (b - a) / k);
	return lerp(b, a, h) - k * h * (1.0 - h);
}

float g_mod(float a, float b) { return a - b * floor(a / b); }

float2 g_rot(float2 q, float a)
{
	float c = cos(a);
	float s = sin(a);
	return float2(c * q.x + s * q.y, -s * q.x + c * q.y);
}

// Inigo Quilez's polygon: the first n points of v, in order
float g_poly(float2 q, float2 v[6], int n)
{
	float d = dot(q - v[0], q - v[0]);
	float s = 1.0;
	int j = n - 1;
	[unroll] for (int i = 0; i < 6; ++i)
	{
		if (i < n)
		{
			float2 e = v[j] - v[i];
			float2 w = q - v[i];
			float2 b = w - e * saturate(dot(w, e) / dot(e, e));
			d = min(d, dot(b, b));
			bool3 c = bool3(q.y >= v[i].y, q.y < v[j].y, e.x * w.y > e.y * w.x);
			if (all(c) || all(!c))
				s = -s;
			j = i;
		}
	}
	return s * sqrt(d);
}

float g_tri(float2 q, float2 a, float2 b, float2 c)
{
	float2 v[6] = { a, b, c, c, c, c };
	return g_poly(q, v, 3);
}

float g_quad(float2 q, float2 a, float2 b, float2 c, float2 d)
{
	float2 v[6] = { a, b, c, d, d, d };
	return g_poly(q, v, 4);
}

// the band r0..r1 within `half_angle` of the direction `angle`
float g_ring_sector(float2 q, float r0, float r1, float angle, float half_angle)
{
	float r = length(q);
	float a = g_mod(atan2(q.y, q.x) - angle + 3.14159265, 6.2831853) - 3.14159265;
	float band = max(r0 - r, r - r1);
	float edge = r * sin(clamp(abs(a) - half_angle, -1.5707963, 1.5707963));
	return max(band, edge);
}

// -------------------------------------------------------------------------------------------------

float g_trefoil(float2 q)
{
	float d = g_circle(q, 0.0, 0.17);
	[unroll] for (int k = 0; k < 3; ++k)
		d = min(d, g_ring_sector(q, 0.3, 0.92, 1.5707963 + k * 2.0943951, 0.5235988));
	return d;
}

float g_grid(float2 q)
{
	float2 a = abs(q);
	return g_box(a, float2(0.42, 0.42), float2(0.32, 0.32), 0.1);
}

float g_pistol(float2 q)
{
	float slide = g_box(q, float2(-0.05, 0.42), float2(0.85, 0.2), 0.05);
	float grip = g_quad(q, float2(0.3, 0.3), float2(0.8, 0.3), float2(0.68, -0.85), float2(0.22, -0.85)) - 0.04;
	float guard = max(max(g_circle(q, float2(0.06, 0.22), 0.3), -g_circle(q, float2(0.06, 0.22), 0.18)), q.y - 0.22);
	return min(min(slide, grip), guard);
}

float g_helmet(float2 q)
{
	float dome = max(g_ellipse(q, float2(0.0, -0.15), float2(0.78, 0.82)), -(q.y + 0.15));
	float brim = g_box(q, float2(0.0, -0.22), float2(0.95, 0.09), 0.05);
	return min(dome, brim);
}

float g_cutlery(float2 q)
{
	float d = g_cap(q, float2(-0.42, -0.85), float2(-0.42, 0.05), 0.08);
	d = min(d, g_box(q, float2(-0.42, 0.12), float2(0.26, 0.12), 0.08));
	[unroll] for (int k = -1; k <= 1; ++k)
		d = min(d, g_cap(q, float2(-0.42 + 0.2 * k, 0.15), float2(-0.42 + 0.2 * k, 0.85), 0.055));
	d = min(d, g_cap(q, float2(0.42, -0.85), float2(0.42, -0.1), 0.09));
	float blade = max(max(g_ellipse(q, float2(0.32, 0.25), float2(0.24, 0.62)), q.x - 0.42), -(q.y + 0.1));
	return min(d, blade);
}

float g_medkit(float2 q)
{
	float body = g_box(q, float2(0.0, -0.05), float2(0.9, 0.72), 0.16);
	float handle = max(g_box(q, float2(0.0, 0.72), float2(0.36, 0.2), 0.08), -g_box(q, float2(0.0, 0.72), float2(0.22, 0.09), 0.0));
	float cross_ = min(g_box(q, float2(0.0, -0.05), float2(0.13, 0.46), 0.0), g_box(q, float2(0.0, -0.05), float2(0.46, 0.13), 0.0));
	return min(max(body, -cross_), handle);
}

float g_gem(float2 q)
{
	float crown = g_quad(q, float2(-0.55, 0.62), float2(0.55, 0.62), float2(0.95, 0.18), float2(-0.95, 0.18));
	float pavilion = g_tri(q, float2(-0.95, 0.18), float2(0.95, 0.18), float2(0.0, -0.9));
	float d = min(crown, pavilion);
	float facets = abs(q.y - 0.18) - 0.035;
	facets = min(facets, g_seg(q, float2(-0.25, 0.62), float2(-0.35, 0.18)) - 0.035);
	facets = min(facets, g_seg(q, float2(0.25, 0.62), float2(0.35, 0.18)) - 0.035);
	facets = min(facets, g_seg(q, float2(-0.35, 0.18), float2(0.0, -0.9)) - 0.035);
	facets = min(facets, g_seg(q, float2(0.35, 0.18), float2(0.0, -0.9)) - 0.035);
	return max(d, -max(facets, d + 0.08));
}

float g_gear(float2 q)
{
	float r = length(q);
	float a = g_mod(atan2(q.y, q.x) + 0.3926991, 0.7853982) - 0.3926991;
	float tooth = g_box(float2(r * cos(a), r * sin(a)), float2(0.78, 0.0), float2(0.18, 0.15), 0.05);
	return max(min(g_circle(q, 0.0, 0.66), tooth), -g_circle(q, 0.0, 0.26));
}

float g_sort(float2 q)
{
	float d = g_box(q, float2(-0.32, 0.55), float2(0.55, 0.11), 0.05);
	d = min(d, g_box(q, float2(-0.45, 0.05), float2(0.42, 0.11), 0.05));
	d = min(d, g_box(q, float2(-0.58, -0.45), float2(0.29, 0.11), 0.05));
	d = min(d, g_box(q, float2(0.62, 0.15), float2(0.1, 0.55), 0.03));
	return min(d, g_tri(q, float2(0.3, -0.35), float2(0.94, -0.35), float2(0.62, -0.85)));
}

float g_biohazard(float2 q)
{
	float d = 1e5;
	[unroll] for (int k = 0; k < 3; ++k)
	{
		float a = 1.5707963 + k * 2.0943951;
		float2 dir = float2(cos(a), sin(a));
		d = min(d, max(g_circle(q, 0.36 * dir, 0.5), -g_circle(q, 0.52 * dir, 0.34)));
	}
	d = max(d, -g_circle(q, 0.0, 0.13));
	d = min(d, max(g_circle(q, 0.0, 0.42), -g_circle(q, 0.0, 0.32)));
	return max(d, -g_circle(q, 0.0, 0.12));
}

float g_bolt(float2 q)
{
	float2 v[6] = { float2(0.18, 0.95), float2(-0.55, -0.08), float2(-0.05, -0.08), float2(-0.22, -0.95),
		float2(0.55, 0.12), float2(0.06, 0.12) };
	return g_poly(q, v, 6);
}

float g_flame(float2 q)
{
	float outer = g_smin(g_circle(q, float2(0.0, -0.32), 0.55),
		g_tri(q, float2(-0.48, -0.2), float2(0.05, 0.95), float2(0.5, -0.2)), 0.25);
	float inner = g_smin(g_circle(q, float2(0.02, -0.48), 0.24),
		g_tri(q, float2(-0.2, -0.42), float2(0.06, 0.1), float2(0.24, -0.42)), 0.1);
	return max(outer, -inner);
}

float g_psi(float2 q)
{
	float d = g_cap(q, float2(0.0, -0.9), float2(0.0, 0.88), 0.1);
	d = min(d, max(abs(length(q - float2(0.0, 0.3)) - 0.58) - 0.1, q.y - 0.3));
	d = min(d, g_cap(q, float2(-0.58, 0.3), float2(-0.58, 0.82), 0.1));
	d = min(d, g_cap(q, float2(0.58, 0.3), float2(0.58, 0.82), 0.1));
	return min(d, g_cap(q, float2(-0.32, -0.88), float2(0.32, -0.88), 0.1));
}

float g_claw(float2 q)
{
	float d = 1e5;
	[unroll] for (int k = 0; k < 3; ++k)
	{
		float ox = -0.5 + 0.5 * k;
		float lift = 0.1 * abs(k - 1);
		d = min(d, g_cap(q, float2(ox - 0.28, -0.85 + lift), float2(ox + 0.3, 0.85 - lift), 0.09));
	}
	return d;
}

float g_bullet(float2 q)
{
	float tip = max(g_ellipse(q, float2(0.0, 0.12), float2(0.34, 0.85)), -(q.y - 0.12));
	float d = min(min(tip, g_box(q, float2(0.0, -0.38), float2(0.34, 0.5), 0.04)), g_box(q, float2(0.0, -0.84), float2(0.4, 0.08), 0.03));
	return max(d, -g_box(q, float2(0.0, 0.05), float2(0.4, 0.035), 0.0));
}

// a hammer: the strike
float g_impact(float2 q)
{
	float d = g_box(q, float2(-0.1, 0.5), float2(0.62, 0.26), 0.07);
	d = min(d, g_box(q, float2(0.62, 0.5), float2(0.1, 0.32), 0.04));
	return min(d, g_cap(q, float2(-0.1, 0.3), float2(-0.1, -0.9), 0.12));
}

float g_burst(float2 q)
{
	float t = abs(frac(atan2(q.y, q.x) / 0.7853982) - 0.5) * 2.0;
	return (length(q) - (0.42 + 0.55 * pow(t, 1.6))) * 0.6;
}

float g_heart(float2 q)
{
	float2 p = float2(abs(q.x), q.y + 0.78) / 1.05;
	float d;
	if (p.x + p.y > 1.0)
		d = length(p - float2(0.25, 0.75)) - 0.3535534;
	else
	{
		float m = max(p.x + p.y, 0.0) * 0.5;
		d = sqrt(min(dot(p - float2(0.0, 1.0), p - float2(0.0, 1.0)), dot(p - m, p - m))) * sign(p.x - p.y);
	}
	return d * 1.05;
}

float g_drop(float2 q)
{
	return g_smin(g_circle(q, float2(0.0, -0.32), 0.56), g_tri(q, float2(-0.42, -0.1), float2(0.0, 0.95), float2(0.42, -0.1)), 0.18);
}

float g_runner(float2 q)
{
	float d = g_circle(q, float2(0.32, 0.72), 0.18);
	d = min(d, g_cap(q, float2(0.15, 0.4), float2(-0.08, -0.12), 0.12));
	d = min(d, g_cap(q, float2(0.12, 0.32), float2(0.5, 0.18), 0.08));
	d = min(d, g_cap(q, float2(0.5, 0.18), float2(0.62, 0.42), 0.08));
	d = min(d, g_cap(q, float2(0.12, 0.32), float2(-0.22, 0.4), 0.08));
	d = min(d, g_cap(q, float2(-0.22, 0.4), float2(-0.46, 0.18), 0.08));
	d = min(d, g_cap(q, float2(-0.08, -0.12), float2(0.3, -0.42), 0.1));
	d = min(d, g_cap(q, float2(0.3, -0.42), float2(0.22, -0.88), 0.1));
	d = min(d, g_cap(q, float2(-0.08, -0.12), float2(-0.38, -0.48), 0.1));
	return min(d, g_cap(q, float2(-0.38, -0.48), float2(-0.78, -0.5), 0.1));
}

float g_weight(float2 q)
{
	float bell = min(g_circle(q, float2(0.0, -0.25), 0.62), g_box(q, float2(0.0, -0.75), float2(0.48, 0.14), 0.06));
	float handle = max(g_box(q, float2(0.0, 0.5), float2(0.48, 0.4), 0.3), -g_box(q, float2(0.0, 0.47), float2(0.27, 0.23), 0.14));
	return min(bell, handle);
}

float g_clock(float2 q)
{
	float d = abs(length(q) - 0.78) - 0.11;
	d = min(d, g_cap(q, 0.0, float2(0.0, 0.5), 0.075));
	return min(d, g_cap(q, 0.0, float2(0.38, -0.12), 0.075));
}

float g_wrench(float2 q)
{
	float2 r = g_rot(q, 0.7853982);
	float d = g_cap(r, float2(0.0, -0.85), float2(0.0, 0.3), 0.14);
	d = min(d, max(g_circle(r, float2(0.0, 0.55), 0.36), -g_box(r, float2(0.0, 0.82), float2(0.14, 0.3), 0.0)));
	return min(d, max(g_circle(r, float2(0.0, -0.7), 0.28), -g_circle(r, float2(0.0, -0.7), 0.13)));
}

float g_crosshair(float2 q)
{
	float2 a = abs(q);
	float d = abs(length(q) - 0.62) - 0.09;
	d = min(d, g_box(a, float2(0.0, 0.78), float2(0.07, 0.22), 0.0));
	d = min(d, g_box(a, float2(0.78, 0.0), float2(0.22, 0.07), 0.0));
	return min(d, g_circle(q, 0.0, 0.12));
}

float g_hand(float2 q)
{
	float d = g_box(q, float2(0.02, -0.38), float2(0.5, 0.42), 0.22);
	d = min(d, g_cap(q, float2(-0.36, -0.1), float2(-0.36, 0.5), 0.11));
	d = min(d, g_cap(q, float2(-0.12, -0.1), float2(-0.12, 0.78), 0.11));
	d = min(d, g_cap(q, float2(0.12, -0.1), float2(0.12, 0.82), 0.11));
	d = min(d, g_cap(q, float2(0.36, -0.1), float2(0.36, 0.6), 0.11));
	return min(d, g_cap(q, float2(0.45, -0.45), float2(0.82, -0.05), 0.12));
}

float g_gauge(float2 q)
{
	float d = g_ring_sector(q + float2(0.0, 0.2), 0.63, 0.87, 1.5707963, 1.9477874);
	d = min(d, g_cap(q, float2(0.0, -0.2), float2(0.45, 0.32), 0.08));
	return min(d, g_circle(q, float2(0.0, -0.2), 0.16));
}

float g_shield(float2 q)
{
	float outer = min(g_box(q, float2(0.0, 0.38), float2(0.74, 0.5), 0.12),
		max(g_ellipse(q, float2(0.0, 0.12), float2(0.74, 1.0)), q.y - 0.12));
	// solid, with a thin band cut in from the edge
	return max(outer, -max(outer + 0.13, -(outer + 0.21)));
}

float g_cartridge(float2 q)
{
	float d = 1e5;
	[unroll] for (int k = -1; k <= 1; ++k)
	{
		float ox = 0.5 * k;
		float tip = max(g_ellipse(q, float2(ox, 0.3), float2(0.17, 0.6)), -(q.y - 0.3));
		float round_ = min(min(tip, g_box(q, float2(ox, -0.3), float2(0.17, 0.6), 0.03)), g_box(q, float2(ox, -0.86), float2(0.2, 0.06), 0.02));
		d = min(d, max(round_, -g_box(q, float2(ox, 0.24), float2(0.25, 0.03), 0.0)));
	}
	return d;
}

float g_moon(float2 q) { return max(g_circle(q, 0.0, 0.82), -g_circle(q, float2(0.38, 0.28), 0.66)); }

float g_upgrade(float2 q)
{
	float2 a[6] = { float2(-0.8, -0.05), float2(0.0, 0.75), float2(0.8, -0.05), float2(0.5, -0.05), float2(0.0, 0.43), float2(-0.5, -0.05) };
	float2 b[6] = { float2(-0.8, -0.7), float2(0.0, 0.1), float2(0.8, -0.7), float2(0.5, -0.7), float2(0.0, -0.22), float2(-0.5, -0.7) };
	return min(g_poly(q, a, 6), g_poly(q, b, 6));
}

float g_pin(float2 q)
{
	float d = g_smin(g_circle(q, float2(0.0, 0.3), 0.55), g_tri(q, float2(-0.47, 0.02), float2(0.0, -0.95), float2(0.47, 0.02)), 0.08);
	return max(d, -g_circle(q, float2(0.0, 0.3), 0.22));
}

float g_keyboard(float2 q)
{
	float d = g_box(q, float2(0.0, 0.0), float2(0.95, 0.62), 0.14);
	float keys = g_box(q, float2(0.0, -0.33), float2(0.42, 0.085), 0.03);
	keys = min(keys, g_box(q, float2(-0.72, -0.33), float2(0.1, 0.085), 0.03));
	keys = min(keys, g_box(q, float2(0.72, -0.33), float2(0.1, 0.085), 0.03));
	[unroll] for (int k = 0; k < 6; ++k)
	{
		keys = min(keys, g_box(q, float2(-0.72 + 0.288 * k, 0.33), float2(0.1, 0.1), 0.03));
		keys = min(keys, g_box(q, float2(-0.72 + 0.288 * k, 0.02), float2(0.1, 0.1), 0.03));
	}
	return max(d, -keys);
}

float g_scales(float2 q)
{
	// the post, the beam, the pivot and the foot
	float d = g_cap(q, float2(0.0, -0.72), float2(0.0, 0.6), 0.07);
	d = min(d, g_cap(q, float2(-0.66, 0.5), float2(0.66, 0.5), 0.07));
	d = min(d, g_circle(q, float2(0.0, 0.66), 0.12));
	d = min(d, g_box(q, float2(0.0, -0.8), float2(0.4, 0.08), 0.04));
	// a pan on strings under either end of the beam
	[unroll] for (int k = -1; k <= 1; k += 2)
	{
		float x = 0.6 * k;
		d = min(d, g_cap(q, float2(x, 0.5), float2(x - 0.26, -0.04), 0.035));
		d = min(d, g_cap(q, float2(x, 0.5), float2(x + 0.26, -0.04), 0.035));
		d = min(d, max(g_circle(q, float2(x, -0.04), 0.32), q.y + 0.04));
	}
	return d;
}

// kind: SUIShapeStyle::Kind, Trefoil (7) and after
float sd_glyph(float kind, float2 q)
{
	int k = int(kind + 0.5);
	switch (k)
	{
	case 7: return g_trefoil(q);
	case 8: return g_grid(q);
	case 9: return g_pistol(q);
	case 10: return g_helmet(q);
	case 11: return g_cutlery(q);
	case 12: return g_medkit(q);
	case 13: return g_gem(q);
	case 14: return g_gear(q);
	case 15: return g_sort(q);
	case 16: return g_biohazard(q);
	case 17: return g_bolt(q);
	case 18: return g_flame(q);
	case 19: return g_psi(q);
	case 20: return g_claw(q);
	case 21: return g_bullet(q);
	case 22: return g_impact(q);
	case 23: return g_burst(q);
	case 24: return g_heart(q);
	case 25: return g_drop(q);
	case 26: return g_runner(q);
	case 27: return g_weight(q);
	case 28: return g_clock(q);
	case 29: return g_wrench(q);
	case 30: return g_crosshair(q);
	case 31: return g_hand(q);
	case 32: return g_gauge(q);
	case 33: return g_shield(q);
	case 34: return g_cartridge(q);
	case 35: return g_moon(q);
	case 36: return g_upgrade(q);
	case 37: return g_pin(q);
	case 38: return g_keyboard(q);
	default: return g_scales(q);
	}
}

#endif
