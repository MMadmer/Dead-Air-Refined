#ifndef	common_functions_h_included
#define	common_functions_h_included

//	contrast function
float Contrast(float Input, float ContrastPower)
{
     //piecewise contrast function
     bool IsAboveHalf = Input > 0.5 ;
     float ToRaise = saturate(2*(IsAboveHalf ? 1-Input : Input));
     float Output = 0.5*pow(ToRaise, ContrastPower);
     Output = IsAboveHalf ? 1-Output : Output;
     return Output;
}

// Tonemap tinting arrives in da_tonemap_params (bound in r2.cpp). The SIGNATURE of tonemap()
// must not change: archive shaders call it too, and a mismatch dies silently with a stub.
// A zero constant (a shader where it is not bound) reproduces the old behaviour exactly: the
// white point falls back to 1.7 and the luminance share stays zero.
// y = white point, z = luminance-tonemap share, w = late-desaturation power.
uniform float4 da_tonemap_params;

// Colour grade (da_grade_params, bound in r2.cpp), applied to the tonemapped value, so it sees
// display-range colour and never touches the HDR bloom source. x = saturation, y = an extra
// saturation factor for green-dominant colour, z = the pull of green toward olive (a share of
// the green channel handed to red), w = contrast around linear middle grey. A zero constant
// leaves the frame as it was.
uniform float4 da_grade_params;

float3 da_grade( float3 c )
{
	[branch] if ( da_grade_params.x < 0.001f ) return c;
	const float3 LUM = float3(0.2126f, 0.7152f, 0.0722f);
	// how green the colour is: green above both other channels, as a share of green
	const float g = saturate((c.g - max(c.r, c.b)) / max(c.g, 1e-4f));
	// olive: foliage under a real camera sits toward yellow, not at the texture's pure green
	c.r = lerp(c.r, c.g, da_grade_params.z * g);
	const float l = dot(c, LUM);
	const float s = da_grade_params.x * lerp(1.0f, da_grade_params.y, g);
	c = lerp(l.xxx, c, s);
	// contrast around 0.18 linear, never below black
	return max(0.0f, (c - 0.18f) * da_grade_params.w + 0.18f);
}

void tonemap( out float4 low, out float4 high, float3 rgb, float scale)
{
	rgb		=	rgb*scale;

	const float fWhiteIntensity = (da_tonemap_params.y > 0.01f) ? da_tonemap_params.y : 1.7;

	const float fWhiteIntensitySQR = fWhiteIntensity*fWhiteIntensity;

//	low		=	(rgb/(rgb + 1)).xyzz;
	float3 tm	=	(rgb*(1+rgb/fWhiteIntensitySQR)) / (rgb+1);

	// Luminance-preserving tonemap (r__tonemap_hue). Zero keeps the per-channel path.
	// The per-channel curve pulls channels toward each other, so everything bright bleaches -
	// sunlit foliage goes white. Here the curve is computed ONCE on luminance and the channels
	// are rescaled by the new-to-old luminance ratio, so proportions survive any brightness.
	[branch] if ( da_tonemap_params.z > 0.001f )
	{
		const float3 LUM = float3(0.2126f, 0.7152f, 0.0722f);
		const float  l   = dot(rgb, LUM);
		const float  lt  = (l*(1+l/fWhiteIntensitySQR)) / (l+1);
		float3 hue = rgb * (lt / max(l, 1e-4f));

		// True overexposure still goes to white: without this the sun disc and speculars come
		// out coloured and acid. The power (r__tonemap_desat) decides how late that starts.
		hue = lerp(hue, lt.xxx, pow(saturate(lt), max(da_tonemap_params.w, 1.0f)));

		tm = lerp(tm, hue, saturate(da_tonemap_params.z));
	}

	tm = da_grade(tm);

	low		=	tm.xyzz;

	high	=	rgb.xyzz/def_hdr;	// 8x dynamic range
}

// Foliage albedo knobs (bound in r2.cpp): .x gloss multiplier shifted by one (0 = constant
// not bound, keep stock), .z vibrance, .w debleach strength. Used by deffer_base_aref_*.
uniform float4 da_foliage;

