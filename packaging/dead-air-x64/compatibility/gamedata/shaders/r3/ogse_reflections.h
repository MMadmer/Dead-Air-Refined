#ifndef OGSE_REFLECTIONS_H
#define OGSE_REFLECTIONS_H
// [DA_PORT] Water/puddle SSR. The ray-march is ported from IX-Ray 1.6 (STCoP)
// gamedata/shaders/d3d11/reflections.hlsli (FastViewReflectionsSSR + BinaryRefinementHUD):
// the ray start and end are projected to screen space, the stride is clipped to the screen
// edge so a fixed number of samples always covers everything the screen can reflect, a hit
// is then bisected (binary refinement) and confirmed with a relative thickness test.
//
// The old OGSE march stepped a fixed 1/256 of the screen SSR_MAX_IT times - at the default
// quality it reached only ~62% of the screen (160/256), and it interpolated view-space z
// LINEARLY along the screen ray, which is wrong under perspective.
//
// Depth semantics differ from the donor and are adapted here:
//   donor: s_position.x holds hardware depth P, linearized as depth_unpack.x/(P-depth_unpack.y);
//          the march compares NDC z against the raw P.
//   ours:  s_position.z holds linear VIEW-SPACE z (GBUFFER_OPTIMIZATION packing - see
//          common_functions.h, gbuffer_load_data). NDC z is affine in 1/z, so marching
//          q = 1/z linearly along the screen segment IS the donor's NDC-z march,
//          perspective-correct by construction. No m_P and no depth_unpack needed.
//
// Reflection quality, set from the "r3_water_refl" console/video setting (SSR_QUALITY 1..4).
// SSR_STEPS is the number of linear samples the edge-clipped ray is split into (the donor
// uses 30 = SSLR_STEPS; that is our default-quality value). REFL_RANGE still cuts
// reflections off past that eye depth and fades them to white before it.
// SSR_MAX_IT is kept defined only so any older includer keeps compiling; the new march
// does not use it.
#ifndef SSR_QUALITY
#  define SSR_QUALITY 3
#endif
#if SSR_QUALITY <= 1
#  define SSR_MAX_IT 64
#  define REFL_RANGE 100
#  define SSR_STEPS  16
#elif SSR_QUALITY == 2
#  define SSR_MAX_IT 110
#  define REFL_RANGE 140
#  define SSR_STEPS  24
#elif SSR_QUALITY == 3
#  define SSR_MAX_IT 160
#  define REFL_RANGE 180
#  define SSR_STEPS  30
#else
#  define SSR_MAX_IT 240
#  define REFL_RANGE 220
#  define SSR_STEPS  48
#endif
#define SKY_EPS float(0.001)

// Donor constants (IX-Ray FastViewReflectionsSSR / BinaryRefinementHUD).
#define SSR_REFINE_STEPS	5		// MAX_FIND_STEP: bisection iterations after the coarse hit
#define SSR_THICKNESS_REL	0.01	// relative |dz|/z acceptance of the refined hit
#define SSR_Z_NEAR_LIMIT	0.2		// metres: never march the ray closer to the camera than this

uniform float4		screen_res;

static const float2 resolution = screen_res.xy;
static const float2 inv_resolution = screen_res.zw;

float	get_depth_fast			(float2 tc)
{
#ifndef USE_MSAA
	return s_position.Sample( smp_nofilter, tc).z;
#else
	return s_position.Load( int3( tc * pos_decompression_params2.xy ,0),0 ).z;
#endif
}

half is_sky(float depth)		{return step(depth, SKY_EPS);}
half is_not_sky(float depth)	{return step(SKY_EPS, depth);}

TextureCube	s_env0;
TextureCube	s_env1;

// [DA_PORT] Per-pixel dither ("r3_water_refl_jitter"). The donor jitters every stride by
// +-20% with a TIME-VARYING hash and averages the noise out in its temporal pass. We have
// no temporal accumulation, so the hash stays a function of screen position only - a
// time-varying one would shimmer. Varying the hash input per STEP (golden-ratio offset)
// still decorrelates the strides enough to break residual banding into fine static noise.
// SSR_JITTER_AMP feeds ssr_march_core: 0 = uniform strides, 1 = donor's 0.8..1.2 range.
float ssr_jitter_hash(float2 tc)
{
	return frac(sin(dot(tc, float2(12.9898, 78.233))) * 43758.5453);
}
#ifdef SSR_JITTER
#  define SSR_JITTER_AMP 1.0
#else
#  define SSR_JITTER_AMP 0.0
#endif

