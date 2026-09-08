#pragma once

namespace xray::render::RENDER_NAMESPACE
{
class CBlender_fluid_advect : public IBlender
{
public:
    virtual LPCSTR getComment() { return "INTERNAL: 3dfluid maths"; }
    virtual BOOL canBeDetailed() { return FALSE; }
    virtual BOOL canBeLMAPped() { return FALSE; }
    virtual void Compile(CBlender_Compile& C);
};

class CBlender_fluid_advect_velocity : public IBlender
{
public:
    virtual LPCSTR getComment() { return "INTERNAL: 3dfluid maths"; }
    virtual BOOL canBeDetailed() { return FALSE; }
    virtual BOOL canBeLMAPped() { return FALSE; }
    virtual void Compile(CBlender_Compile& C);
};

class CBlender_fluid_simulate : public IBlender
{
public:
    virtual LPCSTR getComment() { return "INTERNAL: 3dfluid maths"; }
    virtual BOOL canBeDetailed() { return FALSE; }
    virtual BOOL canBeLMAPped() { return FALSE; }
    virtual void Compile(CBlender_Compile& C);
};

class CBlender_fluid_obst : public IBlender
{
public:
    virtual LPCSTR getComment() { return "INTERNAL: 3dfluid maths 2"; }
    virtual BOOL canBeDetailed() { return FALSE; }
    virtual BOOL canBeLMAPped() { return FALSE; }
    virtual void Compile(CBlender_Compile& C);
};

class CBlender_fluid_emitter : public IBlender
{
public:
    virtual LPCSTR getComment() { return "INTERNAL: 3dfluid emitters"; }
    virtual BOOL canBeDetailed() { return FALSE; }
    virtual BOOL canBeLMAPped() { return FALSE; }
    virtual void Compile(CBlender_Compile& C);
};

class CBlender_fluid_obstdraw : public IBlender
{
public:
    virtual LPCSTR getComment() { return "INTERNAL: 3dfluid maths 2"; }
    virtual BOOL canBeDetailed() { return FALSE; }
    virtual BOOL canBeLMAPped() { return FALSE; }
    virtual void Compile(CBlender_Compile& C);
};

class CBlender_fluid_raydata : public IBlender
{
public:
    virtual LPCSTR getComment() { return "INTERNAL: 3dfluid maths 2"; }
    virtual BOOL canBeDetailed() { return FALSE; }
    virtual BOOL canBeLMAPped() { return FALSE; }
    virtual void Compile(CBlender_Compile& C);
};

//	The campfire's simulation passes and its own ray-cast. Separate blenders because a shader
//	only carries six elements and the stock ray-cast already uses five of them.
class CBlender_fluid_dafire : public IBlender
{
public:
    virtual LPCSTR getComment() { return "INTERNAL: 3dfluid campfire"; }
    virtual BOOL canBeDetailed() { return FALSE; }
    virtual BOOL canBeLMAPped() { return FALSE; }
    virtual void Compile(CBlender_Compile& C);
};

class CBlender_fluid_dafire_ray : public IBlender
{
public:
    virtual LPCSTR getComment() { return "INTERNAL: 3dfluid campfire raycast"; }
    virtual BOOL canBeDetailed() { return FALSE; }
    virtual BOOL canBeLMAPped() { return FALSE; }
    virtual void Compile(CBlender_Compile& C);
};

class CBlender_fluid_raycast : public IBlender
{
public:
    virtual LPCSTR getComment() { return "INTERNAL: 3dfluid maths 2"; }
    virtual BOOL canBeDetailed() { return FALSE; }
    virtual BOOL canBeLMAPped() { return FALSE; }
    virtual void Compile(CBlender_Compile& C);
};
} // namespace xray::render::RENDER_NAMESPACE
