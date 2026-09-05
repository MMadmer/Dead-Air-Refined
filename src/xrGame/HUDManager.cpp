#include "StdAfx.h"
#include "HUDManager.h"
#include "HUDTarget.h"
#include "Actor.h"
#include "xrEngine/IGame_Level.h"
#include "xrEngine/xr_input.h"
#include "GamePersistent.h"
#include "MainMenu.h"
#include "Grenade.h"
#include "Spectator.h"
#include "Car.h"
#include "UIGameCustom.h"
#include "xrUICore/Cursor/UICursor.h"
#include "game_cl_base.h"
#include "da_pda3d.h"
#include "ui/UIPdaWnd.h"
#include "xrUICore/Static/UIStatic.h"
#ifdef DEBUG
#include "PHDebug.h"
#endif

extern CUIGameCustom* CurrentGameUI() { return HUD().GetGameUI(); }

//--------------------------------------------------------------------
CHUDManager::CHUDManager() : m_pHUDTarget(xr_new<CHUDTarget>())
{
    // A controller psy hit and a bloodsucker execution hide the HUD for the duration of the
    // attack and restore it when the attack ends. psHUD_Flags is engine-global, so loading a
    // save mid-attack destroys the attacker without ever restoring the flag and the player is
    // left with no HUD until another attack completes. The manager is rebuilt with the level,
    // so this is the point where a session can no longer owe anyone a restore.
    psHUD_Flags.set(HUD_DRAW_RT2, TRUE);
}
//--------------------------------------------------------------------
CHUDManager::~CHUDManager()
{
    OnDisconnected();

    if (pUIGame)
        pUIGame->UnLoad();

    xr_delete(pUIGame);
    xr_delete(m_pHUDTarget);
    xr_delete(m_pda_rt_dbg);
}

//--------------------------------------------------------------------
void CHUDManager::OnFrame()
{
    ZoneScoped;

    if (!psHUD_Flags.is(HUD_DRAW_RT2))
        return;

    if (!b_online)
        return;

    // 3D PDA screen state (interference/power/boot -> m_affects, face sub-rect) is a
    // once-per-frame service regardless of whether the presenter is up.
    da_pda3d::update();

    if (pUIGame)
        pUIGame->OnFrame();

    m_pHUDTarget->CursorOnFrame();
}
//--------------------------------------------------------------------

void CHUDManager::Render_First(u32 context_id)
{
    ZoneScoped;

    // Gates match Render_Actor_Body in the reference: this entry point no longer draws the
    // weapon HUD, so the psHUD_Flags mask it used to share does not apply. HUD_WEAPON is not
    // in the engine default mask and HUD_WEAPON_RT clears while leaning, which switched the
    // body off on a stock profile; HUDview() adds focus and holder checks for the same reason.
    if (0 == pUIGame)
        return;
    IGameObject* O = g_pGameLevel->CurrentViewEntity();
    if (0 == O)
        return;
    CActor* A = smart_cast<CActor*>(O);
    // The legs mesh stays off for the landing roll and while a script owns the camera (the ledge
    // climb): the camera leaves the head, so the body would tumble or hang in the frame. The world
    // shadow comes from the separate caster (render_shadow_caster) and keeps going - a shadow that
    // is there beats a shadow that is right.
    if (!A || !A->g_Alive() || A->active_cam() != eacFirstEye || (A->MovingState() & mcClimb) || A->IsFallRolling() ||
        A->script_cam())
        return;

    // R1 keeps the actor hidden and renders only its shadow; newer renderers draw the first-person body.
    {
        const auto root = O->H_Root();
        ScopeLock lock{ &render_lock };
        const bool wasInvisible = root->renderable_Invisible();
        root->renderable_Invisible(GEnv.Render->GenerationIsR1());
        A->renderable_RenderBody(context_id, root);
        root->renderable_Invisible(wasInvisible);
    }
}

bool need_render_hud()
{
    if (Device.IsAnselActive)
        return false;

    IGameObject* O = g_pGameLevel ? g_pGameLevel->CurrentViewEntity() : NULL;
    if (0 == O)
        return false;

    CActor* A = smart_cast<CActor*>(O);
    if (A && (!A->HUDview() || !A->g_Alive()))
        return false;

    if (smart_cast<CCar*>(O) || smart_cast<CSpectator*>(O))
        return false;

    return true;
}

void CHUDManager::Render_Last(u32 context_id)
{
    ZoneScoped;

    if (!psHUD_Flags.is(HUD_WEAPON | HUD_WEAPON_RT | HUD_WEAPON_RT2 | HUD_DRAW_RT2))
        return;
    if (0 == pUIGame)
        return;

    if (!need_render_hud())
        return;

    IGameObject* O = g_pGameLevel->CurrentViewEntity();
    // hud itself
    {
        const auto root = O->H_Root();
        ScopeLock lock{ &render_lock };
        root->renderable_HUD(true);
        O->OnHUDDraw(context_id, this, root);
        root->renderable_HUD(false);
    }
}

