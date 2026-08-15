//---------------------------------------------------------------------------
#include "stdafx.h"
#pragma hdrstop

#include "SkeletonCustom.h"

namespace xray::render::RENDER_NAMESPACE
{
extern int psSkeletonUpdate;

#ifdef DEBUG
void check_kinematics(CKinematics* _k, LPCSTR s);
#endif

namespace
{
constexpr u64 CalculationTimeMask = u64(u32(-1));

IC u64 PublishedState(const u32 epoch, const u32 time) { return (u64(epoch) << 32) | time; }
IC u32 PublishedEpoch(const u64 state) { return u32(state >> 32); }
IC u32 PublishedTime(const u64 state) { return u32(state & CalculationTimeMask); }

IC bool IsPublished(const u64 state, const u32 epoch, const u32 time)
{
    return PublishedEpoch(state) == epoch && PublishedTime(state) == time;
}

class CalculationGuard final
{
    bool& m_in_progress;

public:
    explicit CalculationGuard(bool& in_progress) : m_in_progress(in_progress)
    {
        VERIFY(!m_in_progress);
        m_in_progress = true;
    }

    ~CalculationGuard() { m_in_progress = false; }
};
}

void CKinematics::CalculateBones(BOOL bForceExact)
{
    ZoneScoped;

    u32 calculation_time = Device.dwTimeGlobal;
    u64 published_state = UCalc_PublishedState.load(std::memory_order_acquire);
    u32 calculation_epoch = UCalc_Epoch.load(std::memory_order_acquire);
    if (IsPublished(published_state, calculation_epoch, calculation_time))
        return;

    UCalc_mtlock lock;
    calculation_time = Device.dwTimeGlobal;
    published_state = UCalc_PublishedState.load(std::memory_order_acquire);
    calculation_epoch = UCalc_Epoch.load(std::memory_order_acquire);
    if (IsPublished(published_state, calculation_epoch, calculation_time))
        return;

    // The global lock is recursive, so callbacks need an explicit same-object guard.
    if (UCalc_InProgress)
        return;
    const CalculationGuard calculation_guard(UCalc_InProgress);

    OnCalculateBones();
    published_state = UCalc_PublishedState.load(std::memory_order_acquire);
    calculation_epoch = UCalc_Epoch.load(std::memory_order_acquire);
    if (!bForceExact && PublishedEpoch(published_state) == calculation_epoch &&
        calculation_time < PublishedTime(published_state) + UCalc_Interval)
        return;

    if (Update_Visibility)
        Visibility_Update();

    _DBG_SINGLE_USE_MARKER;
    calculation_epoch = UCalc_Epoch.load(std::memory_order_acquire);

// exact computation
// Calculate bones
#ifdef DEBUG
    RImplementation.BasicStats.Animation.Begin();
#endif

    Bone_Calculate(bones->at(iRoot), &Fidentity);
#ifdef DEBUG
    check_kinematics(this, dbg_name.c_str());
    RImplementation.BasicStats.Animation.End();
#endif
    VERIFY(LL_GetBonesVisible() != 0);
    // Calculate BOXes/Spheres if needed
    const s32 visibox_update = UCalc_Visibox.fetch_add(1, std::memory_order_relaxed) + 1;
    if (visibox_update >= psSkeletonUpdate)
    {
        ZoneScopedN("Skeleton update");

        // mark
        UCalc_Visibox.store(-(::Random.randI(psSkeletonUpdate - 1)), std::memory_order_relaxed);

        // the update itself
        Fbox Box;
        Box.invalidate();
        for (u32 b = 0; b < bones->size(); b++)
        {
            if (!LL_GetBoneVisible(u16(b)))
                continue;
            Fobb& obb = (*bones)[b]->obb;
            Fmatrix& Mbone = bone_instances[b].mTransform;
            Fmatrix Mbox;
            obb.xform_get(Mbox);
            Fmatrix X;
            X.mul_43(Mbone, Mbox);
            const Fvector& halfSize = obb.m_halfsize;
            Fbox localBox;
            localBox.set(-halfSize.x, -halfSize.y, -halfSize.z, halfSize.x, halfSize.y, halfSize.z);
            Fbox transformedBox;
            transformedBox.xform(localBox, X);
            Box.merge(transformedBox);
        }
        if (bones->size())
        {
            // previous frame we have updated box - update sphere
            vis.box.vMin = (Box.vMin);
            vis.box.vMax = (Box.vMax);
            vis.box.getsphere(vis.sphere.P, vis.sphere.R);
        }
#ifdef DEBUG
        // Validate
        VERIFY3(_valid(vis.box.vMin) && _valid(vis.box.vMax), "Invalid bones-xform in model", dbg_name.c_str());
        if (vis.sphere.R > 1000.f)
        {
            for (u16 ii = 0; ii < LL_BoneCount(); ++ii)
            {
                Fmatrix tr;
                tr = LL_GetTransform(ii);
                Log("bone ", LL_BoneName_dbg(ii));
                Log("bone_matrix", tr);
            }
            Log("end-------");
        }
        VERIFY3(vis.sphere.R < 1000.f, "Invalid bones-xform in model", dbg_name.c_str());
#endif
    }

    //
    if (Update_Callback)
        Update_Callback(this);

    // Publish only a complete pose that no callback invalidated while calculating.
    if (UCalc_Epoch.load(std::memory_order_acquire) == calculation_epoch)
        UCalc_PublishedState.store(PublishedState(calculation_epoch, calculation_time), std::memory_order_release);
}

#ifdef DEBUG
void check_kinematics(CKinematics* _k, LPCSTR s)
{
    CKinematics* K = _k;
    Fmatrix& MrootBone = K->LL_GetBoneInstance(K->LL_GetBoneRoot()).mTransform;
    if (MrootBone.c.y > 10000)
    {
        Msg("all bones transform:--------[%s]", s);

        for (u16 ii = 0; ii < K->LL_BoneCount(); ++ii)
        {
            Fmatrix tr;

            tr = K->LL_GetTransform(ii);
            Log("bone ", K->LL_BoneName_dbg(ii));
            Log("bone_matrix", tr);
        }
        Log("end-------");
        VERIFY3(0, "check_kinematics failed for ", s);
    }
}
#endif

// Добавить скриптовое смещение для кости --#SM+#--
void CKinematics::LL_AddTransformToBone(KinematicsABT::additional_bone_transform& offset)
{
    m_bones_offsets.push_back(offset);
}

// Обнулить скриптовое смещение для конкретной кости или всех сразу (bone_id = BI_NONE) --#SM+#--
void CKinematics::LL_ClearAdditionalTransform(u16 bone_id)
{
    if (bone_id == BI_NONE)
    {
        m_bones_offsets.clear();
        return;
    }

    BONE_TRANSFORM_VECTOR_IT it = m_bones_offsets.begin();
    while (it != m_bones_offsets.end())
    {
        if (it->m_bone_id == bone_id)
        {
            it = m_bones_offsets.erase(it);
        }
        else
            ++it;
    }
}

void CKinematics::BuildBoneMatrix(
    const CBoneData* bd, CBoneInstance& bi, const Fmatrix* parent, u8 channel_mask /*= (1<<0)*/)
{
    bi.mTransform.mul_43(*parent, bd->bind_transform);
    CalculateBonesAdditionalTransforms(bd, bi, parent, channel_mask); //--#SM+#--
}

// Добавляем константные смещения к нужным костям --#SM+#--
void CKinematics::CalculateBonesAdditionalTransforms(
    const CBoneData* bd, CBoneInstance& bi, const Fmatrix* parent, u8 channel_mask /* = (1<<0)*/)
{
    // bi.mTransform.c - содержит смещение относительно первой кости модели\центра сцены (0, 0, 0)
    for (auto& it : m_bones_offsets)
    {
        if (it.m_bone_id == bd->GetSelfID())
        {
            const Fvector vOldPos = bi.mTransform.c;
            bi.mTransform.mulB_43(it.m_transform); // Rotation
            bi.mTransform.c.add(vOldPos, it.m_transform.c); // Translation
        }
    }
}

void CKinematics::CLBone(const CBoneData* bd, CBoneInstance& bi, const Fmatrix* parent, u8 channel_mask /*= (1<<0)*/)
{
    ZoneScoped;

    u16 SelfID = bd->GetSelfID();

    if (LL_GetBoneVisible(SelfID))
    {
        if (bi.callback_overwrite())
        {
            if (bi.callback())
                bi.callback()(&bi);
        }
        else
        {
            BuildBoneMatrix(bd, bi, parent, channel_mask);
#ifndef MASTER_GOLD
            R_ASSERT2(_valid(bi.mTransform), "anim kils bone matrix");
#endif // #ifndef MASTER_GOLD
            if (bi.callback())
            {
                bi.callback()(&bi);
#ifndef MASTER_GOLD
                R_ASSERT2(_valid(bi.mTransform), make_string("callback kils bone matrix bone: %s ", bd->name.c_str()));
#endif // #ifndef MASTER_GOLD
            }
        }
        bi.mRenderTransform.mul_43(bi.mTransform, bd->m2b_transform);
    }
}

void CKinematics::Bone_GetAnimPos(Fmatrix& pos, u16 id, u8 mask_channel, bool ignore_callbacks)
{
    ZoneScoped;

    R_ASSERT(id < LL_BoneCount());
    CBoneInstance bi = LL_GetBoneInstance(id);
    BoneChain_Calculate(&LL_GetData(id), bi, mask_channel, ignore_callbacks);
#ifndef MASTER_GOLD
    R_ASSERT(_valid(bi.mTransform));
#endif
    pos.set(bi.mTransform);
}

void CKinematics::Bone_Calculate(CBoneData* bd, Fmatrix* parent)
{
    ZoneScoped;

    u16 SelfID = bd->GetSelfID();
    CBoneInstance& BONE_INST = LL_GetBoneInstance(SelfID);
    CLBone(bd, BONE_INST, parent, u8(-1));
    // Calculate children
    for (xr_vector<CBoneData*>::iterator C = bd->children.begin(); C != bd->children.end(); ++C)
        Bone_Calculate(*C, &BONE_INST.mTransform);
}

void CKinematics::BoneChain_Calculate(const CBoneData* bd, CBoneInstance& bi, u8 mask_channel, bool ignore_callbacks)
{
    ZoneScoped;

    u16 SelfID = bd->GetSelfID();
    // CBlendInstance& BLEND_INST	= LL_GetBlendInstance(SelfID);
    // CBlendInstance::BlendSVec &Blend = BLEND_INST.blend_vector();
    // ignore callbacks
    BoneCallback bc = bi.callback();
    BOOL ow = bi.callback_overwrite();
    if (ignore_callbacks)
    {
        bi.set_callback(bi.callback_type(), nullptr, bi.callback_param(), 0);
    }
    if (SelfID == LL_GetBoneRoot())
    {
        CLBone(bd, bi, &Fidentity, mask_channel);
        // restore callback
        bi.set_callback(bi.callback_type(), bc, bi.callback_param(), ow);
        return;
    }
    u16 ParentID = bd->GetParentID();
    R_ASSERT(ParentID != BI_NONE);
    CBoneData* ParrentDT = &LL_GetData(ParentID);
    CBoneInstance parrent_bi = LL_GetBoneInstance(ParentID);
    BoneChain_Calculate(ParrentDT, parrent_bi, mask_channel, ignore_callbacks);
    CLBone(bd, bi, &parrent_bi.mTransform, mask_channel);
    // restore callback
    bi.set_callback(bi.callback_type(), bc, bi.callback_param(), ow);
}
} // namespace xray::render::RENDER_NAMESPACE
