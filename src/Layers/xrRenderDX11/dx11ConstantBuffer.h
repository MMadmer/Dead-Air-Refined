#pragma once

namespace xray::render::RENDER_NAMESPACE
{
struct R_constant;
struct R_constant_load;

class dx11ConstantBuffer : public xr_resource_named
{
public:
    dx11ConstantBuffer(u32 contextId, ID3DShaderReflectionConstantBuffer* pTable);
    ~dx11ConstantBuffer();

    bool Similar(dx11ConstantBuffer& _in);
    ID3DBuffer* GetBuffer() { return m_pBuffer; }
    void Flush(u32 context_id);
    bool QueueForFlush();
    void CancelFlush() { m_queuedForFlush = false; }
    bool NeedsFlush() const { return m_bChanged; }

    //	Set copy data into constant buffer
    //	Plain buffer member
    void set(R_constant* C, R_constant_load& L, const Fmatrix& A);
    void set(R_constant* C, R_constant_load& L, const Fvector4& A);
    void set(R_constant* C, R_constant_load& L, float A);
    void set(R_constant* C, R_constant_load& L, int A);
    //	Array buffer member
    void seta(R_constant* C, R_constant_load& L, u32 e, const Fmatrix& A);
    void seta(R_constant* C, R_constant_load& L, u32 e, const Fvector4& A);

    void* AccessDirect(R_constant_load& L, size_t DataSize);

private:
    void Update(u16 offset, const void* data, size_t size);
    void MarkDirty(u16 offset, size_t size);
    void ResetDirtyRange();

private:
    u32 m_contextId;
    shared_str m_strBufferName;
    D3D_CBUFFER_TYPE m_eBufferType;

    //	Buffer data description
    u32 m_uiMembersCRC;
    xr_vector<D3D_SHADER_TYPE_DESC> m_MembersList;
    xr_vector<shared_str> m_MembersNames;

    ID3DBuffer* m_pBuffer;
    u32 m_uiBufferSize; //	Cache buffer size for debug validation
    void* m_pBufferData;
    void* m_pCommittedData;
    bool m_bChanged;
    bool m_hasCommittedData;
    bool m_queuedForFlush;
    bool m_knownDifferent;
    u32 m_dirtyBegin;
    u32 m_dirtyEnd;

    static const u32 lineSize = sizeof(Fvector4);

    //	Never try to copy objects of this class due to the pointer and autoptr members
    dx11ConstantBuffer(const dx11ConstantBuffer&);
    dx11ConstantBuffer& operator=(dx11ConstantBuffer&);
};

typedef resptr_core<dx11ConstantBuffer, resptr_base<dx11ConstantBuffer>> ref_cbuffer;
} // namespace xray::render::RENDER_NAMESPACE