#include "player_hud.h"

void CHUDManager::Render_Shadow(u32 context_id)
{
    ZoneScoped;

    if (!psHUD_Flags.is(HUD_WEAPON | HUD_WEAPON_RT | HUD_WEAPON_RT2 | HUD_DRAW_RT2))
        return;
    if (0 == pUIGame)
        return;
    if (!need_render_hud())
        return;
    if (!g_player_hud)
        return;

    IGameObject* O = g_pGameLevel->CurrentViewEntity();
    if (0 == O)
        return;

    // renderable_HUD deliberately stays false here: the caster has to enter the shadow graph
    // as ordinary dynamic geometry, because mapHUD is drawn by the main pass only and anything
    // parked there would never reach a shadow map. The same lock as the other two entry points
    // keeps the HUD state consistent while the main pass builds its own graph in parallel.
    const auto root = O->H_Root();
    ScopeLock lock{ &render_lock };
    g_player_hud->render_shadow(context_id, root);
}

// The actor is dropped from the spatial database while it is in first person, so its world
// shadow has to be driven by hand: the caster is prepared here once per frame, and every
// shadow-map pass asks for it separately.
void CHUDManager::Update_Actor_Shadow(bool enabled)
{
    ZoneScoped;

    if (!g_pGameLevel)
        return;

    CActor* A = smart_cast<CActor*>(g_pGameLevel->CurrentViewEntity());
    if (!A)
        return;

    ScopeLock lock{ &render_lock };
    A->update_shadow_caster(enabled);
}

void CHUDManager::Render_Actor_Shadow(u32 context_id, const Fvector& source)
{
    ZoneScoped;

    if (!g_pGameLevel)
        return;

    CActor* A = smart_cast<CActor*>(g_pGameLevel->CurrentViewEntity());
    if (!A)
        return;

    A->render_shadow_caster(context_id, A->H_Root(), source);
}

bool CHUDManager::RenderActiveItemUIQuery()
{
    if (!psHUD_Flags.is(HUD_DRAW_RT2))
        return false;

    if (!psHUD_Flags.is(HUD_WEAPON | HUD_WEAPON_RT | HUD_WEAPON_RT2))
        return false;

    if (!need_render_hud())
        return false;

    return (g_player_hud && g_player_hud->render_item_ui_query());
}

void CHUDManager::RenderActiveItemUI()
{
    if (!psHUD_Flags.is(HUD_DRAW_RT2))
        return;

    g_player_hud->render_item_ui();
}

// ---- 3D PDA: the UI->texture pass -------------------------------------------------------
static CUIPdaWnd* pda_for_rt(CUIGameCustom* ui_game, bool online)
{
    if (GEnv.isDedicatedServer || !online || !ui_game)
        return nullptr;
    if (!da_pda3d::want_rt())
        return nullptr;
    CUIPdaWnd* pda = ui_game->GetPdaMenuPtr();
    return (pda && pda->IsShown()) ? pda : nullptr;
}

bool CHUDManager::RenderPdaScreenUIQuery()
{
    if (!pda_for_rt(pUIGame, b_online))
        return false;
    // Throttle the "held in hands" stage: no cursor, no interaction - a ~15 Hz refresh is
    // indistinguishable there (the RT is persistent, a skipped frame keeps the last image,
    // and a skipped CUIPdaWnd::Draw queues no glyphs, so nothing leaks into the main frame).
    // Focused stage runs at full rate.
    if (!da_pda3d::ui_focused() && g_pda3d_dbg == 0)
    {
        static u32 next_frame = 0;
        if (Device.dwFrame < next_frame)
            return false;
        next_frame = Device.dwFrame + 4;
    }
    return true;
}

bool CHUDManager::RenderPdaScreenUI()
{
    CUIPdaWnd* pda = pda_for_rt(pUIGame, b_online);
    if (!pda)
        return false;

    // Same recipe as CMainMenu::OnRenderPPUI_main, and the exact bug IX-Ray shipped without:
    // the FONT FLUSH must happen while the PDA texture is still bound, or every glyph queued
    // here spills onto the backbuffer later and the screen shows a text-free dialog. No
    // pp_start: the RT is device-sized and the regular in-game scale must match the 2D
    // dialog pixel for pixel. Note the queue is naturally empty at this point - this pass
    // runs before any other UI Draw of the frame (CLevel::OnRender entry).
    pda->SetInRTPass(true);
    pda->Draw();
    pda->SetInRTPass(false);
    if (pda->NeedCursor())
        GetUICursor().RenderToPdaScreen();
    UI().RenderFont();
    pda->MarkRasterizedToRT();
    return true;
}