// Kills BRIGHT AND COLOURLESS: a bleached branch has high luminance at near-zero saturation,
// green needles have saturation and stay untouched - unlike a flat darkening that would
// press the whole tree down at once.
float3 da_debleach( float3 c, float k )
{
	[branch] if ( k < 0.001f ) return c;
	const float3 LUM = float3(0.2126f, 0.7152f, 0.0722f);
	const float  mx  = max(c.r, max(c.g, c.b));
	const float  mn  = min(c.r, min(c.g, c.b));
	const float  sat = (mx > 1e-4f) ? ((mx - mn) / mx) : 0.0f;
	const float  bl  = saturate(dot(c, LUM)) * (1.0f - saturate(sat));
	return c * (1.0f - saturate(k) * bl);
}

float3 Vibrance( float3 i, half val )
{
	float luminance = dot( float3( i.rgb ), LUMINANCE_VECTOR );
	return float3( lerp( luminance, float3( i.rgb ), val ));
}

float4 sat( float4 i, half val )
{
	float luminance = dot( float3( i.rgb ), LUMINANCE_VECTOR );
	return float4( lerp( luminance, float3( i.rgb ), val ), i.w );
}

float4 combine_bloom( float3  low, float4 high)
{
        return float4( low + high.rgb, 1.h );
}

float calc_fogging( float4 w_pos )
{
	return dot(w_pos,fog_plane);
}

float2 unpack_tc_base( float2 tc, float du, float dv )
{
		return (tc.xy + float2	(du,dv))*(32.f/32768.f); //!Increase from 32bit to 64bit floating point
}

float3 calc_sun_r1( float3 norm_w )
{
	return L_sun_color*saturate(dot((norm_w),-L_sun_dir_w));
}

float3 calc_model_hemi_r1( float3 norm_w )
{
 return max(0,norm_w.y)*L_hemi_color.rgb;
}

float3 calc_model_lq_lighting( float3 norm_w )
{
	return L_material.x*calc_model_hemi_r1(norm_w) + L_ambient.rgb + L_material.y*calc_sun_r1(norm_w);
}

float3 	unpack_normal( float3 v )	{ return 2*v-1; }
float3 	unpack_bx2( float3 v )	{ return 2*v-1; }
float3 	unpack_bx4( float3 v )	{ return 4*v-2; } //!reduce the amount of stretching from 4*v-2 and increase precision
float2 	unpack_tc_lmap( float2 tc )	{ return tc*(1.f/32768.f);	} // [-1  .. +1 ]
float4	unpack_color( float4 c ) { return c.bgra; }
float4	unpack_D3DCOLOR( float4 c ) { return c.bgra; }
float3	unpack_D3DCOLOR( float3 c ) { return c.bgr; }

float3   p_hemi( float2 tc )
{
//	float3	t_lmh = tex2D (s_hemi, tc);
//	float3	t_lmh = s_hemi.Sample( smp_rtlinear, tc);
//	return	dot(t_lmh,1.h/4.h);
	float4	t_lmh = s_hemi.Sample( smp_rtlinear, tc);
	return	t_lmh.a;
}

float   get_hemi( float4 lmh)
{
	return lmh.a;
}

float   get_sun( float4 lmh)
{
	return lmh.g;
}

float3	v_hemi(float3 n)
{
	return L_hemi_color.rgb*(.5f + .5f*n.y);
}

float3	v_sun(float3 n)
{
	return L_sun_color*dot(n,-L_sun_dir_w);
}

float3	calc_reflection( float3 pos_w, float3 norm_w )
{
    return reflect(normalize(pos_w-eye_position), norm_w);
}

float4 proj_to_screen(float4 proj)
{
	float4 screen = proj;
	screen.x = (proj.x + proj.w);
	screen.y = (proj.w - proj.y);
	screen.xy *= 0.5;
	return screen;
}
float4 screen_to_proj(float2 screen, float z)
{
	float4 proj;
	proj.w = 1.0;
	proj.z = z;
	proj.x = screen.x*2 - proj.w;
	proj.y = -screen.y*2 + proj.w;
	return proj;
}
float is_in_range(float3 args)
{
	float mn = (args.x > args.y) ? 1: 0;
	float mx = (args.z > args.x) ? 1: 0;
	return mn*mx;
}