// [DA_PORT] The IX-Ray march core, adapted to our linear-z G-buffer.
//   uv0/z0 - screen uv and view-space z of the ray start (the reflecting pixel);
//   uv1/z1 - the same for a point ~1 m along the reflected ray, already near-clipped by
//            the caller so z1 > 0 (a segment end behind the camera projects flipped);
//   jitter_amp - 0 disables the stride jitter (the puddle pass wants it off, see
//            da_puddle_refl.ps); water passes SSR_JITTER_AMP.
// Returns float3(hit_uv, valid): valid is 1 when the bisected hit passed the thickness test.
//
// The segment is parametrized by t: uv(t) linear, q(t) = 1/z linear - the standard
// perspective-correct screen-space identity (this is what makes it equal to the donor's
// NDC-z march). t is then extended past the segment end up to the screen border (the
// donor's GetMaxDirLength over the NDC unit box) and bounded in depth: no farther than
// REFL_RANGE, no closer than SSR_Z_NEAR_LIMIT.
float3 ssr_march_core(float2 uv0, float z0, float2 uv1, float z1, float jitter_amp)
{
	float3 P0   = float3(uv0, rcp(z0));
	float3 dirv = float3(uv1 - uv0, rcp(z1) - P0.z);

	// degenerate segment (start == end): nothing to march
	if (max(max(abs(dirv.x), abs(dirv.y)), abs(dirv.z)) < 1e-7)
		return float3(uv0, 0.0);

	// screen-edge clip: smallest positive t at which uv leaves [0,1]^2. Per axis take the
	// boundary that lies ahead of the direction of travel; rcp(0) = +inf drops out of min.
	float2 rd   = rcp(dirv.xy);
	float2 tbm  = max((0.0 - uv0) * rd, (1.0 - uv0) * rd);
	float t_max = min(tbm.x, tbm.y);

	// depth caps: REFL_RANGE far (past it the caller fades to the envmap anyway) and
	// SSR_Z_NEAR_LIMIT near (q = 1/z must stay positive; the near cap adapts when the
	// start itself is already closer than the limit)
	if (dirv.z < 0.0)			// q falls: the ray runs away from the camera
		t_max = min(t_max, (rcp(float(REFL_RANGE)) - P0.z) / dirv.z);
	else if (dirv.z > 0.0)		// q rises: the ray comes toward the camera
		t_max = min(t_max, (rcp(min(float(SSR_Z_NEAR_LIMIT), z0 * 0.5)) - P0.z) / dirv.z);

	if (t_max <= 0.0)
		return float3(uv0, 0.0);

	// uniform stride: SSR_STEPS samples cover the whole clipped ray (donor scheme)
	float t_step = t_max / float(SSR_STEPS + 1);
	float t = 0.0;

	// [DA_PORT] [loop] ОБЯЗАТЕЛЕН. Граница цикла - константа (SSR_STEPS), поэтому без
	// атрибута компилятор разворачивает цикл целиком - до 48 копий тела на максимальном
	// качестве, каждая с выборкой текстуры. На таком развороте D3DCompile не выдаёт
	// ошибку, а ПАДАЕТ, унося процесс без стека и без сообщения: игра просто исчезала на
	// загрузке любого уровня с водой, а лог обрывался на имени шейдера. Цикл и так
	// динамический - из него выходят по попаданию, - так что разворачивать его незачем.
	[loop]
	for (int i = 0; i < SSR_STEPS; ++i)
	{
		float stride = t_step * lerp(1.0, lerp(0.8, 1.2, ssr_jitter_hash(uv0 + float(i) * 0.618034)), jitter_amp);
		t += stride;

		float3 P     = P0 + dirv * t;
		float  ray_z = rcp(P.z);
		float  depth = get_depth_fast(P.xy);
		depth = lerp(depth, 0.f, is_sky(depth));

		// same hit predicate as the old march: the surface must be in front of the ray
		// AND no closer than the ray start (rejects the HUD weapon and sky-as-zero)
		if (depth <= ray_z && z0 <= depth)
		{
			// binary refinement: bisect the last stride (donor MAX_FIND_STEP halving)
			float t_lo = t - stride;
			float t_hi = t;
			[unroll(SSR_REFINE_STEPS)]
			for (int j = 0; j < SSR_REFINE_STEPS; ++j)
			{
				float  t_mid = (t_lo + t_hi) * 0.5;
				float3 Pm    = P0 + dirv * t_mid;
				float  dm    = get_depth_fast(Pm.xy);
				dm = lerp(dm, 10000.f, is_sky(dm));	// sky = infinitely far, donor parity
				if (dm <= rcp(Pm.z))	t_hi = t_mid;
				else					t_lo = t_mid;
			}

			float3 Ph    = P0 + dirv * t_hi;
			float  hit_z = rcp(Ph.z);
			float  hit_d = get_depth_fast(Ph.xy);
			hit_d = lerp(hit_d, 10000.f, is_sky(hit_d));

			// donor thickness test: relative view-z proximity of the ray to the surface
			float valid = (abs(hit_d - hit_z) * rcp(max(hit_d, hit_z)) < SSR_THICKNESS_REL) ? 1.0 : 0.0;
			valid *= step(z0, hit_d);	// keep the start-depth guard through the refinement
			return float3(Ph.xy, valid);
		}
	}
	return float3(uv0, 0.0);
}