extern ENGINE_API bool bShowPauseString;
//отрисовка элементов интерфейса
void CHUDManager::RenderUI()
{
    ZoneScoped;

    if (!psHUD_Flags.is(HUD_DRAW_RT2))
        return;

    if (!b_online)
        return;

    if (true /*|| psHUD_Flags.is(HUD_DRAW | HUD_DRAW_RT)*/)
    {
        HitMarker.Render();
        if (pUIGame)
            pUIGame->Render();

        // pda3d_dbg 2: blit $user$ui fullscreen over the frame - a pixel-for-pixel view of
        // what the PDA screen material will sample. Lazy static widget, debug-only cost.
        if (g_pda3d_dbg > 1)
        {
            if (!m_pda_rt_dbg)
            {
                m_pda_rt_dbg = xr_new<CUIStatic>("pda3d_rt_dbg");
                m_pda_rt_dbg->InitTexture("$user$ui");
                m_pda_rt_dbg->SetWndRect(Frect().set(0.f, 0.f, UI_BASE_WIDTH, UI_BASE_HEIGHT));
                m_pda_rt_dbg->SetTextureRect(Frect().set(0.f, 0.f, float(Device.dwWidth), float(Device.dwHeight)));
                m_pda_rt_dbg->SetAutoDelete(false);
            }
            m_pda_rt_dbg->Update();
            m_pda_rt_dbg->Draw();
        }

        UI().RenderFont();
    }

    m_pHUDTarget->Render();

    if (Device.Paused() && bShowPauseString)
    {
        CGameFont* pFont = UI().Font().pFontGraffiti50Russian;
        pFont->SetColor(0x80FF0000);
        LPCSTR _str = StringTable().translate("st_game_paused").c_str();

        Fvector2 _pos;
        _pos.set(UI_BASE_WIDTH / 2.0f, UI_BASE_HEIGHT / 2.0f);
        UI().ClientToScreenScaled(_pos);
        pFont->SetAligment(CGameFont::alCenter);
        pFont->Out(_pos.x, _pos.y, _str);
        pFont->OnRender();
    }
}

void CHUDManager::OnEvent(EVENT E, u64 P1, u64 P2) {}
collide::rq_result& CHUDManager::GetCurrentRayQuery() { return m_pHUDTarget->GetRQ(); }
void CHUDManager::SetCrosshairDisp(float dispf, float disps)
{
    m_pHUDTarget->GetHUDCrosshair().SetDispersion(psHUD_Flags.test(HUD_CROSSHAIR_DYNAMIC) ? dispf : disps);
}

#ifdef DEBUG
void CHUDManager::SetFirstBulletCrosshairDisp(float fbdispf)
{
    m_pHUDTarget->GetHUDCrosshair().SetFirstBulletDispertion(fbdispf);
}
#endif

void CHUDManager::ShowCrosshair(bool show) { m_pHUDTarget->ShowCrosshair(show); }
void CHUDManager::HitMarked(const Fvector& dir)
{
    HitMarker.Hit(dir);
}

bool CHUDManager::AddGrenade_ForMark(CGrenade* grn) { return HitMarker.AddGrenade_ForMark(grn); }
void CHUDManager::Update_GrenadeView(Fvector& pos_actor) { HitMarker.Update_GrenadeView(pos_actor); }
void CHUDManager::SetHitmarkType(LPCSTR tex_name) { HitMarker.InitShader(tex_name); }
void CHUDManager::SetGrenadeMarkType(LPCSTR tex_name) { HitMarker.InitShader_Grenade(tex_name); }
// ------------------------------------------------------------------------------------

void CHUDManager::Load()
{
    ZoneScoped;

    if (!pUIGame)
    {
        pUIGame = Game().createGameUI();
    }
    else
    {
        pUIGame->SetClGame(&Game());
    }
}

void CHUDManager::OnUIReset()
{
    ZoneScoped;

    // The debug blit widget holds a shader on $user$ui - rebuild it lazily after a reset.
    xr_delete(m_pda_rt_dbg);

    if (!pUIGame)
        return;

    pUIGame->HideShownDialogs();

    pUIGame->UnLoad();
    pUIGame->Load();

    pUIGame->OnConnected();
}

void CHUDManager::OnDisconnected()
{
    ZoneScoped;

    b_online = false;
    if (pUIGame)
        Device.seqFrame.Remove(pUIGame);
}

void CHUDManager::OnConnected()
{
    if (b_online)
        return;

    ZoneScoped;

    b_online = true;
    if (pUIGame)
        Device.seqFrame.Add(pUIGame, REG_PRIORITY_LOW - 1000);
}

void CHUDManager::net_Relcase(IGameObject* obj)
{
    ZoneScoped;

    HitMarker.net_Relcase(obj);

    VERIFY(m_pHUDTarget);
    m_pHUDTarget->net_Relcase(obj);
#ifdef DEBUG
    DBG_PH_NetRelcase(obj);
#endif
}

CDialogHolder* CurrentDialogHolder()
{
    if (MainMenu()->IsActive())
        return MainMenu();
    else
        return HUD().GetGameUI();
}