#define USABLE_BIT_1                uint(0x00002000)
#define USABLE_BIT_2                uint(0x00004000)
#define USABLE_BIT_3                uint(0x00008000)
#define USABLE_BIT_4                uint(0x00010000)
#define USABLE_BIT_5                uint(0x00020000)
#define USABLE_BIT_6                uint(0x00040000)
#define USABLE_BIT_7                uint(0x00080000)
#define USABLE_BIT_8                uint(0x00100000)
#define USABLE_BIT_9                uint(0x00200000)
#define USABLE_BIT_10               uint(0x00400000)
#define USABLE_BIT_11               uint(0x00800000)   // At least two of those four bit flags must be mutually exclusive (i.e. all 4 bits must not be set together)
#define USABLE_BIT_12               uint(0x01000000)   // This is because setting 0x47800000 sets all 5 FP16 exponent bits to 1 which means infinity
#define USABLE_BIT_13               uint(0x02000000)   // This will be translated to a +/-MAX_FLOAT in the FP16 render target (0xFBFF/0x7BFF), overwriting the
#define USABLE_BIT_14               uint(0x04000000)   // mantissa bits where other bit flags are stored.
#define USABLE_BIT_15               uint(0x80000000)
#define MUST_BE_SET                 uint(0x40000000)   // This flag *must* be stored in the floating-point representation of the bit flag to store

/*
float2 gbuf_pack_normal( float3 norm )
{
   float2 res;

   res = 0.5 * ( norm.xy + float2( 1, 1 ) ) ;
   res.x *= ( norm.z < 0 ? -1.0 : 1.0 );

   return res;
}

float3 gbuf_unpack_normal( float2 norm )
{
   float3 res;

   res.xy = ( 2.0 * abs( norm ) ) - float2(1,1);

   res.z = ( norm.x < 0 ? -1.0 : 1.0 ) * sqrt( abs( 1 - res.x * res.x - res.y * res.y ) );

   return res;
}
*/

// Holger Gruen AMD - I change normal packing and unpacking to make sure N.z is accessible without ALU cost
// this help the HDAO compute shader to run more efficiently
float2 gbuf_pack_normal( float3 norm )
{
   float2 res;

   res.x  = norm.z;
   res.y  = 0.5f * ( norm.x + 1.0f ) ;
   res.y *= ( norm.y < 0.0f ? -1.0f : 1.0f );

   return res;
}

float3 gbuf_unpack_normal( float2 norm )
{
   float3 res;

   res.z  = norm.x;
   res.x  = ( 2.0f * abs( norm.y ) ) - 1.0f;
   res.y = ( norm.y < 0 ? -1.0 : 1.0 ) * sqrt( abs( 1 - res.x * res.x - res.z * res.z ) );

   return res;
}

// Raised by deffer_impl_flat for the pixels its puddle mask covers - rain puddles live on the
// static ground only. The puddle reflection pass reads the flag back and leaves every other
// pixel alone: a grass blade or a leaf card whose XZ falls on a puddle is not water, however
// level the depth around it looks from afar.
static uint da_gbuf_ground = 0u;

float gbuf_pack_hemi_mtl( float hemi, float mtl )
{
   uint packed_mtl = uint( ( mtl / 1.333333333 ) * 31.0 );
	// hemi keeps seven bits (steps of 1/127 - an occlusion term never showed the eighth); bit 13,
	// the low FP16 mantissa bit, carries the ground flag.
	uint packed = ( MUST_BE_SET + ( uint( saturate(hemi) * 127.9 ) << 14 ) + ( ( packed_mtl & uint( 31 ) ) << 21 )
		+ ( da_gbuf_ground != 0u ? USABLE_BIT_1 : 0u ) );

   if( ( packed & USABLE_BIT_13 ) == 0 )
      packed |= USABLE_BIT_14;

   if( packed_mtl & uint( 16 ) )
      packed |= USABLE_BIT_15;

   return asfloat( packed );
}

float gbuf_unpack_hemi( float mtl_hemi )
{
	return float( ( asuint( mtl_hemi ) >> 14 ) & uint(127) ) * (1.0/127.0);
}

float gbuf_unpack_ground( float mtl_hemi )
{
	return ( asuint( mtl_hemi ) & USABLE_BIT_1 ) != 0u ? 1.0 : 0.0;
}

float gbuf_unpack_mtl( float mtl_hemi )
{
   uint packed       = asuint( mtl_hemi );
   uint packed_hemi  = ( ( packed >> 21 ) & uint(15) ) + ( ( packed & USABLE_BIT_15 ) == 0 ? 0 : 16 );
   return float( packed_hemi ) * (1.0/31.0) * 1.333333333;
}

