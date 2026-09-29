#pragma once

namespace xray::render::RENDER_NAMESPACE
{
class ECORE_API R_xforms
{
public:
    Fmatrix m_w; // Basic	- world
    Fmatrix m_invw; // derived	- world2local, cached
    Fmatrix m_v; // Basic	- view
    Fmatrix m_invv; // derived - view2world, cached
    Fmatrix m_p; // Basic	- projection
    Fmatrix m_wv; // Derived	- world2view
    Fmatrix m_vp; // Derived	- view2projection
    Fmatrix m_wvp; // Derived	- world2view2projection

    // Motion vectors (shaders/r3/da_velocity.h). The view-projections without the jitter, this
    // frame's and the previous frame's: the main view's (set once a frame for every context) or
    // the HUD's while the HUD is drawn. They are separate from m_v/m_p on purpose: shadow and
    // light passes replace those and never read these.
    Fmatrix m_vp_nojit;
    Fmatrix m_vp_prev;
    // The world the drawn object had on the previous frame. set_W makes it the current one - no
    // motion of its own, only the camera's - and a mover that knows better calls set_W_prev
    // right after. Per draw like m_wvp: constant binders run once per pass, and a whole batch of
    // objects is drawn under one pass.
    Fmatrix m_w_prev;
    Fmatrix m_wvp_nojit; // derived: m_vp_nojit * m_w
    Fmatrix m_wvp_prev; // derived: m_vp_prev * m_w_prev
    // The main scene is being drawn into the G-buffer with the velocity target bound: movers
    // report their previous transforms and their motion history advances (CKinematics). False
    // everywhere else - a shadow pass would advance the history without drawing the motion.
    bool m_motion_pass{};

    R_constant* c_w;
    R_constant* c_invw;
    R_constant* c_v;
    R_constant* c_p;
    R_constant* c_wv;
    R_constant* c_vp;
    R_constant* c_wvp;
    R_constant* c_vp_nojit;
    R_constant* c_vp_prev;
    R_constant* c_wvp_nojit;
    R_constant* c_wvp_prev;
    R_constant* c_w_prev;

private:
    bool m_bInvWValid;
    bool m_bInvVValid;
    bool m_bMotionValid;

public:
    explicit R_xforms(CBackend& cmd_list_in);
    void unmap();
    void set_W(const Fmatrix& m);
    void set_W_prev(const Fmatrix& m);
    void set_V(const Fmatrix& m);
    void set_P(const Fmatrix& m);
    void set_VP_motion(const Fmatrix& vp_nojit, const Fmatrix& vp_prev);
    const Fmatrix& get_W() { return m_w; }
    const Fmatrix& get_V() { return m_v; }
    const Fmatrix& get_inv_V();
    const Fmatrix& get_P() { return m_p; }
    IC void set_c_w(R_constant* C);
    IC void set_c_invw(R_constant* C);
    IC void set_c_v(R_constant* C);
    IC void set_c_p(R_constant* C);
    IC void set_c_wv(R_constant* C);
    IC void set_c_vp(R_constant* C);
    IC void set_c_wvp(R_constant* C);
    IC void set_c_vp_nojit(R_constant* C);
    IC void set_c_vp_prev(R_constant* C);
    IC void set_c_wvp_nojit(R_constant* C);
    IC void set_c_wvp_prev(R_constant* C);
    IC void set_c_w_prev(R_constant* C);

private:
    void apply_invw();
    void update_motion();
    void apply_motion();

    CBackend& cmd_list;
};
} // namespace xray::render::RENDER_NAMESPACE
