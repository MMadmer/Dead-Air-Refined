#pragma once
#include "firedeps.h"

#include "Include/xrRender/Kinematics.h"
#include "Include/xrRender/KinematicsAnimated.h"
#include "actor_defs.h"

class player_hud;
class CHudItem;
class CMotionDef;

struct motion_descr
{
    MotionID mid;
    shared_str name;
};

struct player_hud_motion
{
    shared_str m_base_name;
    shared_str m_additional_name;
    xr_vector<motion_descr> m_animations;
    float m_anim_speed;
};

struct player_hud_motion_container
{
    xr_unordered_map<shared_str, player_hud_motion> m_anims;

    [[nodiscard]]
    const player_hud_motion* find_motion(const shared_str& name) const;

    // lenient: a cycle missing from the hands model is logged and skipped instead of asserting -
    // scene sections come from an addon whose motion sets may not be attached to every rig.
    void load(IKinematicsAnimated* model, const shared_str& sect, bool lenient = false);
};

struct hud_item_measures
{
    enum
    {
        e_fire_point = (1 << 0),
        e_fire_point2 = (1 << 1),
        e_shell_point = (1 << 2),
        e_16x9_mode_now = (1 << 3)
    };

    Fvector m_hands_offset[2][3]{}; // pos,rot/ normal,aim,GL
    Fvector m_hands_attach[2]{}; // pos,rot
    Fvector m_item_attach[2]{}; // pos,rot

    Fvector m_fire_point_offset{};
    Fvector m_fire_point2_offset{};
    Fvector m_shell_point_offset{};

    u16 m_fire_bone;
    u16 m_fire_bone2;
    u16 m_shell_bone;
    Flags8 m_prop_flags;

    Fmatrix load(const shared_str& sect_name, IKinematics* K);
    Fmatrix load_monolithic(const shared_str& sect_name, IKinematics* K, CHudItem* owner);
    void load_inertion_params(const shared_str& sect_name);
    void update(Fmatrix& attach_offset);

    struct inertion_params
    {
        float m_pitch_offset_r;
        float m_pitch_offset_n;
        float m_pitch_offset_d;
        float m_pitch_low_limit;
        float m_origin_offset;
        float m_origin_offset_aim;
        float m_tendto_speed;
        float m_tendto_speed_aim;
    };
    inertion_params m_inertion_params; //--#SM+#--
};

struct attachable_hud_item
{
    player_hud* m_parent{};
    CHudItem* m_parent_hud_item{};
    shared_str m_sect_name;
    shared_str m_visual_name;
    IKinematics* m_model{};
    u16 m_attach_place_idx{};
    bool m_monolithic{};
    hud_item_measures m_measures;

    // runtime positioning
    Fmatrix m_attach_offset{};
    Fmatrix m_item_transform{};

    player_hud_motion_container m_hand_motions;

    attachable_hud_item(player_hud* parent, const shared_str& sect_name, IKinematicsAnimated* model);
    ~attachable_hud_item();

    void reload_measures();

    void update(bool bForce);
    void update_hud_additional(Fmatrix& trans) const;

    void setup_firedeps(firedeps& fd);

    void render(u32 context_id, IRenderable* root);
    void render_item_ui() const;
    bool render_item_ui_query() const;
    bool need_renderable() const;
    void set_bone_visible(const shared_str& bone_name, BOOL bVisibility, BOOL bSilent = FALSE);

    // hands bind position
    Fvector& hands_attach_pos();
    Fvector& hands_attach_rot();

    // hands runtime offset
    Fvector& hands_offset_pos();
    Fvector& hands_offset_rot();

    // props
    u32 m_upd_firedeps_frame{ u32(-1) };
    void tune(Ivector values);
    u32 anim_play(const shared_str& anim_name, BOOL bMixIn, const CMotionDef*& md, u8& rnd);
};

class player_hud
{
public:
    player_hud() = default;
    ~player_hud();
    void load(const shared_str& model_name);
    void load_default() { load("actor_hud_05"); };
    void update(const Fmatrix& trans);
    void render_hud(u32 context_id, IRenderable* root);
    void render_shadow(u32 context_id, IRenderable* root);
    void render_item_ui() const;
    bool render_item_ui_query() const;
    u32 anim_play(u16 part, const MotionID& M, BOOL bMixIn, const CMotionDef*& md, float speed, IKinematicsAnimated* itemModel);
    const shared_str& section_name() const { return m_sect_name; }
    attachable_hud_item* create_hud_item(const shared_str& sect);

    void attach_item(CHudItem* item);
    bool allow_activation(CHudItem* item) const;
    attachable_hud_item* attached_item(u16 item_idx) { return m_attached_items[item_idx]; };
    void detach_item_idx(u16 idx);
    void detach_item(CHudItem* item);
    void detach_all_items()
    {
        // Through detach_item_idx so on_b_hud_detach still runs: clearing the slots behind the
        // items' backs left them believing they were still attached, and the pooled hud models
        // kept whatever state the last owner had put them in.
        // Left hand first: detach_item_idx only restores the right hand's animation
        // partition while slot 0 is still occupied.
        detach_item_idx(1);
        detach_item_idx(0);
    };