#ifndef EXTEND_F_DEFFER
f_deffer pack_gbuffer( float4 norm, float4 pos, float4 col )
#else
f_deffer pack_gbuffer( float4 norm, float4 pos, float4 col, uint imask )
#endif
{
	f_deffer res;

#ifndef GBUFFER_OPTIMIZATION
	// This layout stores the material as a plain float: its sign carries the ground flag.
	res.position	= float4( pos.xyz, da_gbuf_ground != 0u ? -abs( pos.w ) - 0.001 : pos.w );
	res.Ne			= norm;
	res.C			   = col;
#else
	res.position	= float4( gbuf_pack_normal( norm.xyz ), pos.z, gbuf_pack_hemi_mtl( norm.w, pos.w ) );
	res.C			   = col;
#endif

#ifdef EXTEND_F_DEFFER
   res.mask = imask;
#endif

	return res;
}

#ifdef GBUFFER_OPTIMIZATION
gbuffer_data gbuffer_load_data( float2 tc : TEXCOORD, float2 pos2d, int iSample )
{
	gbuffer_data gbd;

	gbd.P = float3(0,0,0);
	gbd.hemi = 0;
	gbd.mtl = 0;
	gbd.C = 0;
	gbd.N = float3(0,0,0);

#ifndef USE_MSAA
	float4 P	= s_position.Sample( smp_nofilter, tc );
#else
	float4 P	= s_position.Load( int2( pos2d ), iSample );
#endif

	// 3d view space pos reconstruction math
	// center of the plane (0,0) or (0.5,0.5) at distance 1 is eyepoint(0,0,0) + lookat (assuming |lookat| ==1
	// left/right = (0,0,1) -/+ tan(fHorzFOV/2) * (1,0,0 )
	// top/bottom = (0,0,1) +/- tan(fVertFOV/2) * (0,1,0 )
	// lefttop		= ( -tan(fHorzFOV/2),  tan(fVertFOV/2), 1 )
	// righttop		= (  tan(fHorzFOV/2),  tan(fVertFOV/2), 1 )
	// leftbottom   = ( -tan(fHorzFOV/2), -tan(fVertFOV/2), 1 )
	// rightbottom	= (  tan(fHorzFOV/2), -tan(fVertFOV/2), 1 )
	gbd.P  = float3( P.z * ( pos2d * pos_decompression_params.zw - pos_decompression_params.xy ), P.z );

	// reconstruct N
	gbd.N = gbuf_unpack_normal( P.xy );

	// reconstruct material
	gbd.mtl	= gbuf_unpack_mtl( P.w );

   // reconstruct hemi
   gbd.hemi = gbuf_unpack_hemi( P.w );
   gbd.ground = gbuf_unpack_ground( P.w );

#ifndef USE_MSAA
   float4	C	= s_diffuse.Sample( smp_nofilter, tc );
#else
   float4	C	= s_diffuse.Load( int2( pos2d ), iSample );
#endif

	gbd.C		= C.xyz;
	gbd.gloss	= C.w;

	return gbd;
}

gbuffer_data gbuffer_load_data( float2 tc : TEXCOORD, float2 pos2d )
{
   return gbuffer_load_data( tc, pos2d, 0 );
}

gbuffer_data gbuffer_load_data_offset( float2 tc : TEXCOORD, float2 OffsetTC : TEXCOORD, float2 pos2d )
{
	float2  delta	  = ( ( OffsetTC - tc ) * pos_decompression_params2.xy );

	return gbuffer_load_data( OffsetTC, pos2d + delta, 0 );
}

gbuffer_data gbuffer_load_data_offset( float2 tc : TEXCOORD, float2 OffsetTC : TEXCOORD, float2 pos2d, uint iSample )
{
   float2  delta	  = ( ( OffsetTC - tc ) * pos_decompression_params2.xy );

   return gbuffer_load_data( OffsetTC, pos2d + delta, iSample );
}

