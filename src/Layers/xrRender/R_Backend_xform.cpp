#include "stdafx.h"
#pragma hdrstop

#include "R_Backend_xform.h"

namespace xray::render::RENDER_NAMESPACE
{
void R_xforms::set_W(const Fmatrix& m)
{
    m_w.set(m);
    m_wv.mul_43(m_v, m_w);
    m_wvp.mul(m_p, m_wv);
    if (c_w)
        cmd_list.set_c(c_w, m_w);
    if (c_wv)
        cmd_list.set_c(c_wv, m_wv);
    if (c_wvp)
        cmd_list.set_c(c_wvp, m_wvp);
    m_bInvWValid = false;
    if (c_invw)
        apply_invw();
    // Not a mover until it says so (set_W_prev).
    m_w_prev.set(m);
    m_bMotionValid = false;
    if (c_wvp_nojit || c_wvp_prev || c_w_prev)
        apply_motion();
    cmd_list.set_xform(D3DTS_WORLD, m);
}

void R_xforms::set_W_prev(const Fmatrix& m)
{
    m_w_prev.set(m);
    m_bMotionValid = false;
    if (c_wvp_nojit || c_wvp_prev || c_w_prev)
        apply_motion();
}

void R_xforms::set_VP_motion(const Fmatrix& vp_nojit, const Fmatrix& vp_prev)
{
    m_vp_nojit.set(vp_nojit);
    m_vp_prev.set(vp_prev);
    m_bMotionValid = false;
    if (c_vp_nojit)
        cmd_list.set_c(c_vp_nojit, m_vp_nojit);
    if (c_vp_prev)
        cmd_list.set_c(c_vp_prev, m_vp_prev);
    if (c_wvp_nojit || c_wvp_prev || c_w_prev)
        apply_motion();
}

// The derived motion matrices are built only when a shader asks for them: most draws never do.
void R_xforms::update_motion()
{
    if (m_bMotionValid)
        return;
    m_wvp_nojit.mul(m_vp_nojit, m_w);
    m_wvp_prev.mul(m_vp_prev, m_w_prev);
    m_bMotionValid = true;
}

void R_xforms::apply_motion()
{
    update_motion();
    if (c_wvp_nojit)
        cmd_list.set_c(c_wvp_nojit, m_wvp_nojit);
    if (c_wvp_prev)
        cmd_list.set_c(c_wvp_prev, m_wvp_prev);
    if (c_w_prev)
        cmd_list.set_c(c_w_prev, m_w_prev);
}

void R_xforms::set_V(const Fmatrix& m)
{
    m_v.set(m);
    m_bInvVValid = false;
    m_wv.mul_43(m_v, m_w);
    m_vp.mul(m_p, m_v);
    m_wvp.mul(m_p, m_wv);
    if (c_v)
        cmd_list.set_c(c_v, m_v);
    if (c_vp)
        cmd_list.set_c(c_vp, m_vp);
    if (c_wv)
        cmd_list.set_c(c_wv, m_wv);
    if (c_wvp)
        cmd_list.set_c(c_wvp, m_wvp);
    cmd_list.set_xform(D3DTS_VIEW, m);
}

const Fmatrix& R_xforms::get_inv_V()
{
    if (!m_bInvVValid)
    {
        m_invv.invert(m_v);
        m_bInvVValid = true;
    }

    return m_invv;
}

void R_xforms::set_P(const Fmatrix& m)
{
    m_p.set(m);
    m_vp.mul(m_p, m_v);
    m_wvp.mul(m_p, m_wv);
    if (c_p)
        cmd_list.set_c(c_p, m_p);
    if (c_vp)
        cmd_list.set_c(c_vp, m_vp);
    if (c_wvp)
        cmd_list.set_c(c_wvp, m_wvp);
    // always setup projection - D3D relies on it to work correctly :(
    cmd_list.set_xform(D3DTS_PROJECTION, m);
}

void R_xforms::apply_invw()
{
    VERIFY(c_invw);

    if (!m_bInvWValid)
    {
        m_invw.invert_b(m_w);
        m_bInvWValid = true;
    }

    cmd_list.set_c(c_invw, m_invw);
}

void R_xforms::unmap()
{
    c_w = nullptr;
    c_invw = nullptr;
    c_v = nullptr;
    c_p = nullptr;
    c_wv = nullptr;
    c_vp = nullptr;
    c_wvp = nullptr;
    c_vp_nojit = nullptr;
    c_vp_prev = nullptr;
    c_wvp_nojit = nullptr;
    c_wvp_prev = nullptr;
    c_w_prev = nullptr;
}
R_xforms::R_xforms(CBackend& cmd_list_in)
    : cmd_list(cmd_list_in)
{
    unmap();
    m_w.identity();
    m_invw.identity();
    m_v.identity();
    m_invv.identity();
    m_p.identity();
    m_wv.identity();
    m_vp.identity();
    m_wvp.identity();
    m_vp_nojit.identity();
    m_vp_prev.identity();
    m_w_prev.identity();
    m_wvp_nojit.identity();
    m_wvp_prev.identity();
    m_bInvWValid = true;
    m_bInvVValid = true;
    m_bMotionValid = true;
}
} // namespace xray::render::RENDER_NAMESPACE
