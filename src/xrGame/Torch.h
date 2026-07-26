#pragma once

#include "inventory_item_object.h"
#include "HudSound.h"

class CLAItem;
class CNightVisionEffector;

class CTorch : public CInventoryItemObject
{
private:
    typedef CInventoryItemObject inherited;

protected:
    float fBrightness;
    CLAItem* lanim;

    u16 guid_bone;
    shared_str light_trace_bone;

    float m_delta_h;
    Fvector2 m_prev_hp;
    bool m_switched_on;
    ref_light light_render;
    ref_light light_render2;
    ref_light light_omni;
    ref_glow glow_render;
    Fvector m_focus;
    Fcolor m_torch_color;
    Fcolor m_torch2_color;
    float m_torch_inertion;

private:
    inline bool can_use_dynamic_lights();

public:
    CTorch();
    virtual ~CTorch();

    virtual void Load(LPCSTR section);
    virtual bool net_Spawn(CSE_Abstract* DC);
    virtual void net_Destroy();
    virtual void net_Export(NET_Packet& P); // export to server
    virtual void net_Import(NET_Packet& P); // import from server

    virtual void OnH_A_Chield();
    virtual void OnH_B_Independent(bool just_before_destroy);

    virtual void UpdateCL();

    void Switch();
    void Switch(bool light_on);
    void Switch2(bool light_on);
    bool torch_active() const;

    void SetTorchSpot(bool spot);
    void SetTorchRadius(float value);
    void SetTorchRange(float value);
    void SetTorchInertion(float value);
    void SetTorchColorR(float value);
    void SetTorchColorG(float value);
    void SetTorchColorB(float value);
    void SetTorchColorA(float value);
    void SetTorchOffsetX(float value);
    void SetTorchOffsetY(float value);
    void SetTorchOffsetZ(float value);
    void SetTorchAnimation(LPCSTR value);
    void SetTorchTexture(LPCSTR value);

    void SetTorch2Radius(float value);
    void SetTorch2Range(float value);
    void SetTorch2ColorR(float value);
    void SetTorch2ColorG(float value);
    void SetTorch2ColorB(float value);
    void SetTorch2ColorA(float value);
    void SetTorch2OffsetX(float value);
    void SetTorch2OffsetY(float value);
    void SetTorch2OffsetZ(float value);

    virtual bool can_be_attached() const;

    // CAttachableItem
    virtual void enable(bool value);

public:
    void SwitchNightVision();
    void SwitchNightVision(bool light_on, bool use_sounds = true);

    bool GetNightVisionStatus() { return m_bNightVisionOn; }
    CNightVisionEffector* GetNightVision() { return m_night_vision; }
protected:
    bool m_bNightVisionEnabled;
    bool m_bNightVisionOn;

    CNightVisionEffector* m_night_vision;
    HUD_SOUND_COLLECTION m_sounds;

    enum EStats
    {
        eTorchActive = (1 << 0),
        eNightVisionActive = (1 << 1),
        eAttached = (1 << 2)
    };

public:
    virtual bool use_parent_ai_locations() const { return (!H_Parent()); }
    virtual void create_physic_shell();
    virtual void activate_physic_shell();
    virtual void setup_physic_shell();

    virtual void afterDetach();

private:
    DECLARE_SCRIPT_REGISTER_FUNCTION(CGameObject);
};

class CNightVisionEffector
{
    CActor* m_pActor;
    HUD_SOUND_COLLECTION m_sounds;

public:
    enum EPlaySounds
    {
        eStartSound = 0,
        eStopSound,
        eIdleSound,
        eBrokeSound
    };
    CNightVisionEffector(const shared_str& sect);
    void Start(const shared_str& sect, CActor* pA, bool play_sound = true);
    void Stop(const float factor, bool play_sound = true);
    bool IsActive();
    void OnDisabled(CActor* pA, bool play_sound = true);
    void PlaySounds(EPlaySounds which);
};