float4 get_reflection (float3 screen_pixel_pos, float3 next_screen_pixel_pos, float3 reflect)
{
	float4 final_color = {1.0,1.0,1.0,1.0};

	// handle case when reflect vector faces the camera
	float facing = dot(eye_direction, reflect);

	if ((facing < -0.5) || (screen_pixel_pos.z > REFL_RANGE)) return final_color;

	// the old half-pixel shift of the marched uv is kept; both endpoints move together,
	// so the ray direction is unchanged and the sampled texels line up with the old march
	float2 uv0 = screen_pixel_pos.xy      + float2(0.5,0.5)*screen_res.zw;
	float2 uv1 = next_screen_pixel_pos.xy + float2(0.5,0.5)*screen_res.zw;

	float3 hit = ssr_march_core(uv0, screen_pixel_pos.z, uv1, next_screen_pixel_pos.z, SSR_JITTER_AMP);

	if (hit.z > 0.5)
	{
		// s_image = rt_SSR, the frame grabbed before water is drawn (see effects_water.s)
		final_color.xyz = s_image.SampleLevel( smp_rtlinear, hit.xy, 0 ).xyz;
		// screen-edge fade kept byte-for-byte from the old march: fold the lower half so
		// only the top and the sides fade, then distance-to-centre^6 is the envmap weight
		float2 tmp = hit.xy;
		tmp.y = lerp(tmp.y, 0.5, step(0.5, tmp.y));
		float screendedgefact = saturate(distance(tmp , float2(0.5, 0.5)) * 2.0);
		final_color.w = pow(screendedgefact,6);
	}
	return lerp(final_color,float4(1.0,1.0,1.0,1.0),screen_pixel_pos.z/REFL_RANGE);
}

float3 calc_envmap(float3 vreflect)
{
	vreflect.y = vreflect.y*2-1;
	float3	env0	= s_env0.SampleLevel( smp_base, vreflect.xyz, 0).xyz;
	float3	env1	= s_env1.SampleLevel( smp_base, vreflect.xyz, 0).xyz;
	return lerp (env0,env1,L_ambient.w);
}
float4 calc_reflections(float4 pos, float3 vreflect)
{
	float4 refl = {1.0,1.0,1.0,1.0};
	float3 v_pixel_pos = mul((float3x4)m_V, pos);
	float4 p_pixel_pos = mul(m_VP, pos);
	float4 s_pixel_pos = proj_to_screen(p_pixel_pos);
	s_pixel_pos.xy /= s_pixel_pos.w;
	s_pixel_pos.z = v_pixel_pos.z;

	float3 reflect_vec = normalize(vreflect);

	// [DA_PORT] near clip: if the 1 m segment end lands behind the camera its projection
	// flips and the march would walk the wrong way. Shorten the segment so its end stays
	// in front of the camera (the march extends past the segment end up to the screen
	// edge anyway, so the reach does not change).
	float dz  = mul((float3x4)m_V, float4(reflect_vec, 0.0)).z;
	float seg = 1.0;
	float near_z = min(float(SSR_Z_NEAR_LIMIT), v_pixel_pos.z * 0.5);
	if (v_pixel_pos.z + dz < near_z)
		seg = (near_z - v_pixel_pos.z) / dz;	// dz < 0 whenever this branch is taken

	float3 W_m_point = pos.xyz + reflect_vec * seg;

	float3 V_m_point = mul((float3x4)m_V, float4(W_m_point, 1.0));
	float4 P_m_point = mul(m_VP, float4(W_m_point, 1.0));
	float4 S_m_point = proj_to_screen(P_m_point);
	S_m_point.xy /= S_m_point.w;
	S_m_point.z = V_m_point.z;

	refl = get_reflection(s_pixel_pos.xyz, S_m_point.xyz, reflect_vec);

	return refl;
}
float4 calc_reflections_late_out(float4 pos, float3 vreflect, float sw)
{
	float4 refl = {1.0,1.0,1.0,1.0};

	float3 v_pixel_pos = mul((float3x4)m_V, pos);
	float4 p_pixel_pos = mul(m_VP, pos);
	float4 s_pixel_pos = proj_to_screen(p_pixel_pos);
	s_pixel_pos.xy /= s_pixel_pos.w;
	s_pixel_pos.z = v_pixel_pos.z;

	float3 reflect_vec = normalize(vreflect);

	// same near clip as calc_reflections - see the comment there
	float dz  = mul((float3x4)m_V, float4(reflect_vec, 0.0)).z;
	float seg = 1.0;
	float near_z = min(float(SSR_Z_NEAR_LIMIT), v_pixel_pos.z * 0.5);
	if (v_pixel_pos.z + dz < near_z)
		seg = (near_z - v_pixel_pos.z) / dz;

	float3 W_m_point = pos.xyz + reflect_vec * seg;

	float3 V_m_point = mul((float3x4)m_V, float4(W_m_point, 1.0));
	float4 P_m_point = mul(m_VP, float4(W_m_point, 1.0));
	float4 S_m_point = proj_to_screen(P_m_point);
	S_m_point.xy /= S_m_point.w;
	S_m_point.z = V_m_point.z;

	if (sw > 0.01)
		refl = get_reflection(s_pixel_pos.xyz, S_m_point.xyz, reflect_vec);

	return refl;
}
#endif