    void calc_transform(u16 attach_slot_idx, const Fmatrix& offset, Fmatrix& result) const;

    // The hands are TWO copies of one model: the right half (m_model, left arm hidden) and the
    // left half (m_model_2, right arm hidden), each with its own matrix, seat and cycles. That
    // is what lets a one-hand scene take one hand while the weapon keeps the other, seated where
    // its own section puts it. pid 0 = both, 1 = left, 2 = right; script_anim bypasses the
    // ownership lock a running scene holds on a hand.
    void play_blend(u16 pid, const MotionID& M, BOOL bMixIn, float speed, bool script_anim);
    // Seat of a half: 0 = right, 1 = left. Without its own item a half takes the other's seat;
    // with no item at all, the scene's.
    Fvector attach_pos(u8 part) const;
    Fvector attach_rot(u8 part) const;

    // Script scenes (game.play_hud_motion): a hand cycle from any hud section, with an optional
    // item in the hand and no CHudItem behind it. Length in ms, 0 = nothing to play; target_ms
    // stretches the cycle to the scene length, 0 plays it as recorded.
    u32 scene_motion_length(pcstr section, pcstr anim, float speed);
    u32 scene_play(u8 hand, pcstr section, pcstr anim, bool mix_in, float speed, u32 target_ms = 0);
    void scene_stop();
    bool scene_active() const;
    void scene_item_tune(Fvector& pos, Fvector& rot, float& scale) const
    {
        pos = m_scene_item_pos;
        rot = m_scene_item_rot;
        scale = m_scene_item_scale;
    }
    void tune(Ivector values);
    u32 motion_length(const MotionID& M, const CMotionDef*& md, float speed, IKinematicsAnimated* itemModel) const;
    u32 motion_length(const shared_str& anim_name, const shared_str& hud_name, const CMotionDef*& md);
    void OnMovementChanged(ACTOR_DEFS::EMoveCommand cmd) const;

private:
    void load_ancors();
    void update_inertion(Fmatrix& trans) const;
    void update_additional(Fmatrix& trans) const;
    bool inertion_allowed() const;
    void scene_item_release();
    // The scene item's matrix: the grip bone of the half that plays, plus the section's own
    // seat. Shared by the frame update and the scene's first frame.
    void scene_item_calc_transform();
    // Everything the frame update does for a scene, done once at the moment it starts.
    void scene_first_frame();
    player_hud_motion_container& scene_motions(const shared_str& sect);

private:
    shared_str m_sect_name;

    Fmatrix m_attach_offset{};

    Fmatrix m_transform{ Fidentity };
    IKinematicsAnimated* m_model{};
    IKinematicsAnimated* m_model_2{};
    Fmatrix m_transform_2{ Fidentity };
    Fmatrix m_attach_offset_2{};

    // Scene state. Motion sets are cached per section: loading walks the config and looks each
    // cycle up in the hands model, and a section is played on every climb or bite.
    xr_map<shared_str, player_hud_motion_container> m_scene_motions;
    u32 m_scene_end{};
    bool m_scene_on{};
    Fvector m_scene_hands_pos{};
    Fvector m_scene_hands_rot{}; // degrees
    bool m_scene_one_hand{};
    // The hand the scene owns: 0 right, 1 left, 2 both, u8(-1) none. While owned, the items'
    // cycles skip it. The seat of the owned hand slides toward the scene seat (k 0..1); the hand
    // whose seat slides is remembered apart, because ownership ends at once while the seat still
    // returns for a moment - to the same hand.
    u8 m_scene_hand{ u8(-1) };
    float m_scene_seat_k{};
    u8 m_scene_hand_seat{ 1 };
    // The scene item (a bottle, a bag): owned here, created on play, deleted on stop. The
    // animated view may be null: a model without motions is held still.
    IRenderVisual* m_scene_item_visual{};
    IKinematicsAnimated* m_scene_item_model{};
    Fmatrix m_scene_item_offset{};
    Fvector m_scene_item_pos{};
    Fvector m_scene_item_rot{}; // degrees
    float m_scene_item_scale{ 1.f };
    u16 m_scene_item_attach{};
    bool m_scene_item_attached{ true };
    bool m_scene_item_root_lock{ true };
    bool m_scene_item_lead_gun{};
    Fmatrix m_scene_item_transform{};
    // The camera matrix of the last update, before any item's own additions: a scene starts
    // after that update and needs a base for its first frame.
    Fmatrix m_last_cam_trans{ Fidentity };

    // Bones are the same in both copies (one model), so one anchor list serves both.
    xr_vector<u16> m_ancors;
    attachable_hud_item* m_attached_items[2]{};
    xr_unordered_map<shared_str, attachable_hud_item*> m_pool;
};

extern player_hud* g_player_hud;
