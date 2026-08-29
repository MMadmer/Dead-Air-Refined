#pragma once
#include "xrEngine/EngineAPI.h"
#include "xrEngine/EventAPI.h"
#include "xrEngine/pure.h"

ENGINE_API extern Flags32 psHUD_Flags;
#define HUD_CROSSHAIR (1 << 0)
#define HUD_CROSSHAIR_DIST (1 << 1)
#define HUD_WEAPON (1 << 2)
#define HUD_INFO (1 << 3)
#define HUD_DRAW (1 << 4)
#define HUD_CROSSHAIR_RT (1 << 5)
#define HUD_WEAPON_RT (1 << 6)
#define HUD_CROSSHAIR_DYNAMIC (1 << 7)
#define HUD_CROSSHAIR_RT2 (1 << 9)
#define HUD_DRAW_RT (1 << 10)
#define HUD_WEAPON_RT2 (1 << 11)
#define HUD_DRAW_RT2 (1 << 12)
#define HUD_DRAW_MAP (1 << 13)
#define HUD_DRAW_INFO (1 << 14)
#define HUD_LEFT_HANDED (1 << 15)
#define HUD_CROSSHAIR_ITEM (1 << 16)
#define HUD_CROSSHAIR_WEAPON (1 << 17)
#define HUD_CROSSHAIR_NEAREST (1 << 18)
#define HUD_CROSSHAIR_DOT (1 << 19)

class IGameObject;

class ENGINE_API XR_NOVTABLE CCustomHUD
    : public IEventReceiver,
      public CUIResetNotifier
{
public:
    virtual void Render_First(u32 context_id) = 0;
    virtual void Render_Last(u32 context_id) = 0;
    // The first-person hands and item as shadow casters. Separate from Render_Last because a
    // caster must not be marked as HUD geometry: mapHUD belongs to the main pass alone.
    virtual void Render_Shadow(u32 context_id) = 0;

    // The first-person actor's world shadow. Prepared once per frame on the main thread, then
    // handed to every shadow map by hand - the hidden actor is not in the spatial database.
    virtual void Update_Actor_Shadow(bool enabled) = 0;
    virtual void Render_Actor_Shadow(u32 context_id, const Fvector& source) = 0;

    virtual void OnFrame() = 0;
    virtual void Load() = 0;
    virtual void OnDisconnected() = 0;
    virtual void OnConnected() = 0;
    virtual void RenderActiveItemUI() = 0;
    virtual bool RenderActiveItemUIQuery() = 0;
    // 3D PDA: does anything want the PDA rasterized into $user$ui this frame? Cheap
    // predicate the renderer asks before paying for the bind+clear.
    virtual bool RenderPdaScreenUIQuery() = 0;
    // 3D PDA: rasterize the PDA dialog (widgets, hints, cursor, FONTS) into the currently
    // bound render target. Called by the renderer with $user$ui bound, before the world
    // passes. Returns true if anything was drawn - the same frame's ordinary 2D pass is
    // then suppressed by the dialog itself.
    virtual bool RenderPdaScreenUI() = 0;
    virtual void net_Relcase(IGameObject* object) = 0;
};