#else // GBUFFER_OPTIMIZATION
gbuffer_data gbuffer_load_data( float2 tc : TEXCOORD, uint iSample )
{
	gbuffer_data gbd;

#ifndef USE_MSAA
	float4 P	= s_position.Sample( smp_nofilter, tc );
#else
    float4 P	= s_position.Load( int2( tc * pos_decompression_params2.xy ), iSample );
#endif

	gbd.P		= P.xyz;
	// the sign of the material carries the ground flag in this layout (see pack_gbuffer)
	gbd.mtl		= abs( P.w );
	gbd.ground	= P.w < 0.0 ? 1.0 : 0.0;

#ifndef USE_MSAA
	float4 N	= s_normal.Sample( smp_nofilter, tc );
#else
	float4 N	= s_normal.Load( int2( tc * pos_decompression_params2.xy ), iSample );
#endif

	gbd.N		= N.xyz;
	gbd.hemi	= N.w;

#ifndef USE_MSAA
	float4	C	= s_diffuse.Sample(  smp_nofilter, tc );
#else
	float4	C	= s_diffuse.Load( int2( tc * pos_decompression_params2.xy ), iSample );
#endif


	gbd.C		= C.xyz;
	gbd.gloss	= C.w;

	return gbd;
}

gbuffer_data gbuffer_load_data( float2 tc : TEXCOORD  )
{
   return gbuffer_load_data( tc, 0 );
}

gbuffer_data gbuffer_load_data_offset( float2 tc : TEXCOORD, float2 OffsetTC : TEXCOORD, uint iSample )
{
   return gbuffer_load_data( OffsetTC, iSample );
}

#endif // GBUFFER_OPTIMIZATION

#ifdef GBUFFER_OPTIMIZATION
float3 gbuffer_load_position(float2 tc, float2 pos2d, uint iSample)
{
#ifndef USE_MSAA
	float depth = s_position.Sample(smp_nofilter, tc).z;
#else
	float depth = s_position.Load(int2(pos2d), iSample).z;
#endif
	return float3(depth * (pos2d * pos_decompression_params.zw - pos_decompression_params.xy), depth);
}

float gbuffer_load_depth(float2 tc, float2 pos2d, uint iSample)
{
#ifndef USE_MSAA
	return s_position.Sample(smp_nofilter, tc).z;
#else
	return s_position.Load(int2(pos2d), iSample).z;
#endif
}

float gbuffer_load_depth_offset(float2 tc, float2 offsetTC, float2 pos2d, uint iSample)
{
#ifndef USE_MSAA
	return s_position.Sample(smp_nofilter, offsetTC).z;
#else
	float2 delta = (offsetTC - tc) * pos_decompression_params2.xy;
	return s_position.Load(int2(pos2d + delta), iSample).z;
#endif
}
#else
float3 gbuffer_load_position(float2 tc, uint iSample)
{
#ifndef USE_MSAA
	return s_position.Sample(smp_nofilter, tc).xyz;
#else
	return s_position.Load(int2(tc * pos_decompression_params2.xy), iSample).xyz;
#endif
}

float gbuffer_load_depth(float2 tc, uint iSample)
{
#ifndef USE_MSAA
	return s_position.Sample(smp_nofilter, tc).z;
#else
	return s_position.Load(int2(tc * pos_decompression_params2.xy), iSample).z;
#endif
}

float gbuffer_load_depth_offset(float2 offsetTC, uint iSample)
{
	return gbuffer_load_depth(offsetTC, iSample);
}
#endif

//////////////////////////////////////////////////////////////////////////
//	Aplha to coverage code
#if ( defined( MSAA_ALPHATEST_DX10_1_ATOC ) || defined( MSAA_ALPHATEST_DX10_1 ) )

#if MSAA_SAMPLES == 2
uint alpha_to_coverage ( float alpha, float2 pos2d )
{
	uint mask;
	uint pos = uint(pos2d.x) | uint( pos2d.y);
	if( alpha < 0.3333 )
		mask = 0;
	else if( alpha < 0.6666 )
		mask = 1 << ( pos & 1 );
	else
		mask = 3;

	return mask;
}
#endif

#if MSAA_SAMPLES == 4
uint alpha_to_coverage ( float alpha, float2 pos2d )
{
	uint mask;

	float off = float( ( uint(pos2d.x) | uint( pos2d.y) ) & 3 );
	alpha = saturate( alpha - off * ( ( 0.2 / 4.0 ) / 3.0 ) );
	if( alpha < 0.40 )
	{
		if( alpha < 0.20 )
			mask = 0;
		else if( alpha < 0.40 ) // only one bit set
			mask = 1;
	}
  else
  {
	if( alpha < 0.60 ) // 2 bits set => 1100 0110 0011 1001 1010 0101
	{
		mask = 3;
	}
	else if( alpha < 0.8 ) // 3 bits set => 1110 0111 1011 1101
	  mask = 7;
	else
	  mask = 0xf;
 }

	return mask;
}
#endif

