#ifndef OGSE_REFLECTIONS_H
#define OGSE_REFLECTIONS_H
// [DA_PORT] Reflection quality, set from the "r3_water_refl" console/video setting (SSR_QUALITY 1..4).
// SSR_MAX_IT is the ray-march length: the step is 1/256 of the screen, so 64 iterations only reach 25%
// of the screen and distant things never reflect. REFL_RANGE cuts reflections off past that eye depth
// and fades them to white before it. Both scale with quality; 3 (high) is the default.
#ifndef SSR_QUALITY
#  define SSR_QUALITY 3
#endif
#if SSR_QUALITY <= 1
#  define SSR_MAX_IT 64
#  define REFL_RANGE 100
#elif SSR_QUALITY == 2
#  define SSR_MAX_IT 110
#  define REFL_RANGE 140
#elif SSR_QUALITY == 3
#  define SSR_MAX_IT 160
#  define REFL_RANGE 180
#else
#  define SSR_MAX_IT 240
#  define REFL_RANGE 220
#endif
#define SKY_EPS float(0.001)

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

#ifdef SSR_JITTER
// [DA_PORT] Per-pixel dither for the ray start ("r3_water_refl_jitter"). Every pixel marching in
// lockstep is what turns a fixed-stride trace into visible stair-steps along reflection edges;
// offsetting each start by a fraction of one step trades those bands for fine noise.
// Deliberately depends on screen position only, not on time — a time-varying hash would shimmer,
// since we have no temporal accumulation to average it out.
float ssr_jitter_hash(float2 tc)
{
	return frac(sin(dot(tc, float2(12.9898, 78.233))) * 43758.5453);
}
#endif

float4 get_reflection (float3 screen_pixel_pos, float3 next_screen_pixel_pos, float3 reflect)
{
	float4 final_color = {1.0,1.0,1.0,1.0};
	float2 factors = {1.f,1.f};
	
	float3 main_vec = next_screen_pixel_pos - screen_pixel_pos;
	float3 grad_vec = main_vec / (max(abs(main_vec.x), abs(main_vec.y)) * 256);
	
	// handle case when reflect vector faces the camera
	factors.x = dot(eye_direction, reflect);

	if ((factors.x < -0.5) || (screen_pixel_pos.z > REFL_RANGE)) return final_color;
	else
	{
		float3 curr_pixel = screen_pixel_pos;
		curr_pixel.xy += float2(0.5,0.5)*screen_res.zw;
#ifdef SSR_JITTER
		curr_pixel.xyz += grad_vec.xyz * ssr_jitter_hash(screen_pixel_pos.xy);	// [DA_PORT] break up banding
#endif
		float max_it = SSR_MAX_IT;	// [DA_PORT] scales with r3_water_refl (see top of file)
		float i = 0;
		bool br = false;
		
		// [DA_PORT] [loop] ОБЯЗАТЕЛЕН. max_it инициализируется константой (SSR_MAX_IT), поэтому
		// компилятор считает границу известной и разворачивает цикл целиком — 160 копий тела на
		// качестве по умолчанию и 240 на максимальном, каждая с выборкой текстуры. На этом
		// D3DCompile не выдаёт ошибку, а ПАДАЕТ, унося процесс без стека и без сообщения: игра
		// просто исчезала на загрузке любого уровня с водой, а лог обрывался на имени шейдера.
		// Цикл и так динамический — из него выходят по br, — так что разворачивать его незачем.
		[loop]
		while ((i < max_it) && (br == false))
		{
			curr_pixel.xyz += grad_vec.xyz;
			float depth = get_depth_fast(curr_pixel.xy);
			depth = lerp(depth, 0.f, is_sky(depth));
			float delta = step(depth, curr_pixel.z)*step(screen_pixel_pos.z, depth);
			if (delta > 0.5)
			{
				// [DA_PORT] restore the screen-space colour fetch: the r3/DX11 copy shipped with it
				// replaced by black (only the r2/DX9 copy sampled the frame), so DX11 water had a hit
				// mask but no reflection. s_image = rt_SSR, the frame grabbed before water was drawn.
				final_color.xyz = s_image.SampleLevel( smp_rtlinear, curr_pixel.xy, 0 ).xyz;
				float2 tmp = curr_pixel.xy;
				tmp.y = lerp(tmp.y, 0.5, step(0.5, tmp.y));
				float screendedgefact = saturate(distance(tmp , float2(0.5, 0.5)) * 2.0);
				final_color.w = pow(screendedgefact,6);
				br = true;
			}
			i += 1.0;
		}
		return lerp(final_color,float4(1.0,1.0,1.0,1.0),screen_pixel_pos.z/REFL_RANGE);
	}
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
	float3 W_m_point = pos.xyz + reflect_vec;

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
	float3 W_m_point = pos.xyz + reflect_vec;

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