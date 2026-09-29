#pragma once

namespace xray::render::RENDER_NAMESPACE
{
class CBlender_combine : public IBlender
{
    // What combine_2 reads as the scene (s_image): generic0, or the upscaler's output.
    pcstr m_image{ r2_RT_generic0 };

public:
    virtual LPCSTR getComment() { return "INTERNAL: combiner"; }
    virtual BOOL canBeDetailed() { return FALSE; }
    virtual BOOL canBeLMAPped() { return FALSE; }
    virtual void Compile(CBlender_Compile& C);

    CBlender_combine();
    explicit CBlender_combine(pcstr image);
    virtual ~CBlender_combine();
};

class CBlender_sunshafts final : public IBlender
{
public:
    LPCSTR getComment() override { return "INTERNAL: Dead Air screen-space sun shafts"; }
    BOOL canBeDetailed() override { return FALSE; }
    BOOL canBeLMAPped() override { return FALSE; }
    void Compile(CBlender_Compile& C) override;
};

class CBlender_fxaa final : public IBlender
{
public:
    LPCSTR getComment() override { return "INTERNAL: Dead Air FXAA"; }
    BOOL canBeDetailed() override { return FALSE; }
    BOOL canBeLMAPped() override { return FALSE; }
    void Compile(CBlender_Compile& C) override;
};

#if RENDER != R_R2
class CBlender_combine_msaa final : public IBlender
{
    pcstr Name{};
    pcstr Definition{};

public:
    CBlender_combine_msaa() = default;
    CBlender_combine_msaa(pcstr name, pcstr definition)
        : Name(name), Definition(definition) {}

    virtual LPCSTR getComment() { return "INTERNAL: combiner"; }
    virtual BOOL canBeDetailed() { return FALSE; }
    virtual BOOL canBeLMAPped() { return FALSE; }
    virtual void Compile(CBlender_Compile& C);
};
#endif
} // namespace xray::render::RENDER_NAMESPACE