#if MSAA_SAMPLES == 8
uint alpha_to_coverage ( float alpha, float2 pos2d )
{
	uint mask;

	float off = float( ( uint(pos2d.x) | uint( pos2d.y) ) & 3 );
	alpha = saturate( alpha - off * ( ( 0.1111 / 8.0 ) / 3.0 ) );
  if( alpha < 0.4444 )
  {
	if( alpha < 0.2222 )
	{
		if( alpha < 0.1111 )
			mask = 0;
		else // only one bit set 0.2222
			mask = 1;
	}
	else
	{
		if( alpha < 0.3333 ) // 2 bits set0=> 10000001 + 11000000 .. 00000011 : 8 // 0.2222
		  				   //        set1=> 10100000 .. 00000101 + 10000010 + 01000001 : 8
						   //		set2=> 10010000 .. 00001001 + 10000100 + 01000010 + 00100001 : 8
						   //		set3=> 10001000 .. 00010001 + 10001000 + 01000100 + 00100010 + 00010001 : 8
		{
			mask = 3;
		}
	    else // 3 bits set0 => 11100000 .. 00000111 + 10000011 + 11000001 : 8 ? 0.4444 // 0.3333
			 //        set1 => 10110000 .. 00001011 + 10000101 + 11000010 + 01100001: 8
			 //        set2 => 11010000 .. 00001101 + 10000110 + 01000011 + 10100001: 8
			 //        set3 => 10011000 .. 00010011 + 10001001 + 11000100 + 01100010 + 00110001 : 8
			 //        set4 => 11001000 .. 00011001 + 10001100 + 01000110 + 00100011 + 10010001 : 8
		{
			mask = 0x7;
		}
	}
  }
  else
  {
	  if( alpha < 0.6666 )
	  {
		if( alpha < 0.5555 ) // 4 bits set0 => 11110000 .. 00001111 + 10000111 + 11000011 + 11100001 : 8 // 0.5555
		 				   //        set1 => 11011000 .. 00011011 + 10001101 + 11000110 + 01100011 + 10110001 : 8
						   //        set2 => 11001100 .. 00110011 + 10011001 : 4 make 8
						   //        set3 => 11000110 + 01100011 + 10110001 + 11011000 + 01101100 + 00110110 + 00011011 + 10001101 : 8
						   //        set4 => 10111000 .. 00010111 + 10001011 + 11000101 + 11100010 + 01110001 : 8
						   //        set5 => 10011100 .. 00100111 + 10010011 + 11001001 + 11100100 + 01110010 + 00111001 : 8
						   //        set6 => 10101010 .. 01010101 : 2 make 8
						   //        set7 => 10110100 +  01011010 + 00101101 + 10010110 + 01001011 + 10100101 + 11010010 + 01101001 : 8
						   //        set8 => 10011010 +  01001101 + 10100110 + 01010011 + 10101001 + 11010100 + 01101010 + 00110101 : 8
		{
			mask = 0xf;
		}
		else // 5 bits set0 => 11111000 01111100 00111110 00011111 10001111 11000111 11100011 11110001 : 8  // 0.6666
		     //        set1 => 10111100 : 8
		     //        set2 => 10011110 : 8
		     //        set3 => 11011100 : 8
		     //        set4 => 11001110 : 8
		     //        set5 => 11011010 : 8
		     //        set6 => 10110110 : 8
		{
			mask = 0x1F;
		}
	  }
	  else
	  {
		if( alpha < 0.7777 ) // 6 bits set0 => 11111100 01111110 00111111 10011111 11001111 11100111 11110011 11111001 : 8
						  //        set1 => 10111110 : 8
						  //        set2 => 11011110 : 8
		{
			mask = 0x3F;
		}
		else if( alpha < 0.8888 ) // 7 bits set0 => 11111110 :8
		{
			mask = 0x7F;
		}
		else // all 8 bits set
			mask = 0xFF;
	 }
  }

	return mask;
}
#endif
#endif



#endif	//	common_functions_h_included
