#ifndef	common_defines_h_included
#define	common_defines_h_included

//////////////////////////////////////////////////////////////////////////////////////////
// Defines                                		//

#define H_MAIN		7.0f
#define H_TERR		0.5f*H_MAIN
#define H_GRASS		0.5f*H_MAIN
#define H_MODELS	0.2f*H_MAIN
#define H_BUSHES	0.2f*H_MAIN

#define L_RANGE		1.0f
#define L_BRIGHT	1.0f

#define def_gloss       float(1.f /255.f)
// Alpha-ref rides the quality presets now: the engine binds da_aref_u every frame (see
// cl_da_aref in Blender_Recorder_StandartBinding.cpp), ladder 180/160/128/110/100 by preset.
// Default preset keeps the historical 128/255 look exactly.
uniform float           da_aref_u;
#define def_aref        (da_aref_u)
#define def_dbumph      float(0.333f)
#define def_virtualh    float(0.05f)              // 5cm
#define def_distort     float(0.05f)             // we get -0.5 .. 0.5 range, this is -512 .. 512 for 1024, so scale it
#define def_hdr         float(7.h)         		// hight luminance range float(3.h)
#define def_hdr_clip	float(0.75h)        		//
#define def_lum_hrange	float(0.7h)	// hight luminance range

#define	LUMINANCE_VECTOR	float3(0.3f, 0.48f, 0.22f)

#define MAXCOF		7.h
#define EPSDEPTH	0.001h

#define SCR_WIDTH screen_res.x
#define SCR_HEIGHT screen_res.y
#define PIXEL_SIZE float2( 1/SCR_WIDTH, 1/SCR_HEIGHT ) 

//////////////////////////////////////////////////////////////////////////////////////////
#ifndef SMAP_size
#define SMAP_size        4096
#endif
#define PARALLAX_H 0.02
#define parallax float2(PARALLAX_H, -PARALLAX_H/2)
//////////////////////////////////////////////////////////////////////////////////////////

#endif	//	common_defines_h_included