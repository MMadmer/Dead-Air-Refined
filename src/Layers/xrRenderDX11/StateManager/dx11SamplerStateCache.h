#pragma once

namespace xray::render::RENDER_NAMESPACE
{
class dx11SamplerStateCache
{
public:
    enum
    {
        hInvalidHandle = 0xFFFFFFFF
    };

    //	State handle
    typedef u32 SHandle;
    typedef xr_vector<SHandle> HArray;

public:
    dx11SamplerStateCache();
    ~dx11SamplerStateCache();

    void ClearStateArray();
    void ResetContext(u32 contextId);

    SHandle GetState(D3D_SAMPLER_DESC& desc);

    void VSApplySamplers(u32 context_id, HArray& samplers);
    void PSApplySamplers(u32 context_id, HArray& samplers);
    void GSApplySamplers(u32 context_id, HArray& samplers);
    void HSApplySamplers(u32 context_id, HArray& samplers);
    void DSApplySamplers(u32 context_id, HArray& samplers);
    void CSApplySamplers(u32 context_id, HArray& samplers);

    void SetMaxAnisotropy(u32 uiMaxAniso);
    void SetMipLODBias(float uiMipLODBias);

    // The 3D scene's own sampler variant while an upscaler renders it below the display size: the
    // extra LOD bias on top of the global one, anisotropy at least minAniso. Every state gets a
    // twin once (not per frame - the DX12 layer must not see states churn), and a context uses
    // the twins while UseSceneBias is on for it. 0 and 0 drop the twins.
    void SetSceneMipBias(float extraBias, u32 minAniso);
    void UseSceneBias(u32 contextId, bool use);

private:
    enum class ShaderStage : u32
    {
        Vertex,
        Pixel,
        Geometry,
        Hull,
        Domain,
        Compute,
        Count
    };

    typedef ID3DSamplerState IDeviceState;
    typedef D3D_SAMPLER_DESC StateDecs;

    struct StateRecord
    {
        u32 m_crc;
        IDeviceState* m_pState;
        StateDecs m_desc;
        IDeviceState* m_pSceneState; // the scene variant (SetSceneMipBias), null without one
    };

private:
    void CreateState(StateDecs desc, IDeviceState** ppIState);
    void CreateSceneState(StateRecord& rec);
    SHandle FindState(const StateDecs& desc, u32 StateCRC);

    u32 PrepareSamplerStates(
        u32 contextId, ShaderStage stage, const HArray& samplers,
        ID3DSamplerState* pSS[D3D_COMMONSHADER_SAMPLER_SLOT_COUNT]);
    bool IsAlreadyBound(u32 contextId, ShaderStage stage, const HArray& samplers) const;
    void MarkBound(u32 contextId, ShaderStage stage, const HArray& samplers);
    void InvalidateBoundArrays();

    //	Private data
private:
    //	This must be cleared on device destroy
    xr_vector<StateRecord> m_StateArray;

    u32 m_uiMaxAnisotropy;
    float m_uiMipLODBias;
    float m_fSceneMipBias{};
    u32 m_uiSceneMinAniso{};
    bool m_bSceneBias[R__NUM_CONTEXTS]{};
    u8 m_boundSamplerCounts[R__NUM_CONTEXTS][static_cast<u32>(ShaderStage::Count)]{};
    const HArray* m_boundSamplerArrays[R__NUM_CONTEXTS][static_cast<u32>(ShaderStage::Count)]{};
};

extern dx11SamplerStateCache SSManager;
} // namespace xray::render::RENDER_NAMESPACE
