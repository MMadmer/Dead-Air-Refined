#ifndef EXTCOMMON_H_INCLUDED
#define EXTCOMMON_H_INCLUDED

//////////////////////////////////////////////////////////////////////////////////////////
//
#define		MrProperDepthConstant		float(100)//COP

//////////////////////////////////////////////////////////////////////////////////////////
// [DA_PORT] The pixel side of v2p_aa_AA (stub_notransform_aa_aa.vs), field for field: the stock
// struct read seven float4 registers where the vertex shader packs float2 pairs, which D3D11
// tolerates and DirectX 12 refuses at pipeline creation (the sun shafts never drew there).
// Every user reads texCoord0.xy only.
struct		AntiAliasingStruct
{
	float2		texCoord0		:		TEXCOORD0;
	float2		texCoord1		:		TEXCOORD1;
	float2		texCoord2		:		TEXCOORD2;
	float2		texCoord3		:		TEXCOORD3;
	float2		texCoord4		:		TEXCOORD4;
	float4		texCoord5		:		TEXCOORD5;
	float4		texCoord6		:		TEXCOORD6;
};

struct		OUTStruct
{
	float4		Position		:		SV_Target0;
	float4		Normal		:		SV_Target1;
};

//////////////////////////////////////////////////////////////////////////////////////////
//
uniform		Texture2D		sScene;
uniform		Texture2D		sPosition;
uniform		Texture2D		sMask;          //sunshafts occlusion mask
uniform		Texture2D		sMaskBlur;      //smoothed mask
uniform		Texture2D		sSunShafts;     //


uniform		float4		SSParams;
uniform		float4		SSParamsDISPLAY;
//////////////////////////////////////////////////////////////////////////////////////////
//
float		NormalizeDepth(float Depth)
{
	return saturate(Depth/MrProperDepthConstant);
}

float4		proj2screen(float4 Project)
{
	float4 Screen;
	Screen.x = (Project.x+Project.w)*0.5;
	Screen.y = (Project.w-Project.y)*0.5;
	Screen.z = Project.z;
	Screen.w = Project.w;
	return Screen;
}

#endif//EXTCOMMON_H_INCLUDED
