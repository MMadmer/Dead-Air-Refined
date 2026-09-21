#include "pch_script.h"
#include "xrEngine/XR_IOConsole.h"
#include "xrCore/XMS/xms_core.h"
#include "xrMaterialSystem/GameMtlLib.h"
#include "xrEngine/Rain.h"
#include "xrEngine/xr_ioc_cmd.h"
#include "xrEngine/CustomHUD.h"
#include "xrEngine/FDemoRecord.h"
#include "xrEngine/FDemoPlay.h"
#include "xrMessages.h"
#include "xrServer.h"
#include "Level.h"
#include "xrScriptEngine/script_debugger.hpp"
#include "ai_debug.h"
#include "alife_simulator.h"
#include "alife_object_registry.h"
#include "game_cl_base.h"
#include "game_cl_single.h"
#include "game_sv_single.h"
#include "Hit.h"
#include "PHDestroyable.h"
#include "Actor.h"
#include "Inventory.h"
#include "HudItem.h"
#include "da_pda3d.h"
#include "da_opt_bench.h"
#include "da_profiler.h"
#include "Actor_Flags.h"
#include "Bolt.h"
#include "CustomZone.h"
#include "xrScriptEngine/script_engine.hpp"
#include "xrScriptEngine/script_profiler.hpp"
#include "xrScriptEngine/script_process.hpp"
#include "xrServer_Objects.h"
#include "ui/UIMainIngameWnd.h"
#include "xrPhysics/IPHWorld.h"
#include "autosave_manager.h"
#include "ai_space.h"
#include "ai/monsters/basemonster/base_monster.h"
#include "date_time.h"
#include "mt_config.h"
#include "ui/UIOptConCom.h"
#include "UIGameSP.h"
#include "ui/UIActorMenu.h"
#include "ui/ContentService.h"
#include "ui/ModUpdateService.h"
#include "player_hud.h"
#include "xrUICore/Static/UIStatic.h"
#include "xrUICore/ui_styles.h"
#include "zone_effector.h"
#include "GameTask.h"
#include "MainMenu.h"
#include "saved_game_wrapper.h"
#include "xrAICore/Navigation/level_graph.h"
#include "xrNetServer/NET_Messages.h"
#include "xrCore/Debug/CrashReport.h"
#include "xrSound/Sound.h"

#include "CameraLook.h"
#include "character_hit_animations_params.h"
#include "inventory_upgrade_manager.h"

#include "xrGameSpy/GameSpy_Full.h"

#include "ai_debug_variables.h"
#include "xrPhysics/console_vars.h"
#include "GametaskManager.h"

#ifdef DEBUG
#include "PHDebug.h"
#include "ui/UIDebugFonts.h"
#include "xrAICore/Navigation/game_graph.h"
#include "LevelGraphDebugRender.hpp"
#include "CharacterPhysicsSupport.h"
#include "PHMovementControl.h"
#include "Include/xrRender/Kinematics.h"
#endif // DEBUG

string_path g_last_saved_game;

#ifdef DEBUG
extern float air_resistance_epsilon;
#endif // #ifdef DEBUG

// ui/UpdateService.cpp - the major-release notice switch.
extern int g_dar_major_update_notice;
// UIGameSP.cpp - open the inventory without waiting out the backpack scene.
extern int g_dar_instant_inventory;
// ZoneCampfire.cpp - rain puts campfires out.
extern int g_dar_rain_douses_fires;

extern void show_smart_cast_stats();
extern void clear_smart_cast_stats();
extern void release_smart_cast_stats();

extern u64 g_qwStartGameTime;
extern u64 g_qwEStartGameTime;

ENGINE_API
extern float psHUD_FOV;
ENGINE_API
extern float psHUD_FOV_def;
extern float psSqueezeVelocity;
extern int psLUA_GCSTEP;
extern int psLUA_GCTIMEOUT;
extern u32 ps_lua_gc_method;
extern int g_auto_ammo_unload;

extern int x_m_x;
extern int x_m_z;
extern BOOL net_cl_inputguaranteed;
extern BOOL net_sv_control_hit;
extern int g_dwInputUpdateDelta;
#ifdef DEBUG
extern BOOL g_ShowAnimationInfo;
#endif // DEBUG
extern BOOL g_bShowHitSectors;
// extern	BOOL	g_bDebugDumpPhysicsStep	;
extern ESingleGameDifficulty g_SingleGameDifficulty;
//-----------------------------------------------------------
extern float g_fTimeFactor;
extern BOOL b_toggle_weapon_aim;

extern float g_smart_cover_factor;
extern int g_upgrades_log;
extern float g_smart_cover_animation_speed_factor;

extern BOOL g_ai_use_old_vision;
float g_aim_predict_time = 0.40f;
int g_auto_continue_on_load = 1;
int g_keypress_on_start_legacy = 1;
int g_infinite_bolts = 1;

ENGINE_API extern float g_console_sensitive;

//Alundaio
extern BOOL g_ai_die_in_anomaly;
int g_inv_highlight_equipped = 0;
//-Alundaio

int g_first_person_death = 0;
int g_normalize_mouse_sens = 0;
int g_normalize_upgrade_mouse_sens = 0;

// NPC foot IK ground probing is skipped beyond this camera distance, in meters;
// 0 keeps the stock always-on behavior (see CCharacterPhysicsSupport::in_UpdateCL)
float g_ph_ik_dist = 0.f;
// dead objects get a full visibility recalculation at most once per this many
// milliseconds; 0 restores the stock every-cycle behavior (see CVisualMemoryManager)
int g_ai_dead_vision_ms = 1000;

void register_mp_console_commands();
//-----------------------------------------------------------

BOOL g_bCheckTime = FALSE;
int net_cl_inputupdaterate = 50;
Flags32 g_mt_config = {mtLevelPath | mtDetailPath | mtObjectHandler | mtSoundPlayer | mtAiVision | mtBullets |
    mtLUA_GC | mtLevelSounds | mtALife | mtMap};
#ifdef DEBUG
Flags32 dbg_net_Draw_Flags{};
#endif

#ifdef DEBUG
BOOL g_bDebugNode = FALSE;
u32 g_dwDebugNodeSource = 0;
u32 g_dwDebugNodeDest = 0;
extern BOOL g_bDrawBulletHit;
extern BOOL g_bDrawFirstBulletCrosshair;

float debug_on_frame_gather_stats_frequency = 0.f;
#endif
#ifdef DEBUG
extern pstr dbg_stalker_death_anim;
extern BOOL b_death_anim_velocity;
extern BOOL death_anim_debug;
extern BOOL dbg_imotion_draw_skeleton;
extern BOOL dbg_imotion_draw_velocity;
extern BOOL dbg_imotion_collide_debug;
extern float dbg_imotion_draw_velocity_scale;
#endif
int g_AI_inactive_time = 0;
Flags32 g_uCommonFlags;
enum E_COMMON_FLAGS
{
    flAiUseTorchDynamicLights = 1
};

const xr_token lua_gc_method_token[] =
{
    { "gc_disable", 0 },
    { "gc_step", 1 },
    { "gc_timeout", 2 },
    { "gc_full", 3 },
    { nullptr, -1 }
};

CUIOptConCom g_OptConCom;

void log_stability_memory_stats(bool compact)
{
    if (compact)
        Memory.mem_compact();

    u32 m_base = 0, c_base = 0, m_lmaps = 0, c_lmaps = 0;
    GEnv.Render->ResourcesGetMemoryUsage(m_base, c_base, m_lmaps, c_lmaps);
    log_vminfo();

    const size_t processHeap = Memory.mem_usage();
    const auto [ecoStringsBytes, ecoStringsCount] = g_pStringContainer->stat_economy();
    const size_t ecoSmem = g_pSharedMemoryContainer->stat_economy();
    const size_t luaBytes = GEnv.ScriptEngine ? lua_gc(GEnv.ScriptEngine->lua(), LUA_GCCOUNT, 0) * 1024ull : 0;
    const size_t modelBytes = GEnv.Render->models_GetMemoryUsage();
    const size_t alifeObjects = ai().get_alife() ? ai().alife().objects().objects().size() : 0;
    const size_t onlineObjects = g_pGameLevel ? Level().Objects.o_count() : 0;

    u32 pendingRelease = 0;
    if (GEnv.ScriptEngine)
    {
        luabind::functor<u32> pendingCount;
        if (GEnv.ScriptEngine->functor("safe_release_manager.pending_count", pendingCount))
            pendingRelease = pendingCount();
    }

    CrashRuntimeTelemetry telemetry;
    telemetry.textureBytes = size_t(m_base + m_lmaps);
    telemetry.modelBytes = modelBytes;
    telemetry.soundBytes = size_t(psSoundCacheSizeMB) * 1024 * 1024;
    telemetry.luaBytes = luaBytes;
    telemetry.alifeObjects = static_cast<u32>(alifeObjects);
    telemetry.onlineObjects = static_cast<u32>(onlineObjects);
    telemetry.pendingReleaseObjects = pendingRelease;
    telemetry.fps = Device.GetStats().fFPS;
    telemetry.frameMilliseconds = Device.fTimeDeltaReal * 1000.f;
    telemetry.renderMilliseconds = Device.GetStats().RenderTotal.result;
    CrashReporter::UpdateTelemetry(telemetry);

    Msg("* [render]: textures[%zu MiB], models[%zu MiB]", size_t(m_base + m_lmaps) / 1048576, modelBytes / 1048576);
    Msg("* [x-ray]: private[%zu MiB], Lua[%zu MiB]", processHeap / 1048576, luaBytes / 1048576);
    Msg("* [x-ray]: shared string savings[%zu KiB/%zu], shared memory savings[%zu KiB]",
        ecoStringsBytes / 1024, ecoStringsCount, ecoSmem);
    // Corruption canary for the "symbols instead of letters" class of report: if pooled text was
    // stomped in place, this names the stomped entries right in the session log.
    if (const size_t stomped = g_pStringContainer->verify_report())
        Msg("! [x-ray]: shared string pool: %zu entries no longer match their own checksum", stomped);
    Msg("* [ALife]: objects[%zu], online[%zu], pending release[%u]", alifeObjects, onlineObjects, pendingRelease);
#ifdef FS_DEBUG
    Msg("* [ x-ray  ]: file mapping: memory[%d K], count[%d]", g_file_mapped_memory / 1024, g_file_mapped_count);
    dump_file_mappings();
#endif
}

static void full_memory_stats()
{
    log_stability_memory_stats(true);
}

class CCC_MemStats : public IConsole_Command
{
public:
    CCC_MemStats(LPCSTR N) : IConsole_Command(N)
    {
        bEmptyArgsHandled = TRUE;
        xrDebug::SetOutOfMemoryCallback(full_memory_stats);
    };
    virtual void Execute(LPCSTR args) { full_memory_stats(); }
};

class CCC_SessionReport : public IConsole_Command
{
public:
    explicit CCC_SessionReport(pcstr name) : IConsole_Command(name) { bEmptyArgsHandled = true; }

    void Execute(pcstr) override
    {
        log_stability_memory_stats(false);
        FlushLog();
        if (CrashReporter::WriteSession())
            Msg("* Diagnostic session report saved to %s", CrashReporter::LatestReportPath());
        else
            Msg("! Failed to save diagnostic session report");
        FlushLog();
    }
};

class CCC_GameDifficulty : public CCC_Token
{
public:
    CCC_GameDifficulty(LPCSTR N) : CCC_Token(N, (u32*)&g_SingleGameDifficulty, difficulty_type_token){};
    virtual void Execute(LPCSTR args)
    {
        CCC_Token::Execute(args);
        if (g_pGameLevel && Level().game)
        {
            //#ifndef	DEBUG
            if (GameID() != eGameIDSingle)
            {
                Msg("For this game type difficulty level is disabled.");
                return;
            };
            //#endif

            game_cl_Single* game = smart_cast<game_cl_Single*>(Level().game);
            VERIFY(game);
            game->OnDifficultyChanged();
        }
    }
    virtual void Info(TInfo& I) { xr_strcpy(I, "game difficulty"); }
};

class CCC_GameLanguage : public CCC_Token
{
public:
    CCC_GameLanguage(pcstr N) : CCC_Token(N, (u32*)&CStringTable::LanguageID, nullptr) {}

    void Execute(pcstr args) override
    {
        CCC_Token::Execute(args);
        StringTable().ReloadLanguage();

        Device.seqUIReset.Process();

        if (!g_pGameLevel)
            return;

        for (u16 id = 0; id < 0xffff; id++)
        {
            IGameObject* gameObj = Level().Objects.net_Find(id);
            if (gameObj)
            {
                if (CInventoryItem* invItem = gameObj->cast_inventory_item())
                    invItem->ReloadNames();
            }
        }
    }

    const xr_token* GetToken() noexcept override
    {
        tokens = StringTable().GetLanguagesToken();
        if(!tokens) // Prevent failure without usage Nifty counters
        {
            Msg("GetToken: token missing");
            StringTable().Destroy();
            StringTable().Init();

            tokens = StringTable().GetLanguagesToken();
        }
        return CCC_Token::GetToken();
    }
};

#ifdef DEBUG
class CCC_ALifePath : public IConsole_Command
{
public:
    CCC_ALifePath(LPCSTR N) : IConsole_Command(N){};
    virtual void Execute(LPCSTR args)
    {
        if (!ai().get_level_graph())
            Msg("! there is no graph!");
        else
        {
            int id1 = -1, id2 = -1;
            sscanf(args, "%d %d", &id1, &id2);
            if ((-1 != id1) && (-1 != id2))
                if (_max(id1, id2) > (int)ai().game_graph().header().vertex_count() - 1)
                    Msg("! there are only %d vertexes!", ai().game_graph().header().vertex_count());
                else if (_min(id1, id2) < 0)
                    Msg("! invalid vertex number (%d)!", _min(id1, id2));
                else
                {
                    //						Sleep				(1);
                    //						CTimer				timer;
                    //						timer.Start			();
                    //						float				fValue = ai().m_tpAStar->ffFindMinimalPath(id1,id2);
                    //						Msg					("* %7.2f[%d] : %11I64u cycles (%.3f
                    // microseconds)",fValue,ai().m_tpAStar->m_tpaNodes.size(),timer.GetElapsed_ticks(),timer.GetElapsed_ms()*1000.f);
                }
            else
                Msg("! not enough parameters!");
        }
    }
};
#endif // DEBUG

class CCC_ALifeTimeFactor : public IConsole_Command
{
public:
    CCC_ALifeTimeFactor(LPCSTR N) : IConsole_Command(N){};
    virtual void Execute(LPCSTR args)
    {
        float id1 = 0.0f;
        sscanf(args, "%f", &id1);
        if (id1 < EPS_L)
            Msg("Invalid time factor! (%.4f)", id1);
        else
        {
            if (!OnServer())
                return;

            Level().SetGameTimeFactor(id1);
        }
    }

    virtual void Save(IWriter* F){};
    void GetStatus(TStatus& S) override
    {
        if (!g_pGameLevel)
            return;

        float v = Level().GetGameTimeFactor();
        xr_sprintf(S, sizeof(S), "%3.5f", v);
        while (xr_strlen(S) && ('0' == S[xr_strlen(S) - 1]))
            S[xr_strlen(S) - 1] = 0;
    }
    virtual void Info(TInfo& I)
    {
        if (!OnServer())
            return;
        float v = Level().GetGameTimeFactor();
        xr_sprintf(I, sizeof(I), " value = %3.5f", v);
    }
    virtual void fill_tips(vecTips& tips, u32 mode)
    {
        if (!OnServer())
            return;
        float v = Level().GetGameTimeFactor();

        TStatus str;
        xr_sprintf(str, sizeof(str), "%3.5f  (current)  [0.0,1000.0]", v);
        tips.push_back(str);
        IConsole_Command::fill_tips(tips, mode);
    }
};

class CCC_ALifeSwitchDistance : public IConsole_Command
{
public:
    CCC_ALifeSwitchDistance(LPCSTR N) : IConsole_Command(N){};
    virtual void Execute(LPCSTR args)
    {
        if ((GameID() == eGameIDSingle) && ai().get_alife())
        {
            float id1 = 0.0f;
            sscanf(args, "%f", &id1);
            if (id1 < 2.0f)
                Msg("Invalid online distance! (%.4f)", id1);
            else
            {
                NET_Packet P;
                P.w_begin(M_SWITCH_DISTANCE);
                P.w_float(id1);
                Level().Send(P, net_flags(TRUE, TRUE));
            }
        }
        else
            Log("!Not a single player game!");
    }
};

class CCC_ALifeProcessTime : public IConsole_Command
{
public:
    CCC_ALifeProcessTime(LPCSTR N) : IConsole_Command(N){};
    virtual void Execute(LPCSTR args)
    {
        if ((GameID() == eGameIDSingle) && ai().get_alife())
        {
            game_sv_Single* tpGame = smart_cast<game_sv_Single*>(Level().Server->GetGameState());
            VERIFY(tpGame);
            int id1 = 0;
            sscanf(args, "%d", &id1);
            if (id1 < 1)
                Msg("Invalid process time! (%d)", id1);
            else
                tpGame->alife().set_process_time(id1);
        }
        else
            Log("!Not a single player game!");
    }
};

class CCC_ALifeObjectsPerUpdate : public IConsole_Command
{
public:
    CCC_ALifeObjectsPerUpdate(LPCSTR N) : IConsole_Command(N){};
    virtual void Execute(LPCSTR args)
    {
        if ((GameID() == eGameIDSingle) && ai().get_alife())
        {
            game_sv_Single* tpGame = smart_cast<game_sv_Single*>(Level().Server->GetGameState());
            VERIFY(tpGame);
            int id1 = 0;
            sscanf(args, "%d", &id1);
            tpGame->alife().objects_per_update(id1);
        }
        else
            Log("!Not a single player game!");
    }
};

class CCC_ALifeSwitchFactor : public IConsole_Command
{
public:
    CCC_ALifeSwitchFactor(LPCSTR N) : IConsole_Command(N){};
    virtual void Execute(LPCSTR args)
    {
        if ((GameID() == eGameIDSingle) && ai().get_alife())
        {
            game_sv_Single* tpGame = smart_cast<game_sv_Single*>(Level().Server->GetGameState());
            VERIFY(tpGame);
            float id1 = 0;
            sscanf(args, "%f", &id1);
            clamp(id1, .1f, 1.f);
            tpGame->alife().set_switch_factor(id1);
        }
        else
            Log("!Not a single player game!");
    }
};

//-----------------------------------------------------------------------
class CCC_DemoRecord : public IConsole_Command
{
public:
    CCC_DemoRecord(LPCSTR N) : IConsole_Command(N) {}
    virtual void Execute(LPCSTR args)
    {
        if (!g_pGameLevel) // level not loaded
        {
            Log("Demo Record is disabled when level is not loaded.");
            return;
        }

        Console->Hide();

        // close main menu if it is open
        if (MainMenu()->IsActive())
            MainMenu()->Activate(false);

        pstr fn_;
        STRCONCAT(fn_, args, ".xrdemo");
        string_path fn;
        FS.update_path(fn, "$game_saves$", fn_);

        g_pGameLevel->Cameras().AddCamEffector(xr_new<CDemoRecord>(fn));
    }
};

class CCC_DemoRecordSetPos : public CCC_Vector3
{
    static Fvector p;

public:
    CCC_DemoRecordSetPos(LPCSTR N)
        : CCC_Vector3(N, &p, Fvector().set(-FLT_MAX, -FLT_MAX, -FLT_MAX), Fvector().set(FLT_MAX, FLT_MAX, FLT_MAX)){};
    virtual void Execute(LPCSTR args)
    {
#ifndef DEBUG
// if (GameID() != eGameIDSingle)
//{
//	Msg("For this game type Demo Record is disabled.");
//	return;
//};
#endif
        CDemoRecord::GetGlobalPosition(p);
        CCC_Vector3::Execute(args);
        CDemoRecord::SetGlobalPosition(p);
    }
    virtual void Save(IWriter* F) { ; }
};
Fvector CCC_DemoRecordSetPos::p = {0, 0, 0};

// Session tools: never written to user.ltx, so an installation carries nothing about the
// diagnostics of the animation scenes or the weapon faults in its options.
class CCC_SessionFloat : public CCC_Float
{
public:
    using CCC_Float::CCC_Float;
    void Save(IWriter*) override {}
};
class CCC_SessionInteger : public CCC_Integer
{
public:
    using CCC_Integer::CCC_Integer;
    void Save(IWriter*) override {}
};
class CCC_SessionVector3 : public CCC_Vector3
{
public:
    using CCC_Vector3::CCC_Vector3;
    void Save(IWriter*) override {}
};

// Prints the tuned scene item seat as config lines: the numbers live in the console until a
// restart and copying them by eye is one more way to get them wrong.
class CCC_HudSceneItemDump : public IConsole_Command
{
public:
    CCC_HudSceneItemDump(pcstr N) : IConsole_Command(N) { bEmptyArgsHandled = true; }
    void Execute(pcstr /*args*/) override
    {
        extern Fvector g_hud_scene_item_pos_adj, g_hud_scene_item_rot_adj;
        extern float g_hud_scene_item_scale_adj;
        if (!g_player_hud)
        {
            Msg("! [hud-scene] no hands yet");
            return;
        }
        Fvector pos, rot;
        float scale = 1.f;
        g_player_hud->scene_item_tune(pos, rot, scale);
        pos.add(g_hud_scene_item_pos_adj);
        rot.add(g_hud_scene_item_rot_adj);
        scale *= g_hud_scene_item_scale_adj;
        Msg("~ [hud-scene] scene item seat, for the section:");
        Msg("item_position                            = %.4f, %.4f, %.4f", pos.x, pos.y, pos.z);
        Msg("item_orientation                         = %.2f, %.2f, %.2f", rot.x, rot.y, rot.z);
        Msg("item_scale                               = %.4f", scale);
    }
};

class CCC_DemoPlay : public IConsole_Command
{
public:
    CCC_DemoPlay(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };
    virtual void Execute(LPCSTR args)
    {
#ifndef DEBUG
// if (GameID() != eGameIDSingle)
//{
//	Msg("For this game type Demo Play is disabled.");
//	return;
//};
#endif
        if (0 == g_pGameLevel)
        {
            Msg("! There are no level(s) started");
        }
        else
        {
            Console->Hide();
            string_path fn;
            u32 loops = 0;
            pstr comma = strchr(const_cast<pstr>(args), ',');
            if (comma)
            {
                loops = atoi(comma + 1);
                *comma = 0; //. :)
            }
            strconcat(sizeof(fn), fn, args, ".xrdemo");
            FS.update_path(fn, "$game_saves$", fn);
            g_pGameLevel->Cameras().AddCamEffector(xr_new<CDemoPlay>(fn, 1.0f, loops));
        }
    }
};

class CCC_Spawn : public IConsole_Command
{
public:
    CCC_Spawn(pcstr name) : IConsole_Command(name) {}

    void Execute(pcstr args) override
    {
        if (!g_pGameLevel)
            return;

        if (!IsGameTypeSingle())
        {
            Log("Spawn command is available only in singleplayer mode.");
            return;
        }

        if (!pSettings->section_exist(args))
        {
            Msg("! Section [%s] doesn't exist...", args);
            return;
        }

        Fvector pos = Actor()->Position();
        Level().g_cl_Spawn(args, 0xff, M_SPAWN_OBJECT_LOCAL, pos);
    }

    void Info(TInfo& I) override
    {
        xr_strcpy(I, "valid name of an entity or item that can be spawned");
    }
};

class CCC_SpawnToInventory : public IConsole_Command
{
public:
    CCC_SpawnToInventory(pcstr name) : IConsole_Command(name) {}

    void Execute(pcstr args) override
    {
        if (!g_pGameLevel)
            return;

        if (!IsGameTypeSingle())
        {
            Log("Spawn command is available only in singleplayer mode.");
            return;
        }

        if (!pSettings->section_exist(args))
        {
            Msg("! Section [%s] doesn't exist...", args);
            return;
        }

        Level().spawn_item(args, Actor()->Position(), false, Actor()->ID());
    }

    void Info(TInfo& I) override
    {
        xr_strcpy(I, "valid name of an item that can be spawned");
    }
};
// helper functions --------------------------------------------

bool valid_saved_game_name(LPCSTR file_name)
{
    LPCSTR I = file_name;
    LPCSTR E = file_name + xr_strlen(file_name);
    for (; I != E; ++I)
    {
        if (!strchr("/" DELIMITER ":*?\"<>|^()[]%", *I))
            continue;

        return (false);
    };

    return (true);
}

void get_files_list(xr_vector<shared_str>& files, LPCSTR dir, LPCSTR file_ext)
{
    VERIFY(dir && file_ext);
    files.clear();

    FS_Path* P = FS.get_path(dir);
    P->m_Flags.set(FS_Path::flNeedRescan, TRUE);
    FS.m_Flags.set(CLocatorAPI::flNeedCheck, TRUE);
    FS.rescan_pathes();

    LPCSTR fext;
    STRCONCAT(fext, "*", file_ext);

    FS_FileSet files_set;
    FS.file_list(files_set, dir, FS_ListFiles, fext);
    u32 len_str_ext = xr_strlen(file_ext);

    auto itb = files_set.begin();
    auto ite = files_set.end();

    for (; itb != ite; ++itb)
    {
        LPCSTR fn_ext = (*itb).name.c_str();
        VERIFY(xr_strlen(fn_ext) > len_str_ext);
        string_path fn;
        strncpy_s(fn, sizeof(fn), fn_ext, xr_strlen(fn_ext) - len_str_ext);
        files.push_back(fn);
    }
    FS.m_Flags.set(CLocatorAPI::flNeedCheck, FALSE);
}

#include "UIGameCustom.h"

class CCC_ALifeSave : public IConsole_Command
{
    bool m_showStatus;

public:
    CCC_ALifeSave(LPCSTR N, bool showStatus) : IConsole_Command(N), m_showStatus(showStatus)
    {
        bEmptyArgsHandled = true;
    }

    virtual void Execute(LPCSTR args)
    {
#if 0
        if (!Level().autosave_manager().ready_for_autosave()) {
            Msg		("! Cannot save the game right now!");
            return;
        }
#endif
        if (!IsGameTypeSingle())
        {
            Msg("for single-mode only");
            return;
        }
        if (!g_actor || !Actor()->g_Alive())
        {
            Msg("cannot make saved game because actor is dead :(");
            return;
        }

        string_path S, S1;
        S[0] = 0;
        strncpy_s(S, sizeof(S), args, MAX_PATH - 1);

#ifdef DEBUG
        CTimer timer;
        timer.Start();
#endif
        bool updateName = false;
        if (!xr_strlen(S))
        {
            strconcat(sizeof(S), S, Core.UserName, " - ", "quicksave");
        }
        else
        {
            if (!valid_saved_game_name(S))
            {
                Msg("! Save failed: invalid file name - %s", S);
                return;
            }
            updateName = true;
        }

        NET_Packet net_packet;
        net_packet.w_begin(M_SAVE_GAME);
        net_packet.w_stringZ(S);
        net_packet.w_u8(u8(updateName));
        // Legacy packets end after updateName; the reader defaults a missing presentation byte to true.
        net_packet.w_u8(u8(m_showStatus));
        Level().Send(net_packet, net_flags(TRUE));
#ifdef DEBUG
        Msg("Game save overhead  : %f milliseconds", timer.GetElapsed_sec() * 1000.f);
#endif
        const bool compat = ClearSkyMode || ShadowOfChernobylMode;
        CUIGameCustom* gameUi = CurrentGameUI();
        if (m_showStatus && gameUi)
        {
            gameUi->RemoveCustomStatic("game_saved");
            StaticDrawableWrapper* message = gameUi->AddCustomStatic("game_saved", true, compat ? 3.0f : -1.0f);
            if (message)
                message->wnd()->TextItemControl()->SetText(StringTable().translate("st_game_saving").c_str());
        }

        xr_strcat(S, ".dds");
        FS.update_path(S1, "$game_saves$", S);

#ifdef DEBUG
        timer.Start();
#endif
        MainMenu()->Screenshot(IRender::SM_FOR_GAMESAVE, S1);
#ifdef DEBUG
        Msg("Screenshot overhead : %f milliseconds", timer.GetElapsed_sec() * 1000.f);
#endif
    } // virtual void Execute

    virtual void fill_tips(vecTips& tips, u32 mode)
    {
        if (ShadowOfChernobylMode || ClearSkyMode)
            get_files_list(tips, "$game_saves$", SAVE_EXTENSION_LEGACY);
        else
            get_files_list(tips, "$game_saves$", SAVE_EXTENSION);
    }
}; // CCC_ALifeSave

class CCC_ALifeLoadFrom : public IConsole_Command
{
public:
    CCC_ALifeLoadFrom(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; };
    virtual void Execute(LPCSTR args)
    {
        string_path saved_game;
        strncpy_s(saved_game, sizeof(saved_game), args, MAX_PATH - 1);

        if (!ai().get_alife())
        {
            Log("! ALife simulator has not been started yet");
            return;
        }

        if (!xr_strlen(saved_game))
        {
            Log("! Specify file name!");
            return;
        }

        if (!CSavedGameWrapper::saved_game_exist(saved_game))
        {
            Msg("! Cannot find saved game %s", saved_game);
            return;
        }

        if (!CSavedGameWrapper::valid_saved_game(saved_game))
        {
            Msg("! Cannot load saved game %s, version mismatch or saved game is corrupted", saved_game);
            return;
        }

        if (!valid_saved_game_name(saved_game))
        {
            Msg("! Cannot load saved game %s, invalid file name", saved_game);
            return;
        }

        /*     moved to level_network_messages.cpp
                CSavedGameWrapper			wrapper(args);
                if (wrapper.level_id() == ai().level_graph().level_id()) {
                    if (Device.Paused())
                        Device.Pause		(FALSE, TRUE, TRUE, "CCC_ALifeLoadFrom");

                    Level().remove_objects	();

                    game_sv_Single			*game = smart_cast<game_sv_Single*>(Level().Server->game);
                    R_ASSERT				(game);
                    game->restart_simulator	(saved_game);

                    return;
                }
        */

        if (MainMenu()->IsActive())
            MainMenu()->Activate(false);

        Console->Execute("stat_memory");

        if (Device.Paused())
            Device.Pause(FALSE, TRUE, TRUE, "CCC_ALifeLoadFrom");

        NET_Packet net_packet;
        net_packet.w_begin(M_LOAD_GAME);
        net_packet.w_stringZ(saved_game);
        Level().Send(net_packet, net_flags(TRUE));
    }

    virtual void fill_tips(vecTips& tips, u32 mode)
    {
        if (ShadowOfChernobylMode || ClearSkyMode)
            get_files_list(tips, "$game_saves$", SAVE_EXTENSION_LEGACY);
        else
            get_files_list(tips, "$game_saves$", SAVE_EXTENSION);
    }
}; // CCC_ALifeLoadFrom

class CCC_LoadLastSave : public IConsole_Command
{
public:
    CCC_LoadLastSave(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; }
    virtual void Execute(LPCSTR args)
    {
        string_path saved_game = "";
        if (args)
        {
            strncpy_s(saved_game, sizeof(saved_game), args, MAX_PATH - 1);
        }

        if (*saved_game)
        {
            xr_strcpy(g_last_saved_game, saved_game);
            return;
        }

        if (!*g_last_saved_game)
        {
            Msg("! cannot load last saved game since it hasn't been specified");
            return;
        }

        if (!CSavedGameWrapper::saved_game_exist(g_last_saved_game))
        {
            Msg("! Cannot find saved game %s", g_last_saved_game);
            return;
        }

        if (!CSavedGameWrapper::valid_saved_game(g_last_saved_game))
        {
            Msg("! Cannot load saved game %s, version mismatch or saved game is corrupted", g_last_saved_game);
            return;
        }

        if (!valid_saved_game_name(g_last_saved_game))
        {
            Msg("! Cannot load saved game %s, invalid file name", g_last_saved_game);
            return;
        }

        pstr command;
        if (ai().get_alife())
        {
            STRCONCAT(command, "load ", g_last_saved_game);
            Console->Execute(command);
            return;
        }

        STRCONCAT(command, "start server(", g_last_saved_game, "/single/alife/load)");
        Console->Execute(command);
    }

    virtual void Save(IWriter* F)
    {
        if (!*g_last_saved_game)
            return;

        F->w_printf("%s %s\r\n", cName, g_last_saved_game);
    }
};

class CCC_FlushLog : public IConsole_Command
{
public:
    CCC_FlushLog(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; };
    virtual void Execute(LPCSTR /**args**/)
    {
        FlushLog();
        Msg("* Log file has been saved successfully!");
    }
};

// The content commands are read-only or repair-only by design. There is deliberately nothing
// here that can skip content or mark a broken installation as good: content is not optional,
// so a switch that pretended otherwise would only ever produce a crash further downstream.
class CCC_ContentVerify : public IConsole_Command
{
public:
    CCC_ContentVerify(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; }
    virtual void Execute(LPCSTR) { ContentService::ForceVerify(); }
    virtual void Info(TInfo& I) { xr_strcpy(I, "re-hash every content bundle and report the result"); }
};

class CCC_ContentRepair : public IConsole_Command
{
public:
    CCC_ContentRepair(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; }
    virtual void Execute(LPCSTR) { ContentService::StartRepair(); }
    virtual void Info(TInfo& I) { xr_strcpy(I, "download and install whatever content is missing"); }
};

class CCC_ContentState : public IConsole_Command
{
public:
    CCC_ContentState(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; }
    virtual void Execute(LPCSTR) { ContentService::LogState(); }
    virtual void Info(TInfo& I) { xr_strcpy(I, "dump the content installation state to the log"); }
};

class CCC_ClearLog : public IConsole_Command
{
public:
    CCC_ClearLog(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; };
    virtual void Execute(LPCSTR)
    {
        LogFile.clear();
        FlushLog();
        Msg("* Log file has been cleaned successfully!");
    }
};

class CCC_XmsList : public IConsole_Command
{
public:
    CCC_XmsList(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; };
    virtual void Execute(LPCSTR)
    {
        const auto& modules = XMS::Modules();
        if (modules.empty())
        {
            Msg("* XMS: no modules installed");
            return;
        }
        for (const XMS::Module& m : modules)
        {
            if (m.disabled)
                Msg("* [--] %s %s - DISABLED: %s", m.id.c_str(), m.version.c_str(), m.disable_reason.c_str());
            else
                Msg("* [%02u] %s %s (ns=%u)", u32(m.layer), m.id.c_str(), m.version.c_str(), u32(m.ns));
        }
    }
};

class CCC_XmsModes : public IConsole_Command
{
public:
    CCC_XmsModes(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; };
    virtual void Execute(LPCSTR args)
    {
        // no args = show; "none" clears; otherwise a comma list activates
        if (!args || !args[0])
        {
            Msg("* XMS: active modes: [%s]", XMS::ActiveModesCsv()[0] ? XMS::ActiveModesCsv() : "none");
            for (const XMS::Module& m : XMS::Modules())
                for (const XMS::ProvidedMode& mode : m.provides_modes)
                    if (!m.disabled)
                        Msg("*   known mode: %s (module %s)", mode.id.c_str(), m.id.c_str());
            return;
        }
        XMS::SetActiveModes(0 == xr_strcmp(args, "none") ? "" : args);
    }
    virtual void Info(TInfo& I) { xr_strcpy(I, "show or set active XMS game modes (comma list, 'none' to clear)"); }
};

// XMS's own "activate / deactivate a mod". A mod manager does that job by
// copying files over the game; this only writes an id to disabled.ltx, so the
// module stays where it is and the game tree is never touched.
class CCC_XmsEnable : public IConsole_Command
{
    bool m_enable;

public:
    CCC_XmsEnable(LPCSTR N, bool enable) : IConsole_Command(N), m_enable(enable) { bEmptyArgsHandled = false; };
    virtual void Execute(LPCSTR args)
    {
        if (!args || !args[0])
        {
            Msg("! usage: %s <module id>   (see xms_list)", cName);
            return;
        }
        xr_string err;
        if (!XMS::SetModuleEnabled(args, m_enable, err))
            Msg("! XMS: %s", err.c_str());
        else
            Msg("* XMS: module [%s] %s - takes effect on the next start", args, m_enable ? "enabled" : "disabled");
    }
    virtual void Info(TInfo& I)
    {
        xr_strcpy(I, m_enable ? "switch an XMS module back on (next start)" : "switch an XMS module off (next start)");
    }
};

// The Mods menu from the console: the same checks and downloads, without the window.
class CCC_XmsUpdate : public IConsole_Command
{
public:
    CCC_XmsUpdate(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = false; };
    virtual void Execute(LPCSTR args)
    {
        if (!args || !args[0])
        {
            Msg("! usage: %s <module id> | all   (see xms_update_status)", cName);
            return;
        }
        const bool all = 0 == xr_strcmp(args, "all");
        if (!all && !XMS::FindModule(args))
        {
            Msg("! XMS: no module [%s]", args);
            return;
        }
        ModUpdateService::RequestUpdate(all ? "" : args);
        Msg("* XMS: update of %s requested - progress in xms_update_status, applied on the next start", args);
    }
    virtual void Info(TInfo& I) { xr_strcpy(I, "check an XMS module (or 'all') for a release and download it"); }
};

class CCC_XmsUpdateStatus : public IConsole_Command
{
public:
    CCC_XmsUpdateStatus(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; };
    virtual void Execute(LPCSTR) { ModUpdateService::LogStatus(); }
    virtual void Info(TInfo& I) { xr_strcpy(I, "list XMS modules with the state of their updates"); }
};

class CCC_XmsMods : public IConsole_Command
{
public:
    CCC_XmsMods(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; };
    virtual void Execute(LPCSTR) { CMainMenu::RequestModsDialog(); }
    virtual void Info(TInfo& I) { xr_strcpy(I, "open the Mods window of the main menu"); }
};

class CCC_XmsConflicts : public IConsole_Command
{
public:
    CCC_XmsConflicts(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; };
    virtual void Execute(LPCSTR)
    {
        const auto& conflicts = XMS::Conflicts();
        Msg("* XMS: %zu conflict(s); full list in xms_report.json", conflicts.size());
        // console spam guard - the report file carries the rest
        const size_t shown = std::min<size_t>(conflicts.size(), 40);
        for (size_t i = 0; i < shown; ++i)
        {
            const XMS::ConflictRow& row = conflicts[i];
            Msg("*   %s: [%s] %s -> %s", row.subject.c_str(), row.loser.c_str(), "overridden by", row.winner.c_str());
        }
        XMS::WriteReport();
    }
};

// "who gave me this?" - the same question for a file path, a config section, a
// key or an xml node, answered from the overlay map plus the conflict ledger.
class CCC_XmsWhy : public IConsole_Command
{
public:
    CCC_XmsWhy(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = false; };
    virtual void Execute(LPCSTR args)
    {
        if (!args || !args[0])
        {
            Msg("! usage: xms_why <path or section or key fragment>");
            return;
        }

        xr_vector<XMS::OwnedFile> files;
        XMS::FindOwnedFiles(args, files, 20);
        for (const XMS::OwnedFile& f : files)
            Msg("*   file [%s] <- module %s (%s)", f.vpath.c_str(), f.module_id.c_str(), f.physical.c_str());

        // ledger rows carry everything that was overridden, merged or patched
        size_t rows = 0;
        for (const XMS::ConflictRow& row : XMS::Conflicts())
        {
            if (!strstr(row.subject.c_str(), args))
                continue;
            if (++rows > 20)
                break;
            Msg("*   %s [%s]: %s beats %s", kind_name_for_console(row.kind), row.subject.c_str(),
                row.winner.c_str(), row.loser.c_str());
        }

        if (files.empty() && !rows)
            Msg("* XMS: nothing matched [%s] - no module touches it, the base game owns it", args);
    }
    virtual void Info(TInfo& I) { xr_strcpy(I, "explain which XMS module provides or overrides a path/section/key"); }

private:
    static pcstr kind_name_for_console(XMS::ConflictKind kind)
    {
        switch (kind)
        {
        case XMS::ConflictKind::File: return "file";
        case XMS::ConflictKind::LtxSection: return "ltx section";
        case XMS::ConflictKind::LtxKey: return "ltx key";
        case XMS::ConflictKind::XmlPatch: return "xml patch";
        case XMS::ConflictKind::Script: return "script";
        default: return "other";
        }
    }
};

// Runs a Lua chunk the way `run_string` does: through the level script
// process when there is one, else directly on the engine state.
static void run_lua_console_string(LPCSTR args)
{
    if (GEnv.ScriptEngine->script_process(ScriptProcessor::Level))
    {
        GEnv.ScriptEngine->script_process(ScriptProcessor::Level)->add_script(args, true, true);
        return;
    }

    string4096 S;
    shared_str m_script_name = "console command";
    xr_sprintf(S, "%s\n", args);
    int l_iErrorCode = luaL_loadbuffer(GEnv.ScriptEngine->lua(), S, xr_strlen(S), "@console_command");
    if (!l_iErrorCode)
    {
        l_iErrorCode = lua_pcall(GEnv.ScriptEngine->lua(), 0, 0, 0);
        if (l_iErrorCode)
        {
            GEnv.ScriptEngine->print_output(GEnv.ScriptEngine->lua(), m_script_name.c_str(), l_iErrorCode);
            GEnv.ScriptEngine->on_error(GEnv.ScriptEngine->lua());
            return;
        }
    }

    GEnv.ScriptEngine->print_output(GEnv.ScriptEngine->lua(), m_script_name.c_str(), l_iErrorCode);
}

// NQ quest-graph console: `nq <anything>` becomes xms_nq_console.exec("<anything>")
class CCC_XmsNq : public IConsole_Command
{
public:
    CCC_XmsNq(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; bLowerCaseArgs = false; }
    virtual void Execute(LPCSTR args)
    {
        luabind::functor<void> exec;
        if (!GEnv.ScriptEngine || !GEnv.ScriptEngine->functor("xms_nq_console.exec", exec))
        {
            Msg("! NQ runtime is not loaded");
            return;
        }

        pcstr text = args && args[0] ? args : "help";
        xr_string chunk = "xms_nq_console.exec(\"";
        for (pcstr c = text; *c; ++c)
        {
            switch (*c)
            {
            case '\\': chunk += "\\\\"; break;
            case '"': chunk += "\\\""; break;
            case '\n': chunk += "\\n"; break;
            case '\r': chunk += "\\r"; break;
            default: chunk += *c; break;
            }
        }
        chunk += "\")";
        run_lua_console_string(chunk.c_str());
    }
    virtual void Info(TInfo& I) { xr_strcpy(I, "NQ quest-graph runtime console; `nq help` lists subcommands"); }
};

class CCC_FloatBlock : public CCC_Float
{
public:
    CCC_FloatBlock(LPCSTR N, float* V, float _min = 0, float _max = 1) : CCC_Float(N, V, _min, _max){};

    virtual void Execute(LPCSTR args)
    {
#ifdef _DEBUG
        CCC_Float::Execute(args);
#else
        if (!g_pGameLevel || GameID() == eGameIDSingle)
            CCC_Float::Execute(args);
        else
        {
            Msg("! Command disabled for this type of game");
        }
#endif
    }
};

class CCC_Net_CL_InputUpdateRate : public CCC_Integer
{
protected:
    int* value_blin;

public:
    CCC_Net_CL_InputUpdateRate(LPCSTR N, int* V, int _min = 0, int _max = 999)
        : CCC_Integer(N, V, _min, _max), value_blin(V){};

    virtual void Execute(LPCSTR args)
    {
        CCC_Integer::Execute(args);
        if ((*value_blin > 0) && g_pGameLevel)
        {
            g_dwInputUpdateDelta = 1000 / (*value_blin);
        };
    }
};

#ifdef DEBUG

class CCC_DrawGameGraphAll : public IConsole_Command
{
public:
    CCC_DrawGameGraphAll(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; }
    virtual void Execute(LPCSTR args)
    {
        if (!ai().get_level_graph())
            return;

        Level().GetLevelGraphDebugRender()->SetupCurrentLevel(-1);
    }
};

class CCC_DrawGameGraphCurrent : public IConsole_Command
{
public:
    CCC_DrawGameGraphCurrent(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; }
    virtual void Execute(LPCSTR args)
    {
        if (!ai().get_level_graph())
            return;

        Level().GetLevelGraphDebugRender()->SetupCurrentLevel(ai().level_graph().level_id());
    }
};

class CCC_DrawGameGraphLevel : public IConsole_Command
{
public:
    CCC_DrawGameGraphLevel(LPCSTR N) : IConsole_Command(N) {}
    virtual void Execute(LPCSTR args)
    {
        if (!ai().get_level_graph())
            return;

        if (!*args)
        {
            Level().GetLevelGraphDebugRender()->SetupCurrentLevel(-1);
            return;
        }

        const GameGraph::SLevel* level = ai().game_graph().header().level(args, true);
        if (!level)
        {
            Msg("! There is no level %s in the game graph", args);
            return;
        }

        Level().GetLevelGraphDebugRender()->SetupCurrentLevel(level->id());
    }
};

#if defined(USE_DEBUGGER)
class CCC_ScriptDbg : public IConsole_Command
{
public:
    CCC_ScriptDbg(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; };
    virtual void Execute(LPCSTR args)
    {
        if (strstr(cName, "script_debug_break") == cName)
        {
            CScriptDebugger* d = GEnv.ScriptEngine->debugger();
            if (d)
            {
                if (d->Active())
                    d->initiateDebugBreak();
                else
                    Msg("Script debugger not active.");
            }
            else
                Msg("Script debugger not present.");
        }
        else if (strstr(cName, "script_debug_stop") == cName)
        {
            GEnv.ScriptEngine->stopDebugger();
        }
        else if (strstr(cName, "script_debug_restart") == cName)
        {
            GEnv.ScriptEngine->restartDebugger();
        };
    };

    virtual void Info(TInfo& I)
    {
        if (strstr(cName, "script_debug_break") == cName)
            xr_strcpy(I, "initiate script debugger [DebugBreak] command");

        else if (strstr(cName, "script_debug_stop") == cName)
            xr_strcpy(I, "stop script debugger activity");

        else if (strstr(cName, "script_debug_restart") == cName)
            xr_strcpy(I, "restarts script debugger or start if no script debugger presents");
    }
};
#endif // #if defined(USE_DEBUGGER)

class CCC_DumpInfos : public IConsole_Command
{
public:
    CCC_DumpInfos(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; };
    virtual void Execute(LPCSTR args)
    {
        CActor* A = smart_cast<CActor*>(Level().CurrentEntity());
        if (A)
            A->DumpInfo();
    }
    virtual void Info(TInfo& I) { xr_strcpy(I, "dumps all infoportions that actor have"); }
};
class CCC_DumpTasks : public IConsole_Command
{
public:
    CCC_DumpTasks(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; };
    virtual void Execute(LPCSTR args)
    {
        CActor* A = smart_cast<CActor*>(Level().CurrentEntity());
        if (A)
            A->DumpTasks();
    }
    virtual void Info(TInfo& I) { xr_strcpy(I, "dumps all tasks that actor have"); }
};
#include "map_manager.h"
class CCC_DumpMap : public IConsole_Command
{
public:
    CCC_DumpMap(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; };
    virtual void Execute(LPCSTR args) { Level().MapManager().Dump(); }
    virtual void Info(TInfo& I) { xr_strcpy(I, "dumps all currentmap locations"); }
};

#include "alife_graph_registry.h"
class CCC_DumpCreatures : public IConsole_Command
{
public:
    CCC_DumpCreatures(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; };
    virtual void Execute(LPCSTR args)
    {
        typedef CSafeMapIterator<ALife::_OBJECT_ID, CSE_ALifeDynamicObject>::_REGISTRY::const_iterator const_iterator;

        const_iterator I = ai().alife().graph().level().objects().begin();
        const_iterator E = ai().alife().graph().level().objects().end();
        for (; I != E; ++I)
        {
            CSE_ALifeCreatureAbstract* obj = smart_cast<CSE_ALifeCreatureAbstract*>(I->second);
            if (obj)
            {
                Msg("\"%s\",", obj->name_replace());
            }
        }
    }
    virtual void Info(TInfo& I) { xr_strcpy(I, "dumps all creature names"); }
};

class CCC_DebugFonts : public IConsole_Command
{
public:
    CCC_DebugFonts(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; }

    ~CCC_DebugFonts()
    {
        xr_free(m_ui);
    }

    virtual void Execute(LPCSTR args)
    {
        if (!m_ui)
            m_ui = xr_new<CUIDebugFonts>();

        m_ui->ShowDialog(true);
    }

private:
    CUIDebugFonts* m_ui;
};

class CCC_DebugNode : public IConsole_Command
{
public:
    CCC_DebugNode(LPCSTR N) : IConsole_Command(N){};

    virtual void Execute(LPCSTR args)
    {
        string128 param1, param2;
        VERIFY(xr_strlen(args) < sizeof(string128));

        _GetItem(args, 0, param1, ' ');
        _GetItem(args, 1, param2, ' ');

        u32 value1;
        u32 value2;

        sscanf(param1, "%u", &value1);
        sscanf(param2, "%u", &value2);

        if ((value1 > 0) && (value2 > 0))
        {
            g_bDebugNode = TRUE;
            g_dwDebugNodeSource = value1;
            g_dwDebugNodeDest = value2;
        }
        else
        {
            g_bDebugNode = FALSE;
        }
    }
};

class CCC_ShowMonsterInfo : public IConsole_Command
{
public:
    CCC_ShowMonsterInfo(LPCSTR N) : IConsole_Command(N){};

    virtual void Execute(LPCSTR args)
    {
        string128 param1, param2;
        VERIFY(xr_strlen(args) < sizeof(string128));

        _GetItem(args, 0, param1, ' ');
        _GetItem(args, 1, param2, ' ');

        IGameObject* obj = Level().Objects.FindObjectByName(param1);
        CBaseMonster* monster = smart_cast<CBaseMonster*>(obj);
        if (!monster)
            return;

        u32 value2;

        sscanf(param2, "%u", &value2);
        monster->set_show_debug_info(u8(value2));
    }
};

void PH_DBG_SetTrackObject();
extern string64 s_dbg_trace_obj_name;
class CCC_DbgPhTrackObj : public CCC_String
{
public:
    CCC_DbgPhTrackObj(LPCSTR N) : CCC_String(N, s_dbg_trace_obj_name, sizeof(s_dbg_trace_obj_name)){};
    virtual void Execute(LPCSTR args)
    {
        CCC_String::Execute(args);
        if (!xr_strcmp(args, "none"))
        {
            ph_dbg_draw_mask1.set(ph_m1_DbgTrackObject, FALSE);
            return;
        }
        ph_dbg_draw_mask1.set(ph_m1_DbgTrackObject, TRUE);
        PH_DBG_SetTrackObject();
        // IGameObject* O= Level().Objects.FindObjectByName(args);
        // if(O)
        //{
        //	PH_DBG_SetTrackObject(*(O->cName()));
        //	ph_dbg_draw_mask1.set(ph_m1_DbgTrackObject,TRUE);
        //}
    }

    // virtual void	Info	(TInfo& I)
    //{
    //	xr_strcpy(I,"restart game fast");
    //}
};
#endif

class CCC_PHIterations : public CCC_Integer
{
public:
    CCC_PHIterations(LPCSTR N) : CCC_Integer(N, &phIterations, 15, 50){};
    virtual void Execute(LPCSTR args)
    {
        CCC_Integer::Execute(args);
        // dWorldSetQuickStepNumIterations(NULL,phIterations);
        if (physics_world())
            physics_world()->StepNumIterations(phIterations);
    }
};

#ifdef DEBUG
class CCC_PHGravity : public IConsole_Command
{
public:
    CCC_PHGravity(LPCSTR N) : IConsole_Command(N){};
    virtual void Execute(LPCSTR args)
    {
        if (!physics_world())
            return;
#ifndef DEBUG
        if (g_pGameLevel && Level().game && GameID() != eGameIDSingle)
        {
            Msg("Command is not available in Multiplayer");
            return;
        }
#endif
        physics_world()->SetGravity(float(atof(args)));
    }
    void GetStatus(TStatus& S) override
    {
        if (physics_world())
            xr_sprintf(S, "%3.5f", physics_world()->Gravity());
        else
            xr_sprintf(S, "%3.5f", default_world_gravity);
        while (xr_strlen(S) && ('0' == S[xr_strlen(S) - 1]))
            S[xr_strlen(S) - 1] = 0;
    }
};
#endif // DEBUG

class CCC_PHFps : public CCC_Float
{
#ifndef DEBUG
    static constexpr float MIN_FPS = 50;
    static constexpr float MAX_FPS = 200;
#else
    static constexpr float MIN_FPS = 1;
    static constexpr float MAX_FPS = 1000;
#endif

    float m_dummy = 1.f / ph_console::ph_step_time;

public:
    CCC_PHFps(pcstr name) : CCC_Float(name, &m_dummy, MIN_FPS, MAX_FPS) { }

    void Execute(pcstr args) override
    {
        CCC_Float::Execute(args);

        ph_console::ph_step_time = 1.f / m_dummy;
        if (physics_world())
            physics_world()->SetStep(ph_console::ph_step_time);
    }
};

#ifdef DEBUG

struct CCC_ShowSmartCastStats : public IConsole_Command
{
    CCC_ShowSmartCastStats(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; };
    virtual void Execute(LPCSTR args) { show_smart_cast_stats(); }
};

struct CCC_ClearSmartCastStats : public IConsole_Command
{
    CCC_ClearSmartCastStats(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; };
    virtual void Execute(LPCSTR args) { clear_smart_cast_stats(); }
};

#endif

#ifndef MASTER_GOLD
/*
struct CCC_NoClip : public CCC_Mask
{
public:
    CCC_NoClip(LPCSTR N, Flags32* V, u32 M):CCC_Mask(N,V,M){};
    virtual	void Execute(LPCSTR args)
    {
        CCC_Mask::Execute(args);
        if (EQ(args,"on") || EQ(args,"1"))
        {
            if(g_pGameLevel && Level().CurrentViewEntity())
            {
                CActor* actor = smart_cast<CActor*>(Level().CurrentViewEntity());
                actor->character_physics_support()->SetRemoved();
            }
        }
    };
};
*/

struct CCC_ToggleNoClip : public IConsole_Command
{
    CCC_ToggleNoClip(pcstr name) : IConsole_Command(name) { bEmptyArgsHandled = true; };

    void Execute(pcstr /*args*/) override
    {
        psActorFlags.invert(AF_NO_CLIP);

        if (!g_pGameLevel)
            return;

        CActor* actor = smart_cast<CActor*>(Level().CurrentViewEntity());
        if (!actor)
            return;

        // Workaround for actor has no physics at all until first move
        Fvector accel{};
        Actor()->g_Physics(accel, 0.0f, 0.0f);
    }
};

#include "xrAICore/Navigation/game_graph.h"
struct CCC_JumpToLevel : public IConsole_Command
{
    CCC_JumpToLevel(LPCSTR N) : IConsole_Command(N){};

    virtual void Execute(LPCSTR level)
    {
        if (!ai().get_alife())
        {
            Msg("! ALife simulator is needed to perform specified command!");
            return;
        }

        GameGraph::LEVEL_MAP::const_iterator I = ai().game_graph().header().levels().begin();
        GameGraph::LEVEL_MAP::const_iterator E = ai().game_graph().header().levels().end();
        for (; I != E; ++I)
            if (!xr_strcmp((*I).second.name(), level))
            {
                ai().alife().jump_to_level(level);
                return;
            }
        Msg("! There is no level \"%s\" in the game graph!", level);
    }

    virtual void Save(IWriter* F){};
    virtual void fill_tips(vecTips& tips, u32 mode)
    {
        if (!ai().get_alife())
        {
            Msg("! ALife simulator is needed to perform specified command!");
            return;
        }

        GameGraph::LEVEL_MAP::const_iterator itb = ai().game_graph().header().levels().begin();
        GameGraph::LEVEL_MAP::const_iterator ite = ai().game_graph().header().levels().end();
        for (; itb != ite; ++itb)
        {
            tips.push_back((*itb).second.name());
        }
    }
};

//#ifndef MASTER_GOLD
class CCC_Script : public IConsole_Command
{
public:
    CCC_Script(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = false; };
    virtual void Execute(LPCSTR args)
    {
        if (!xr_strlen(args))
        {
            Log("* Specify script name!");
        }
        else
        {
            // rescan pathes
            FS_Path* P = FS.get_path("$game_scripts$");
            P->m_Flags.set(FS_Path::flNeedRescan, TRUE);
            FS.rescan_pathes();
            // run script
            if (GEnv.ScriptEngine->script_process(ScriptProcessor::Level))
                GEnv.ScriptEngine->script_process(ScriptProcessor::Level)->add_script(args, false, true);
        }
    }

    void GetStatus(TStatus& S) override { xr_strcpy(S, "<script_name> (Specify script name!)"); }
    virtual void Save(IWriter* F) {}
    virtual void fill_tips(vecTips& tips, u32 mode) { get_files_list(tips, "$game_scripts$", ".script"); }
};

class CCC_ScriptCommand : public IConsole_Command
{
public:
    CCC_ScriptCommand(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = false; bLowerCaseArgs = false; }
    virtual void Execute(LPCSTR args)
    {
        if (!xr_strlen(args))
            Log("* Specify string to run!");
        else
            run_lua_console_string(args);
    } // void	Execute

    void GetStatus(TStatus& S) override { xr_strcpy(S, "<script_name.function()> (Specify script and function name!)"); }
    virtual void Save(IWriter* F) {}
    virtual void fill_tips(vecTips& tips, u32 mode)
    {
        if (mode == 1)
        {
            get_files_list(tips, "$game_scripts$", ".script");
            return;
        }

        IConsole_Command::fill_tips(tips, mode);
    }
};

class CCC_TimeFactor : public IConsole_Command
{
public:
    CCC_TimeFactor(LPCSTR N) : IConsole_Command(N) {}
    virtual void Execute(LPCSTR args)
    {
        float time_factor = (float)atof(args);
        clamp(time_factor, EPS, 1000.f);
        Device.time_factor(time_factor);
    }
    void GetStatus(TStatus& S) override { xr_sprintf(S, sizeof(S), "%f", Device.time_factor()); }
    virtual void Info(TInfo& I) { xr_strcpy(I, "[0.001 - 1000.0]"); }
    virtual void fill_tips(vecTips& tips, u32 mode)
    {
        TStatus str;
        xr_sprintf(str, sizeof(str), "%3.3f  (current)  [0.001 - 1000.0]", Device.time_factor());
        tips.push_back(str);
        IConsole_Command::fill_tips(tips, mode);
    }
};

#endif // MASTER_GOLD

// QA: start the landing roll on the spot, without a landing. Not saved, not a cheat that
// matters - it only plays the tumble and its sound.
class CCC_FallRollTest : public IConsole_Command
{
public:
    CCC_FallRollTest(pcstr N) : IConsole_Command(N) { bEmptyArgsHandled = true; }
    void Execute(pcstr) override
    {
        CActor* actor = g_pGameLevel ? smart_cast<CActor*>(Level().CurrentControlEntity()) : nullptr;
        if (actor && actor->g_Alive() && !actor->IsFallRolling())
            actor->StartFallRoll(true);
    }
};

class CCC_LuaGCMethod : public CCC_Token
{
public:
    CCC_LuaGCMethod(pcstr name) : CCC_Token(name, &ps_lua_gc_method, lua_gc_method_token) {}

    void Execute(pcstr args) override
    {
        const auto prev = *value;
        CCC_Token::Execute(args);

        switch (*value)
        {
        case 0:
            lua_gc(GEnv.ScriptEngine->lua(), LUA_GCSTOP, 0);
            break;
        case 1:
        case 2:
            if (prev == 0)
                lua_gc(GEnv.ScriptEngine->lua(), LUA_GCRESTART, 0);
            break;
        case 3:
            // Perform a full garbage collection cycle and return to previous strategy.
            lua_gc(GEnv.ScriptEngine->lua(), LUA_GCCOLLECT, 0);
            *value = prev;
            break;
        }
    }
};

#include "GamePersistent.h"

class CCC_MainMenu : public IConsole_Command
{
public:
    CCC_MainMenu(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; };
    virtual void Execute(LPCSTR args)
    {
        bool bWhatToDo = TRUE;
        if (0 == xr_strlen(args))
        {
            bWhatToDo = !MainMenu()->IsActive();
        };

        if (EQ(args, "on") || EQ(args, "1"))
            bWhatToDo = TRUE;

        if (EQ(args, "off") || EQ(args, "0"))
            bWhatToDo = FALSE;

        MainMenu()->Activate(bWhatToDo);
    }
};

class CCC_UIStyle : public CCC_Token
{
    u32 m_id = 0;

public:
    CCC_UIStyle(pcstr name) : CCC_Token(name, &m_id, nullptr) { }

    void Execute(pcstr args) override
    {
        CCC_Token::Execute(args);
        UIStyles->SetupStyle(m_id);
    }

    const xr_token* GetToken() noexcept override // may throw exceptions!
    {
        return UIStyles->GetToken().data();
    }
};

class CCC_UIRestart : public IConsole_Command
{
public:
    CCC_UIRestart(pcstr name) : IConsole_Command(name) { bEmptyArgsHandled = true; }

    void Execute(pcstr /*args*/) override
    {
        UIStyles->Reset();
    }
};

struct CCC_StartTimeSingle : public IConsole_Command
{
    CCC_StartTimeSingle(LPCSTR N) : IConsole_Command(N){};
    virtual void Execute(LPCSTR args)
    {
        u32 year = 1, month = 1, day = 1, hours = 0, mins = 0, secs = 0, milisecs = 0;
        sscanf(args, "%d.%d.%d %d:%d:%d.%d", &year, &month, &day, &hours, &mins, &secs, &milisecs);
        year = _max(year, 1);
        month = _max(month, 1);
        day = _max(day, 1);
        g_qwStartGameTime = generate_time(year, month, day, hours, mins, secs, milisecs);

        if (!g_pGameLevel)
            return;

        if (!Level().Server)
            return;

        if (!Level().Server->GetGameState())
            return;

        Level().SetGameTimeFactor(g_qwStartGameTime, g_fTimeFactor);
    }

    void GetStatus(TStatus& S) override
    {
        u32 year = 1, month = 1, day = 1, hours = 0, mins = 0, secs = 0, milisecs = 0;
        split_time(g_qwStartGameTime, year, month, day, hours, mins, secs, milisecs);
        xr_sprintf(S, "%d.%d.%d %d:%d:%d.%d", year, month, day, hours, mins, secs, milisecs);
    }
};

struct CCC_TimeFactorSingle : public CCC_Float
{
    CCC_TimeFactorSingle(LPCSTR N, float* V, float _min = 0.f, float _max = 1.f) : CCC_Float(N, V, _min, _max){};

    virtual void Execute(LPCSTR args)
    {
        CCC_Float::Execute(args);

        if (!g_pGameLevel)
            return;

        if (!Level().Server)
            return;

        if (!Level().Server->GetGameState())
            return;

        Level().SetGameTimeFactor(g_fTimeFactor);
    }
};

#ifdef DEBUG
class CCC_RadioGroupMask2;
class CCC_RadioMask : public CCC_Mask
{
    CCC_RadioGroupMask2* group;

public:
    CCC_RadioMask(LPCSTR N, Flags32* V, u32 M) : CCC_Mask(N, V, M) { group = NULL; }
    void SetGroup(CCC_RadioGroupMask2* G) { group = G; }
    virtual void Execute(LPCSTR args);

    IC void Set(BOOL V) { value->set(mask, V); }
};

class CCC_RadioGroupMask2
{
    CCC_RadioMask* mask0;
    CCC_RadioMask* mask1;

public:
    CCC_RadioGroupMask2(CCC_RadioMask* m0, CCC_RadioMask* m1)
    {
        mask0 = m0;
        mask1 = m1;
        mask0->SetGroup(this);
        mask1->SetGroup(this);
    }
    void Execute(CCC_RadioMask& m, LPCSTR args)
    {
        BOOL value = m.GetValue();
        if (value)
        {
            mask0->Set(!value);
            mask1->Set(!value);
        }
        m.Set(value);
    }
};

void CCC_RadioMask::Execute(LPCSTR args)
{
    CCC_Mask::Execute(args);
    VERIFY2(group, "CCC_RadioMask: group not set");
    group->Execute(*this, args);
}

#define CMD_RADIOGROUPMASK2(p1, p2, p3, p4, p5, p6)                                        \
    \
{                                                                                   \
        \
static CCC_RadioMask x##CCC_RadioMask1(p1, p2, p3);                                        \
        Console->AddCommand(&x##CCC_RadioMask1);                                           \
        \
static CCC_RadioMask x##CCC_RadioMask2(p4, p5, p6);                                        \
        Console->AddCommand(&x##CCC_RadioMask2);                                           \
        \
static CCC_RadioGroupMask2 x##CCC_RadioGroupMask2(&x##CCC_RadioMask1, &x##CCC_RadioMask2); \
    \
}

struct CCC_DbgBullets : public CCC_Integer
{
    CCC_DbgBullets(LPCSTR N, int* V, int _min = 0, int _max = 999) : CCC_Integer(N, V, _min, _max){};

    virtual void Execute(LPCSTR args)
    {
        extern xr_vector<Fvector> g_hit[];
        g_hit[0].clear();
        g_hit[1].clear();
        g_hit[2].clear();
        CCC_Integer::Execute(args);
    }
};

#include "attachable_item.h"
#include "attachment_owner.h"
#include "InventoryOwner.h"
#include "Inventory.h"
class CCC_TuneAttachableItem : public IConsole_Command
{
public:
    CCC_TuneAttachableItem(LPCSTR N) : IConsole_Command(N){};
    virtual void Execute(LPCSTR args)
    {
        if (CAttachableItem::m_dbgItem)
        {
            CAttachableItem::m_dbgItem = NULL;
            Msg("CCC_TuneAttachableItem switched to off");
            return;
        };

        IGameObject* obj = Level().CurrentViewEntity();
        VERIFY(obj);
        shared_str ssss = args;

        CAttachmentOwner* owner = smart_cast<CAttachmentOwner*>(obj);
        CAttachableItem* itm = owner->attachedItem(ssss);
        if (itm)
        {
            CAttachableItem::m_dbgItem = itm;
        }
        else
        {
            CInventoryOwner* iowner = smart_cast<CInventoryOwner*>(obj);
            PIItem active_item = iowner->m_inventory->ActiveItem();
            if (active_item && active_item->object().cNameSect() == ssss)
                CAttachableItem::m_dbgItem = active_item->cast_attachable_item();
        }

        if (CAttachableItem::m_dbgItem)
            Msg("CCC_TuneAttachableItem switched to ON for [%s]", args);
        else
            Msg("CCC_TuneAttachableItem cannot find attached item [%s]", args);
    }

    virtual void Info(TInfo& I)
    {
        xr_sprintf(I,
            "allows to change bind rotation and position offsets for attached item, <section_name> given as arguments");
    }
};

class CCC_DumpModelBones : public IConsole_Command
{
public:
    CCC_DumpModelBones(LPCSTR N) : IConsole_Command(N) {}
    virtual void Execute(LPCSTR arguments)
    {
        if (!arguments || !*arguments)
        {
            Msg("! no arguments passed");
            return;
        }

        LPCSTR name;

        if (0 == strext(arguments))
            STRCONCAT(name, arguments, ".ogf");
        else
            STRCONCAT(name, arguments);

        string_path fn;

        if (!FS.exist(arguments) && !FS.exist(fn, "$level$", name) && !FS.exist(fn, "$game_meshes$", name))
        {
            Msg("! Cannot find visual \"%s\"", arguments);
            return;
        }

        IRenderVisual* visual = GEnv.Render->model_Create(arguments);
        IKinematics* kinematics = smart_cast<IKinematics*>(visual);
        if (!kinematics)
        {
            GEnv.Render->model_Delete(visual);
            Msg("! Invalid visual type \"%s\" (not a IKinematics)", arguments);
            return;
        }

        Msg("bones for model \"%s\"", arguments);
        for (u16 i = 0, n = kinematics->LL_BoneCount(); i < n; ++i)
            Msg("%s", kinematics->LL_GetData(i).name.c_str());

        GEnv.Render->model_Delete(visual);
    }
};

extern void show_animation_stats();

class CCC_ShowAnimationStats : public IConsole_Command
{
public:
    CCC_ShowAnimationStats(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; };
    virtual void Execute(LPCSTR) { show_animation_stats(); }
};

class CCC_InvUpgradesHierarchy : public IConsole_Command
{
public:
    CCC_InvUpgradesHierarchy(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };
    virtual void Execute(LPCSTR args)
    {
        if (ai().get_alife())
        {
            ai().alife().inventory_upgrade_manager().log_hierarchy();
        }
    }
};

class CCC_InvUpgradesCurItem : public IConsole_Command
{
public:
    CCC_InvUpgradesCurItem(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };
    virtual void Execute(LPCSTR args)
    {
        if (!g_pGameLevel)
        {
            return;
        }
        CUIGameSP* ui_game_sp = smart_cast<CUIGameSP*>(CurrentGameUI());
        if (!ui_game_sp)
        {
            return;
        }
        PIItem item = ui_game_sp->GetActorMenu().get_upgrade_item();
        if (item)
        {
            item->log_upgrades();
        }
        else
        {
            Msg("- Current item in ActorMenu is unknown!");
        }
    }
};

class CCC_InvDropAllItems : public IConsole_Command
{
public:
    CCC_InvDropAllItems(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };
    virtual void Execute(LPCSTR args)
    {
        if (!g_pGameLevel)
        {
            return;
        }
        CUIGameSP* ui_game_sp = smart_cast<CUIGameSP*>(CurrentGameUI());
        if (!ui_game_sp)
        {
            return;
        }
        int d = 0;
        sscanf(args, "%d", &d);
        if (ui_game_sp->GetActorMenu().DropAllItemsFromRuck(d == 1))
        {
            Msg("- All items from ruck of Actor is dropping now.");
        }
        else
        {
            Msg("! ActorMenu is not in state `Inventory`");
        }
    }
}; // CCC_InvDropAllItems

#endif // DEBUG

class CCC_DumpObjects : public IConsole_Command
{
public:
    CCC_DumpObjects(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; };
    virtual void Execute(LPCSTR) { Level().Objects.dump_all_objects(); }
};

class CCC_GSCheckForUpdates : public IConsole_Command
{
    bool m_informNoPatch = true;

    void SetupCallParams(pcstr args)
    {
        m_informNoPatch = true;
        if (args && *args)
        {
            int bInfo = 1;
            sscanf(args, "%d", &bInfo);
            m_informNoPatch = (bInfo != 0);
        }
    }

public:
    CCC_GSCheckForUpdates(LPCSTR N) : IConsole_Command(N)
    {
        bEmptyArgsHandled = true;
    };

    virtual void Execute(LPCSTR arguments)
    {
        auto mm = MainMenu();
        if (!mm)
            return;

        SetupCallParams(arguments);

        if (m_informNoPatch)
        {
            mm->OnPatchCheck(false);
        }
    }
};

class CCC_Net_SV_GuaranteedPacketMode : public CCC_Integer
{
protected:
    int* value_blin;

public:
    CCC_Net_SV_GuaranteedPacketMode(LPCSTR N, int* V, int _min = 0, int _max = 2)
        : CCC_Integer(N, V, _min, _max), value_blin(V){};

    virtual void Execute(LPCSTR args) { CCC_Integer::Execute(args); }
};
#ifdef DEBUG
void DBG_CashedClear();
class CCC_DBGDrawCashedClear : public IConsole_Command
{
public:
    CCC_DBGDrawCashedClear(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; }
private:
    virtual void Execute(LPCSTR args) { DBG_CashedClear(); }
};

#endif

class CCC_DbgVar : public IConsole_Command
{
public:
    CCC_DbgVar(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = false; };
    virtual void Execute(LPCSTR arguments)
    {
        if (!arguments || !*arguments)
        {
            return;
        }

        if (_GetItemCount(arguments, ' ') == 1)
        {
            ai_dbg::show_var(arguments);
        }
        else
        {
            char name[1024];
            float f;
            sscanf(arguments, "%s %f", name, &f);
            ai_dbg::set_var(name, f);
        }
    }
};

class CCC_CleanupTasks : public IConsole_Command
{
public:
    CCC_CleanupTasks(pcstr name) : IConsole_Command(name) {}
    void Execute(pcstr /*args*/) override
    {
        Level().GameTaskManager().CleanupTasks();
    }
};

class CCC_UI_Time_Dilation_Mode : public IConsole_Command
{
    UITimeDilator::UIMode mode;
    bool isEnable;

public:
    CCC_UI_Time_Dilation_Mode(pcstr name, UITimeDilator::UIMode mode) : IConsole_Command(name), mode(mode) {};

    void Execute(pcstr args) override
    {
        if (EQ(args, "on") || EQ(args, "1"))
        {
            TimeDilator()->SetModeEnability(mode, true);
            isEnable = true;
        }
        else if (EQ(args, "off") || EQ(args, "0"))
        {
            TimeDilator()->SetModeEnability(mode, false);
            isEnable = false;
        }
        else
            InvalidSyntax();
    }

    void GetStatus(TStatus& status) override
    {
        xr_strcpy(status, isEnable ? "on" : "off");
    }

    void Info(TInfo& info) override
    {
        xr_strcpy(info, "'on/off' or '1/0'");
    }

    void fill_tips(vecTips& tips, u32 /*mode*/) override
    {
        TStatus str;
        xr_sprintf(str, sizeof(str), "%s (current) [on/off]", isEnable ? "on" : "off");
        tips.push_back(str);
    }
};

class CCC_UI_Time_Factor : public IConsole_Command
{
    float uiTimeFactor = 1.0;

public:
    CCC_UI_Time_Factor(pcstr name) : IConsole_Command(name){};

    void Execute(pcstr args) override
    {
        float time_factor = (float)atof(args);
        clamp(time_factor, EPS, 1.f);
        TimeDilator()->SetUiTimeFactor(time_factor);
        uiTimeFactor = time_factor;
    }

    void Info(TInfo& info) override
    {
        xr_strcpy(info, "[0.001 - 1.0]");
    }

    void fill_tips(vecTips& tips, u32 mode) override
    {
        TStatus str;
        xr_sprintf(str, sizeof(str), "%3.3f (current) [0.001 - 1.0]", uiTimeFactor);
        tips.push_back(str);
    }

    void GetStatus(TStatus& status) override
    {
        xr_sprintf(status, sizeof(status), "%f", uiTimeFactor);
    }
};

class CCC_LuaProfiler : public IConsole_Command
{
public:
    constexpr static cpcstr COMMAND_LUA_PROFILER_STATUS = "lua_profiler_status";
    constexpr static cpcstr COMMAND_LUA_PROFILER_START = "lua_profiler_start";
    constexpr static cpcstr COMMAND_LUA_PROFILER_START_HOOK_MODE = "lua_profiler_start_hook_mode";
    constexpr static cpcstr COMMAND_LUA_PROFILER_START_SAMPLING_MODE = "lua_profiler_start_sampling_mode";
    constexpr static cpcstr COMMAND_LUA_PROFILER_STOP = "lua_profiler_stop";
    constexpr static cpcstr COMMAND_LUA_PROFILER_RESET = "lua_profiler_reset";
    constexpr static cpcstr COMMAND_LUA_PROFILER_LOG = "lua_profiler_log";
    constexpr static cpcstr COMMAND_LUA_PROFILER_SAVE = "lua_profiler_save";

    CCC_LuaProfiler(pcstr name) : IConsole_Command(name) { bEmptyArgsHandled = true; }

    void Execute(pcstr args) override
    {
        CScriptProfiler* profiler = GEnv.ScriptEngine->m_profiler;

        if (strstr(cName, COMMAND_LUA_PROFILER_STATUS) == cName)
        {
            Msg("[P] Profiler status: %s, type - %s", profiler->IsActive() ? "on" : "off",
                profiler->GetTypeString().c_str());
        }
        else if (strstr(cName, COMMAND_LUA_PROFILER_START_HOOK_MODE) == cName)
        {
            profiler->StartHookMode();
        }
        else if (strstr(cName, COMMAND_LUA_PROFILER_START_SAMPLING_MODE) == cName)
        {
            u32 interval = atoi(args);

            profiler->StartSamplingMode(interval ? interval : CScriptProfiler::PROFILE_SAMPLING_INTERVAL_DEFAULT);
        }
        else if (strstr(cName, COMMAND_LUA_PROFILER_START) == cName)
        {
            u32 profiler_type = atoi(args);

            profiler->Start(profiler_type ? (CScriptProfilerType)profiler_type : CScriptProfiler::PROFILE_TYPE_DEFAULT);
        }
        else if (strstr(cName, COMMAND_LUA_PROFILER_STOP) == cName)
        {
            profiler->Stop();
        }
        else if (strstr(cName, COMMAND_LUA_PROFILER_RESET) == cName)
        {
            profiler->Reset();
        }
        else if (strstr(cName, COMMAND_LUA_PROFILER_LOG) == cName)
        {
            u32 limit = atoi(args);

            profiler->LogReport(limit ? limit : CScriptProfiler::PROFILE_ENTRIES_LOG_LIMIT_DEFAULT);
        }
        else if (strstr(cName, COMMAND_LUA_PROFILER_SAVE) == cName)
        {
            profiler->SaveReport();
        }
    }

    void fill_tips(vecTips& tips, u32 /*mode*/) override
    {
        TStatus status_buffer;

        if (strstr(cName, COMMAND_LUA_PROFILER_STATUS) == cName)
        {
            // No arguments.
        }
        else if (strstr(cName, COMMAND_LUA_PROFILER_START_HOOK_MODE) == cName)
        {
            // No arguments.
        }
        else if (strstr(cName, COMMAND_LUA_PROFILER_START_SAMPLING_MODE) == cName)
        {
            xr_sprintf(status_buffer, "%d (default) [1-%d] - sampling interval",
                CScriptProfiler::PROFILE_SAMPLING_INTERVAL_DEFAULT, CScriptProfiler::PROFILE_SAMPLING_INTERVAL_MAX);
            tips.emplace_back(status_buffer);
        }
        else if (strstr(cName, COMMAND_LUA_PROFILER_START) == cName)
        {
            xr_sprintf(status_buffer, "%d - hooks based profiler", CScriptProfilerType::Hook);
            tips.emplace_back(status_buffer);

            xr_sprintf(status_buffer, "%d - sampling based profiler", CScriptProfilerType::Sampling);
            tips.emplace_back(status_buffer);
        }
        else if (strstr(cName, COMMAND_LUA_PROFILER_STOP) == cName)
        {
            // No arguments.
        }
        else if (strstr(cName, COMMAND_LUA_PROFILER_RESET) == cName)
        {
            // No arguments.
        }
        else if (strstr(cName, COMMAND_LUA_PROFILER_LOG) == cName)
        {
            xr_sprintf(status_buffer, "%d (default) - count of profiling entries to print",
                CScriptProfiler::PROFILE_ENTRIES_LOG_LIMIT_DEFAULT, CScriptProfiler::PROFILE_SAMPLING_INTERVAL_MAX);
            tips.emplace_back(status_buffer);
        }
        else if (strstr(cName, COMMAND_LUA_PROFILER_SAVE) == cName)
        {
            // No arguments.
        }
    }

    void Info(TInfo& info) override
    {
        if (strstr(cName, COMMAND_LUA_PROFILER_STATUS) == cName)
            xr_strcpy(info, "no arguments : print lua profiler status");
        else if (strstr(cName, COMMAND_LUA_PROFILER_START_HOOK_MODE) == cName)
            xr_strcpy(info, "no arguments : start lua script profiling in hook mode");
        else if (strstr(cName, COMMAND_LUA_PROFILER_START_SAMPLING_MODE) == cName)
            xr_strcpy(info,
                "integer value in range [1,1000] : start lua script profiling in sampling mode with provided sampling "
                "interval");
        else if (strstr(cName, COMMAND_LUA_PROFILER_START) == cName)
            xr_strcpy(info, "integer value in range [0,2] : start lua script profiling in provided mode");
        else if (strstr(cName, COMMAND_LUA_PROFILER_STOP) == cName)
            xr_strcpy(info, "no arguments : stop lua script profiling");
        else if (strstr(cName, COMMAND_LUA_PROFILER_RESET) == cName)
            xr_strcpy(info, "no arguments : reset lua script profiling stats");
        else if (strstr(cName, COMMAND_LUA_PROFILER_LOG) == cName)
            xr_strcpy(info, "integer value : log lua script profiling stats, limit entries with argument");
        else if (strstr(cName, COMMAND_LUA_PROFILER_SAVE) == cName)
            xr_strcpy(info, "no arguments : save lua script profiling stats in a file");
    }
};

class CCC_FiniteBolts final : public CCC_Mask
{
public:
    explicit CCC_FiniteBolts(pcstr name) : CCC_Mask(name, &psActorFlags, AF_FINITE_BOLTS) {}

    void Execute(pcstr args) override
    {
        CCC_Mask::Execute(args);
        g_infinite_bolts = GetValue() ? 0 : 1;
        if (g_infinite_bolts)
            RequestInfiniteBoltRestock();
    }
};

class CCC_InfiniteBolts final : public CCC_Integer
{
public:
    explicit CCC_InfiniteBolts(pcstr name) : CCC_Integer(name, &g_infinite_bolts, 0, 1) {}

    void Execute(pcstr args) override
    {
        CCC_Integer::Execute(args);
        psActorFlags.set(AF_FINITE_BOLTS, !GetValue());
        if (GetValue())
            RequestInfiniteBoltRestock();
    }
};

// Scripted camera rotation, for QA runs on a headless rig where there is no mouse: turn or
// look up/down at a fixed rate. Pitch is clamped to the camera's own limits.
class CCC_CameraRotate : public IConsole_Command
{
    using rotate_fn = void (*)(float, float);
    rotate_fn m_apply;

public:
    CCC_CameraRotate(LPCSTR name, rotate_fn apply) : IConsole_Command(name), m_apply(apply) {}

    void Execute(LPCSTR args) override
    {
        float speed{};
        float duration{-1.f};
        const int parsed = sscanf(args, "%f %f", &speed, &duration);
        if (parsed < 1 || (parsed > 1 && duration < 0.f && !fsimilar(duration, -1.f)))
        {
            Msg("! Usage: %s <degrees_per_second> [duration_seconds], duration -1 means continuous", cName);
            return;
        }

        m_apply(speed, duration);
        Msg("* %s: speed=%.3f deg/s, duration=%.3f s", cName, speed, duration);
    }

    void Info(TInfo& info) override
    {
        xr_strcpy(info, "speed_deg_per_second [duration_seconds=-1]");
    }
};


// qa_water_goto [pitch] [wade]: the actor stood IN the level's water, looking at it.
//
// The old version walked the collision for a liquid vertex and put the actor a metre and a half
// short of it, which is a shot of the bank about as often as it is a shot of the water - the
// marsh is a lattice of reed islands and the nearest liquid triangle is usually across one. The
// baked field answers the question directly: find a texel that has water over a bed the actor can
// stand on, at least `wade` deep, and put the feet on that bed.
class CCC_QaWaterGoto : public IConsole_Command
{
public:
    CCC_QaWaterGoto(pcstr name) : IConsole_Command(name) { bEmptyArgsHandled = true; }
    void Execute(pcstr args) override
    {
        CActor* actor = g_pGameLevel ? smart_cast<CActor*>(Level().CurrentEntity()) : nullptr;
        if (!actor)
        {
            Msg("! [qa] qa_water_goto: no actor");
            return;
        }
        float pitch = 0.5f;
        float wade = 0.12f;
        if (args && xr_strlen(args))
            sscanf(args, "%f %f", &pitch, &wade);
        pitch = clampr(pitch, -1.5f, 1.5f);
        wade = clampr(wade, 0.02f, 3.f);

        const CEnvironment& env = GamePersistent().Environment();
        if (!env.water_field_valid())
        {
            Msg("! [qa] qa_water_goto: this level has no water field");
            return;
        }

        // Rings outward from where the actor is, so the frame stays near whatever was being
        // looked at, and the first hit is the closest water rather than the biggest.
        const Fvector from = actor->Position();
        Fvector wet{};
        float wet_depth = 0.f;
        bool found = false;
        for (float r = 2.f; r <= 400.f && !found; r *= 1.35f)
        {
            for (int a = 0; a < 32 && !found; ++a)
            {
                const float ang = PI_MUL_2 * float(a) / 32.f;
                Fvector p = from;
                p.x += _cos(ang) * r;
                p.z += _sin(ang) * r;
                float surface, bed;
                if (!env.water_at(p, surface, bed))
                    continue;
                const float d = surface - bed;
                if (d < wade)
                    continue;
                wet.set(p.x, bed, p.z);
                wet_depth = d;
                found = true;
            }
        }
        if (!found)
        {
            Msg("! [qa] qa_water_goto: no water at least %.2f m deep anywhere near the actor", wade);
            return;
        }

        // Face whichever way has the most water in front, so a downward camera frames the
        // surface and not the bank two metres away.
        float best_open = -1.f;
        float best_yaw = 0.f;
        for (int a = 0; a < 16; ++a)
        {
            const float yaw = PI_MUL_2 * float(a) / 16.f;
            float open = 0.f;
            for (float d = 2.f; d <= 24.f; d += 2.f)
            {
                Fvector q = wet;
                q.x += _sin(yaw) * d;
                q.z += _cos(yaw) * d;
                float su, bd;
                if (env.water_at(q, su, bd))
                    open = d;
                else
                    break;
            }
            if (open > best_open)
            {
                best_open = open;
                best_yaw = yaw;
            }
        }

        // MoveActor takes angles: x = -torso pitch, y = -yaw, with forward = (sin yaw, cos yaw).
        actor->MoveActor(wet, Fvector{ pitch, -best_yaw, 0.f });
        Msg("* [qa] standing in %.2f m of water at (%.1f, %.1f, %.1f), %.0f m of it ahead, yaw %.0f",
            wet_depth, wet.x, wet.y, wet.z, best_open, rad2deg(best_yaw));
    }
};

// qa_water_state: what the water model currently believes. The surface is driven entirely by
// solved quantities - the bodies measured off the level's liquid collision, the sea state the
// wind raises over them, the optical profile the level's LTX picked - and none of it is visible
// from a screenshot, so a run that looks wrong cannot otherwise be told from a run that IS wrong.
class CCC_QaWaterState : public IConsole_Command
{
public:
    CCC_QaWaterState(pcstr name) : IConsole_Command(name) { bEmptyArgsHandled = true; }
    void Execute(pcstr) override
    {
        if (!g_pGamePersistent)
            return;
        const CEnvironment& env = g_pGamePersistent->Environment();

        Msg("* [qa] water: %d body(s) measured on this level", env.water_body_count);
        for (int i = 0; i < env.water_body_count; ++i)
        {
            const auto& b = env.water_bodies[i];
            Msg("* [qa]   body %d: %.0f x %.0f m at (%.0f, %.0f, %.0f), depth %.2f m", i,
                b.extent.vMax.x - b.extent.vMin.x, b.extent.vMax.z - b.extent.vMin.z,
                0.5f * (b.extent.vMin.x + b.extent.vMax.x), b.extent.vMin.y,
                0.5f * (b.extent.vMin.z + b.extent.vMax.z), b.depth);
        }
        // The baked field and the ripple window are the two things part 2 hangs off, and
        // neither is visible in a screenshot: a dead field looks exactly like calm water.
        if (env.water_field_valid())
        {
            const Fvector p = Device.vCameraPosition;
            const float surf = env.water_surface_at(p);
            Msg("* [qa] field: %dx%d over %.0f x %.0f m; under the camera surface %s, shore %.1f m",
                CEnvironment::water_field_dim, CEnvironment::water_field_dim,
                env.water_field_bounds.vMax.x - env.water_field_bounds.vMin.x,
                env.water_field_bounds.vMax.z - env.water_field_bounds.vMin.z,
                surf > -flt_max * 0.5f ? "yes" : "no", env.water_shore_dist(p));
        }
        else
            Msg("* [qa] field: not baked on this level");
        Msg("* [qa] ripple: window %.0f m at (%.1f, %.1f), 1/texels %.5f, spectral at depth %.2f m, "
            "analytic front %.2f m/s%s",
            env.water_ripple_win.z, env.water_ripple_win.x, env.water_ripple_win.y, env.water_ripple_win.w,
            env.water_body.x, env.water_ripple_speed, env.water_ripple_win.z > 0.f ? "" : "  (field off - analytic rings only)");
        Msg("* [qa] under water: %.2f m below a surface at %.2f", env.eye_under_depth, env.eye_under_surface);
        Msg("* [qa] rain: %.1f mm/h, extinction %.2f 1/km", env.rain_rate_mmh, env.rain_ext_km);
        Msg("* [qa] sea: Hs %.3f m, peak %.2f m, mss %.4f, wind %.2f m/s (raw %.2f)", env.water_sea.x,
            env.water_sea.y, env.water_sea.z, env.water_sea.w, env.WindSpeedMs());
        Msg("* [qa] here: depth %.2f m, fetch %.0f m, %d wave row(s)", env.water_body.x, env.water_body.y,
            int(env.water_body.w));
        for (int i = 0; i < int(env.water_body.w); ++i)
        {
            const float* row = &env.water_wave[i / 4].m[i % 4][0];
            Msg("* [qa]   wave %d: heading %.0f deg, %.3f m long, amp %.4f m, slope %.4f", i,
                rad2deg(row[0]), PI_MUL_2 / _max(row[1], EPS_S), row[2], row[1] * row[2]);
        }
        for (int slot = 0; slot < 2; ++slot)
        {
            const auto& pr = env.water_profile[slot];
            Msg("* [qa] profile %c: sigma_t (%.2f, %.2f, %.2f) 1/m, body (%.3f, %.3f, %.3f), scum %.2f, "
                "fetch_max %.0f m, wave_damp %.2f",
                slot ? 'B' : 'A', pr.sigma_t.x, pr.sigma_t.y, pr.sigma_t.z, pr.body_r.x, pr.body_r.y,
                pr.body_r.z, pr.scum, pr.fetch_max, pr.wave_damp);
        }
        for (const auto& w : env.water_wakes)
            if (w.used)
                Msg("* [qa] wake %u: at (%.1f, %.2f, %.1f), r %.2f m, strength %.2f, foot velocity (%.2f, %.2f) m/s, fed %.2f s ago",
                    w.id, w.pos.x, w.pos.y, w.pos.z, w.radius, w.strength, w.vel.x, w.vel.y, Device.fTimeGlobal - w.touched);
        // The wading source itself: where the engine thinks the feet are, against the body.
        if (CActor* actor = g_pGameLevel ? smart_cast<CActor*>(Level().CurrentEntity()) : nullptr)
        {
            const Fvector p = actor->Position();
            Fvector vel{};
            if (CCharacterPhysicsSupport* cps = actor->character_physics_support(); cps && cps->movement())
                vel = cps->movement()->GetVelocity();
            Msg("* [qa] actor at (%.2f, %.2f, %.2f), velocity (%.2f, %.2f, %.2f) m/s", p.x, p.y, p.z, vel.x, vel.y, vel.z);
            if (IKinematics* K = smart_cast<IKinematics*>(actor->Visual()))
                for (pcstr name : {"bip01_l_foot", "bip01_r_foot"})
                {
                    const u16 id = K->LL_BoneID(name);
                    if (id == BI_NONE)
                    {
                        Msg("* [qa]   bone %s: none", name);
                        continue;
                    }
                    Fmatrix g;
                    g.mul_43(actor->XFORM(), K->LL_GetBoneInstance(id).mTransform);
                    Msg("* [qa]   %s at (%.2f, %.2f, %.2f)", name, g.c.x, g.c.y, g.c.z);
                }
        }
    }
};

// qa_water_dive [metres]: the actor put UNDER the nearest water surface deep enough that the
// camera is below it. Nothing else in the game can get the camera under water on purpose, so
// without this the underwater path, the surface seen from below and the caustics have no repro.
class CCC_QaWaterDive : public IConsole_Command
{
public:
    CCC_QaWaterDive(pcstr name) : IConsole_Command(name) { bEmptyArgsHandled = true; }
    void Execute(pcstr args) override
    {
        CActor* actor = g_pGameLevel ? smart_cast<CActor*>(Level().CurrentEntity()) : nullptr;
        if (!actor)
        {
            Msg("! [qa] qa_water_dive: no actor");
            return;
        }
        float below = 1.2f;
        if (args && xr_strlen(args))
            sscanf(args, "%f", &below);

        const CEnvironment& env = GamePersistent().Environment();
        if (!env.water_field_valid())
        {
            Msg("! [qa] qa_water_dive: this level has no water field");
            return;
        }

        // Walk out from the actor over the level's water bodies and take the deepest spot found:
        // the shore the other probes aim for is exactly where the camera cannot get under. Open
        // sky wins over depth, because the deepest water on a level is usually a flooded cellar
        // and a frame shot in the dark says nothing about how the medium looks.
        Fvector best = actor->Position();
        float best_depth = -1.f;
        bool best_open = false;
        for (int i = 0; i < env.water_body_count; ++i)
        {
            const Fbox& e = env.water_bodies[i].extent;
            for (int gz = 0; gz < 12; ++gz)
                for (int gx = 0; gx < 12; ++gx)
                {
                    Fvector p;
                    p.x = e.vMin.x + (e.vMax.x - e.vMin.x) * (gx + 0.5f) / 12.f;
                    p.z = e.vMin.z + (e.vMax.z - e.vMin.z) * (gz + 0.5f) / 12.f;
                    p.y = e.vMax.y;
                    float surface, bed;
                    if (!env.water_at(p, surface, bed))
                        continue;
                    const float d = surface - bed;
                    if (d < 2.0f)
                        continue; // a standing camera cannot submerge in less
                    Fvector probe{ p.x, surface + 0.5f, p.z };
                    const bool open = !env.wind_sheltered(probe);
                    if ((open && !best_open) || (open == best_open && d > best_depth))
                    {
                        best_depth = d;
                        best_open = open;
                        best.set(p.x, surface, p.z);
                    }
                }
        }
        if (best_depth <= 0.f)
        {
            Msg("! [qa] qa_water_dive: no water on this level is 2 m deep, so a standing camera "
                "cannot get under any of it");
            return;
        }

        // The camera rides about 1.7 m over the feet, so the feet have to go that far under the
        // surface plus whatever submersion was asked for - and they cannot go through the bed.
        // Below about two metres of water there is nowhere for a standing camera to be both
        // under the surface and above the bottom, so say so rather than quietly framing a shot
        // from inside the terrain, which looks like an underwater bug and is not one.
        constexpr float eye_height = 1.7f;
        const float want = eye_height + below;
        const float room = best_depth - eye_height;
        const float drop = (want > best_depth) ? best_depth : want;
        best.y -= drop;
        actor->MoveActor(best, Fvector{ 0.f, 0.f, 0.f });
        if (room <= 0.05f)
            Msg("! [qa] dived at (%.1f, %.1f, %.1f), but %.2f m of water cannot submerge a %.1f m "
                "camera: the frame is from inside the bed, not from in the water",
                best.x, best.y, best.z, best_depth, eye_height);
        else
            Msg("* [qa] dived at (%.1f, %.1f, %.1f), water %.2f m deep, camera %.2f m under",
                best.x, best.y, best.z, best_depth, _min(below, room));
    }
};

// visor_wipe [delay] [duration]: a hand across the visor, starting delay seconds from now and
// taking duration to cross. The mask-cleaning script calls this when the cleaning item is
// actually in the hands, and the delay is what is left of the draw animation before the hand
// reaches the glass - both live in the script, because both belong to an animation and an
// animation is data. The renderer turns it into a sweep that collects the water ahead of the
// hand and leaves a smeared film behind it. Shipped rather than QA: it is how the game asks.
class CCC_VisorWipe : public IConsole_Command
{
public:
    CCC_VisorWipe(pcstr name) : IConsole_Command(name) { bEmptyArgsHandled = true; }
    void Execute(pcstr args) override
    {
        if (!g_pGamePersistent)
            return;
        float delay = 0.f, len = 0.55f;
        if (args && xr_strlen(args))
            sscanf(args, "%f %f", &delay, &len);
        GamePersistent().Environment().visor_wipe(delay, len);
    }
};

// visor_rate <0..1>: how hard the visor is being wetted, from the mod's own driver. Its own
// channel, because the console float the older drivers write is fought over - one of them
// re-asserts its ramp once a second, and in light rain that ramp is zero.
class CCC_VisorRate : public IConsole_Command
{
public:
    CCC_VisorRate(pcstr name) : IConsole_Command(name) { bEmptyArgsHandled = true; }
    void Execute(pcstr args) override
    {
        if (!g_pGamePersistent)
            return;
        float v = -1.f;
        if (args && xr_strlen(args))
            sscanf(args, "%f", &v);
        GamePersistent().Environment().visor.rate = (v < 0.f) ? -1.f : clampr(v, 0.f, 1.f);
    }
};

// qa_visor_blob <radius mm>: hold a cap of water at the centre of the visor, 0 to let it go. The
// optics of a drop cannot be judged on a bead nine pixels wide.
class CCC_QaVisorBlob : public IConsole_Command
{
public:
    CCC_QaVisorBlob(pcstr name) : IConsole_Command(name) { bEmptyArgsHandled = true; }
    void Execute(pcstr args) override
    {
        if (!g_pGamePersistent)
            return;
        float r = 0.f;
        if (args && xr_strlen(args))
            sscanf(args, "%f", &r);
        GamePersistent().Environment().visor.qa_blob = clampr(r, 0.f, 40.f);
    }
};

// qa_visor_wet [0..1]: pin the rate the visor is being wetted at, or -1 to hand it back to the
// drivers. The stand wears no mask and no driver pushes anything there, so without this the
// field is correctly empty and the probe photographs clean glass.
class CCC_QaVisorWet : public IConsole_Command
{
public:
    CCC_QaVisorWet(pcstr name) : IConsole_Command(name) { bEmptyArgsHandled = true; }
    void Execute(pcstr args) override
    {
        if (!g_pGamePersistent)
            return;
        float v = -1.f;
        if (args && xr_strlen(args))
            sscanf(args, "%f", &v);
        GamePersistent().Environment().visor.qa_wet = (v < 0.f) ? -1.f : clampr(v, 0.f, 1.f);
        Msg("* [qa] visor wetting rate %s", (v < 0.f) ? "back to the drivers" : "pinned");
    }
};

// qa_hands_state: why nothing can be drawn. Every gate between a key press and an item in the
// hands, in the order the game consults them, because when the hands stay empty the one thing
// nobody can see is WHICH of them is holding. A blocked slot is a refcount with several owners -
// a ladder, a car, the death effector, a bloodsucker, a script scene - so the count matters as
// much as the fact.
class CCC_QaHandsState : public IConsole_Command
{
public:
    CCC_QaHandsState(pcstr name) : IConsole_Command(name) { bEmptyArgsHandled = true; }
    void Execute(pcstr /*args*/) override
    {
        CActor* actor = Actor();
        if (!actor)
        {
            Msg("* [qa] hands: no actor");
            return;
        }

        CInventory& inv = actor->inventory();
        string512 blocked;
        xr_strcpy(blocked, sizeof(blocked), "");
        for (u16 i = inv.FirstSlot(); i <= inv.LastSlot(); ++i)
        {
            string32 one;
            xr_sprintf(one, sizeof(one), "%s%d", (i == inv.FirstSlot()) ? "" : " ", inv.BlockedCount(i));
            xr_strcat(blocked, sizeof(blocked), one);
        }
        Msg("* [qa] hands: slots active %d, next %d, prev %d", inv.GetActiveSlot(), inv.GetNextActiveSlot(),
            inv.GetPrevActiveSlot());
        Msg("* [qa] hands: blocked per slot [%d..%d] = %s", inv.FirstSlot(), inv.LastSlot(), blocked);

        // A hud item wedged out of eIdle stalls CInventory::Update every frame, and nothing
        // anywhere says so: the slot switch simply never completes.
        for (int which = 0; which < 2; ++which)
        {
            const u16 slot = which ? inv.GetNextActiveSlot() : inv.GetActiveSlot();
            PIItem item = (slot == NO_ACTIVE_SLOT) ? nullptr : inv.ItemFromSlot(slot);
            CHudItem* hud = item ? item->cast_hud_item() : nullptr;
            if (!hud)
                continue;
            Msg("* [qa] hands: %s item [%s] state %d, next %d, pending %d, hidden %d",
                which ? "next" : "active", item->object().cNameSect().c_str(), hud->GetState(),
                hud->GetNextState(), hud->IsPending() ? 1 : 0, hud->IsHidden() ? 1 : 0);
        }

        extern bool g_da_block_all_except_movement;
        extern bool g_bDisableAllInput;
        Msg("* [qa] hands: input - movement only %d, all input disabled %d, talking %d",
            g_da_block_all_except_movement ? 1 : 0, g_bDisableAllInput ? 1 : 0, actor->IsTalking() ? 1 : 0);
        Msg("* [qa] hands: pda3d presenter %d, ui focused %d",
            da_pda3d::presenter_active() ? 1 : 0, da_pda3d::ui_focused() ? 1 : 0);

        // The rig, and what the items in it can actually play. An item's cycles live in the
        // HANDS model when it is not monolithic, and the hands are replaced by an outfit and by
        // the 3D PDA - an item left holding ids from the previous rig is how a player's crash
        // report ended. Zero cycles here means the item has nothing to play on this rig.
        if (g_player_hud)
        {
            IKinematicsAnimated* rig = g_player_hud->hands_model();
            Msg("* [qa] hands: rig [%s], %u motion slot(s)", g_player_hud->section_name().c_str(),
                rig ? u32(rig->LL_MotionsSlotCount()) : 0u);
            for (u16 i = 0; i < 2; ++i)
            {
                const attachable_hud_item* it = g_player_hud->attached_item(i);
                if (!it)
                    continue;
                Msg("* [qa] hands: place %u item [%s] %s, %u cycle(s) bound", u32(i),
                    it->m_sect_name.c_str(), it->m_monolithic ? "own visual" : "on the hands rig",
                    u32(it->m_hand_motions.m_anims.size()));
            }
        }
    }
};

// qa_hands_rig <hud section>: load another hands rig under whatever is in the hands, which is
// what an outfit change and the 3D PDA both do. The rigs do not carry the same motion sets, and
// an item left holding ids from the previous one crashed the game between two frames of the
// swap; this is the one call that mechanism needs, without a PDA, an outfit or a save.
class CCC_QaHandsRig : public IConsole_Command
{
public:
    CCC_QaHandsRig(pcstr name) : IConsole_Command(name) { bEmptyArgsHandled = true; }
    void Execute(pcstr args) override
    {
        if (!g_player_hud)
            return;
        if (!args || !xr_strlen(args))
        {
            Msg("* [qa] hands rig [%s]", g_player_hud->section_name().c_str());
            return;
        }
        const shared_str sect(args);
        if (!pSettings->section_exist(sect))
        {
            Msg("! [qa] no hud section [%s]", args);
            return;
        }
        g_player_hud->load(sect);
        Msg("* [qa] hands rig -> [%s]", args);
    }
};

// qa_hud_motion <hud section> <alias>: how long that item's cycle is on the rig that is loaded
// now, through the same pooled hud item the game uses. The pool is keyed by section and outlives
// a rig change, so this is also the shortest way to ask whether an item still has its cycles
// after one - zero cycles and a length of zero is an item bound to a rig that is gone.
class CCC_QaHudMotion : public IConsole_Command
{
public:
    CCC_QaHudMotion(pcstr name) : IConsole_Command(name) { bEmptyArgsHandled = true; }
    void Execute(pcstr args) override
    {
        string256 sect, alias;
        if (!g_player_hud || !args || sscanf(args, "%s %s", sect, alias) != 2)
            return;
        if (!pSettings->section_exist(sect))
        {
            Msg("! [qa] no hud section [%s]", sect);
            return;
        }
        const CMotionDef* md = nullptr;
        // anim alias first, hud section second - the order player_hud takes them in.
        const u32 ms = g_player_hud->motion_length(shared_str(alias), shared_str(sect), md);
        Msg("* [qa] hud motion [%s] of [%s] on rig [%s]: %u ms, def %s", alias, sect,
            g_player_hud->section_name().c_str(), ms, md ? "yes" : "none");
    }
};

// qa_visor_state: what the visor effect is being driven with, which no screenshot shows.
class CCC_QaVisorState : public IConsole_Command
{
public:
    CCC_QaVisorState(pcstr name) : IConsole_Command(name) { bEmptyArgsHandled = true; }
    void Execute(pcstr) override
    {
        if (!g_pGamePersistent)
            return;
        const CEnvironment& env = GamePersistent().Environment();
        Msg("* [qa] visor: wipe phase %.2f, direction %d, sweep %.2f s", env.visor_wipe_phase(),
            env.visor.wipe_dir, env.visor.wipe_len);
        Msg("* [qa] visor: rain %.1f mm/h at the eye, visor_rate %.3f (negative = not driven yet), "
            "exposure %.3f = density %.3f x cover %.3f, lens %.1f mm",
            env.rain_rate_mmh, env.visor.rate, env.GetRainExposure(), env.CurrentEnv.rain_density,
            env.GetRainCover(), env.visor.qa_blob);
        // The cover, ray by ray: open, or blocked this far up by this material. When the visor
        // dries in the open this is the line that says what the rain thinks is overhead.
        if (const CEffect_Rain* rain = env.eff_Rain)
        {
            string1024 rays;
            rays[0] = 0;
            for (int i = 0; i < da_rain::cover_rays; ++i)
            {
                const CEffect_Rain::CoverRay r = rain->GetCoverRay(i);
                string256 one;
                if (r.open)
                    xr_sprintf(one, " %d:open", i);
                else
                {
                    const SGameMtl* m = (r.mtl >= 0 && r.mtl < s32(GMLib.CountMaterial()))
                        ? GMLib.GetMaterialByIdx(u16(r.mtl))
                        : nullptr;
                    xr_sprintf(one, " %d:%.1fm[%s]", i, r.range, m ? m->m_Name.c_str() : "?");
                }
                xr_strcat(rays, one);
            }
            Msg("* [qa] visor: cover rays lean %.0f deg toward heading %.0f deg (wind %.2f gust %.2f):%s",
                rad2deg(rain->GetCoverLean()), rad2deg(rain->GetCoverHeading()), env.eff_wind_norm,
                env.eff_wind_gust, rays);
        }
    }
};

// qa_water_ring [radius]: one impact ring at the actor's feet. Feeds whatever ripple path is
// live - the eight analytic slots below the tier that runs the field, the simulation above it.
class CCC_QaWaterRing : public IConsole_Command
{
public:
    CCC_QaWaterRing(pcstr name) : IConsole_Command(name) { bEmptyArgsHandled = true; }
    void Execute(pcstr args) override
    {
        CActor* actor = g_pGameLevel ? smart_cast<CActor*>(Level().CurrentEntity()) : nullptr;
        if (!actor)
            return;
        float radius = 1.6f;
        if (args && xr_strlen(args))
            sscanf(args, "%f", &radius);

        CEnvironment& env = GamePersistent().Environment();
        Fvector p = actor->Position();
        const float surface = env.water_surface_at(p);
        if (surface <= -flt_max * 0.5f)
        {
            Msg("! [qa] qa_water_ring: no water under the actor");
            return;
        }
        p.y = surface;
        env.water_hit(p, radius, CEnvironment::EWaterHit::ring);
        Msg("* [qa] ring r=%.2f at (%.1f, %.1f, %.1f)", radius, p.x, p.y, p.z);
    }
};

// qa_rain_shelter [radius]: the actor moved to the nearest spot with a roof over it, using the
// same sky ray the rain spawner uses to decide a drop may exist. "Does it still rain indoors" has
// no repro otherwise - there is no other way to be sure the camera ended up under something.
class CCC_QaRainShelter : public IConsole_Command
{
public:
    CCC_QaRainShelter(pcstr name) : IConsole_Command(name) { bEmptyArgsHandled = true; }
    void Execute(pcstr args) override
    {
        CActor* actor = g_pGameLevel ? smart_cast<CActor*>(Level().CurrentEntity()) : nullptr;
        if (!actor)
        {
            Msg("! [qa] qa_rain_shelter: no actor");
            return;
        }
        float reach = 60.f;
        if (args && xr_strlen(args))
            sscanf(args, "%f", &reach);

        const CEnvironment& env = GamePersistent().Environment();
        const Fvector from = actor->Position();
        auto& space = Level().ObjectSpace;

        // Rings outward from where the actor stands, so the answer is the CLOSEST shelter and the
        // frame still shows the world the player was just looking at.
        Fvector best{};
        float best_d = -1.f;
        for (float r = 2.f; r <= reach && best_d < 0.f; r += 2.f)
        {
            for (int a = 0; a < 24; ++a)
            {
                const float ang = PI_MUL_2 * float(a) / 24.f;
                Fvector p = from;
                p.x += _cos(ang) * r;
                p.z += _sin(ang) * r;

                // Find the ground there before asking what is over it.
                Fvector top = p;
                top.y += 20.f;
                collide::rq_result ground;
                if (!space.RayPick(top, Fvector{ 0.f, -1.f, 0.f }, 60.f, collide::rqtStatic, ground, nullptr))
                    continue;
                p.y = top.y - ground.range + 0.1f;

                if (env.wind_sheltered(p))
                {
                    best = p;
                    best_d = r;
                    break;
                }
            }
        }

        if (best_d < 0.f)
        {
            Msg("! [qa] qa_rain_shelter: nothing with a roof within %.0f m", reach);
            return;
        }
        actor->MoveActor(best, Fvector{ 0.15f, 0.f, 0.f });
        Msg("* [qa] shelter %.0f m away at (%.1f, %.1f, %.1f)", best_d, best.x, best.y, best.z);
    }
};

void CCC_RegisterCommands()
{
    ZoneScoped;

    // options
    g_OptConCom.Init();

    CMD1(CCC_MemStats, "stat_memory");
    CMD1(CCC_SessionReport, "session_report");

    // game
    CMD3(CCC_Mask, "g_crouch_toggle", &psActorFlags, AF_CROUCH_TOGGLE);
    CMD1(CCC_GameDifficulty, "g_game_difficulty");
    CMD1(CCC_GameLanguage, "g_language");
    CMD3(CCC_String, "g_language_ltx", CStringTable::LanguageIDInLTX, std::size(CStringTable::LanguageIDInLTX));

    CMD3(CCC_Mask, "g_backrun", &psActorFlags, AF_RUN_BACKWARD);

    CMD3(CCC_Mask, "g_multi_item_pickup", &psActorFlags, AF_MULTI_ITEM_PICKUP);

    // The major-release notice, as a plain 0/1 so the Game options checkbox can bind to it by
    // name (CConsole::GetBool accepts CCC_Integer) and user.ltx keeps it across launches.
    // Updates within the installed major line are not gated on anything.
    CMD4(CCC_Integer, "dar_major_update_notice", &g_dar_major_update_notice, 0, 1);
    CMD4(CCC_Integer, "dar_instant_inventory", &g_dar_instant_inventory, 0, 1);
    CMD4(CCC_Integer, "dar_rain_douses_fires", &g_dar_rain_douses_fires, 0, 1);

    // alife
#ifdef DEBUG
    CMD1(CCC_ALifePath, "al_path"); // build path

#endif // DEBUG

    CMD2(CCC_ALifeSave, "save", true); // save game
    CMD2(CCC_ALifeSave, "save_silent", false); // save game without UI messages
    CMD1(CCC_ALifeLoadFrom, "load"); // load game from ...
    CMD1(CCC_LoadLastSave, "load_last_save"); // load last saved game from ...
    CMD2(CCC_CameraRotate, "cam_yaw_rotate", &ConfigureActorCameraYawRotation);
    CMD2(CCC_CameraRotate, "cam_pitch_rotate", &ConfigureActorCameraPitchRotation);

    CMD1(CCC_ContentVerify, "dar_content_verify");
    CMD1(CCC_ContentRepair, "dar_content_repair");
    CMD1(CCC_ContentState, "dar_content_state");

    CMD1(CCC_FlushLog, "flush"); // flush log
    CMD1(CCC_ClearLog, "clear_log");

    // XMS module system
    CMD1(CCC_XmsList, "xms_list");
    CMD2(CCC_XmsEnable, "xms_enable", true);
    CMD2(CCC_XmsEnable, "xms_disable", false);
    CMD1(CCC_XmsConflicts, "xms_conflicts");
    CMD1(CCC_XmsUpdate, "xms_update");
    CMD1(CCC_XmsUpdateStatus, "xms_update_status");
    CMD1(CCC_XmsMods, "xms_mods");
    CMD1(CCC_XmsModes, "xms_modes");
    CMD1(CCC_XmsWhy, "xms_why");
    CMD1(CCC_XmsNq, "nq");

    // collision overlays resolve game materials by name through this
    XMS::SetMaterialResolver([](pcstr name) -> u16
    {
        SGameMtl* material = GMLib.GetMaterial(name);
        return material ? u16(material->GetID()) : u16(-1);
    });

#ifndef MASTER_GOLD
    CMD1(CCC_ALifeTimeFactor, "al_time_factor"); // set time factor
    CMD1(CCC_ALifeSwitchDistance, "al_switch_distance"); // set switch distance
    CMD1(CCC_ALifeProcessTime, "al_process_time"); // set process time
    CMD1(CCC_ALifeObjectsPerUpdate, "al_objects_per_update"); // set process time
    CMD1(CCC_ALifeSwitchFactor, "al_switch_factor"); // set switch factor
#endif // #ifndef MASTER_GOLD

    CMD3(CCC_Mask, "hud_weapon", &psHUD_Flags, HUD_WEAPON);
    CMD3(CCC_Mask, "hud_info", &psHUD_Flags, HUD_INFO);
    CMD3(CCC_Mask, "hud_draw", &psHUD_Flags, HUD_DRAW);

    // hud
    psHUD_Flags.set(HUD_CROSSHAIR, true);
    psHUD_Flags.set(HUD_WEAPON, true);
    psHUD_Flags.set(HUD_DRAW, true);
    psHUD_Flags.set(HUD_INFO, true);
    psHUD_Flags.set(HUD_DRAW_MAP, true);
    psHUD_Flags.set(HUD_DRAW_INFO, true);
    psHUD_Flags.set(HUD_CROSSHAIR_ITEM, false);
    psHUD_Flags.set(HUD_CROSSHAIR_WEAPON, true);
    psHUD_Flags.set(HUD_CROSSHAIR_NEAREST, true);

    CMD3(CCC_Mask, "hud_crosshair", &psHUD_Flags, HUD_CROSSHAIR);
    CMD3(CCC_Mask, "hud_crosshair_dist", &psHUD_Flags, HUD_CROSSHAIR_DIST);
    CMD3(CCC_Mask, "hud_draw_map", &psHUD_Flags, HUD_DRAW_MAP);
    CMD3(CCC_Mask, "hud_draw_info", &psHUD_Flags, HUD_DRAW_INFO);
    CMD3(CCC_Mask, "hud_left_handed", &psHUD_Flags, HUD_LEFT_HANDED);
    CMD3(CCC_Mask, "hud_crosshair_item", &psHUD_Flags, HUD_CROSSHAIR_ITEM);
    CMD3(CCC_Mask, "hud_crosshair_weapon", &psHUD_Flags, HUD_CROSSHAIR_WEAPON);
    CMD3(CCC_Mask, "hud_crosshair_nearest", &psHUD_Flags, HUD_CROSSHAIR_NEAREST);
    CMD3(CCC_Mask, "hud_crosshair_dot", &psHUD_Flags, HUD_CROSSHAIR_DOT);

    CMD4(CCC_Float, "hud_fov", &psHUD_FOV_def, 0.1f, 1.0f);
    CMD4(CCC_Float, "fov", &g_fov, 5.0f, 180.0f);

    // Hand scenes (the animation module): the scene item seat tuned in game, the seat slide
    // speeds and the cycle trace. Definitions in player_hud.cpp.
    {
        extern Fvector g_hud_scene_item_pos_adj, g_hud_scene_item_rot_adj;
        extern float g_hud_scene_item_scale_adj, g_hud_scene_seat_in, g_hud_scene_seat_out;
        extern int g_hud_scene_dbg;
        CMD4(CCC_SessionVector3, "hud_scene_item_pos", &g_hud_scene_item_pos_adj, Fvector().set(-2.f, -2.f, -2.f), Fvector().set(2.f, 2.f, 2.f));
        CMD4(CCC_SessionVector3, "hud_scene_item_rot", &g_hud_scene_item_rot_adj, Fvector().set(-360.f, -360.f, -360.f), Fvector().set(360.f, 360.f, 360.f));
        CMD4(CCC_SessionFloat, "hud_scene_item_scale", &g_hud_scene_item_scale_adj, 0.05f, 20.f);
        CMD4(CCC_SessionFloat, "hud_scene_seat_in", &g_hud_scene_seat_in, 0.5f, 20.f);
        CMD4(CCC_SessionFloat, "hud_scene_seat_out", &g_hud_scene_seat_out, 0.5f, 40.f);
        CMD4(CCC_SessionInteger, "hud_scene_dbg", &g_hud_scene_dbg, 0, 1);
        CMD1(CCC_HudSceneItemDump, "hud_scene_item_dump");
    }

    // Weapon fault trace: one line per shot of the player's weapon with every accumulator, plus
    // the restore/park/clear events. Definition in Weapon.cpp.
    {
        extern int g_wpn_fault_dbg;
        CMD4(CCC_SessionInteger, "wpn_fault_dbg", &g_wpn_fault_dbg, 0, 1);
    }

    // Demo
    CMD1(CCC_DemoPlay, "demo_play");
    CMD1(CCC_DemoRecord, "demo_record");
    CMD1(CCC_DemoRecordSetPos, "demo_set_cam_position");

#ifndef MASTER_GOLD
    // ai
    CMD3(CCC_Mask, "mt_ai_vision", &g_mt_config, mtAiVision);
    CMD3(CCC_Mask, "mt_level_path", &g_mt_config, mtLevelPath);
    CMD3(CCC_Mask, "mt_detail_path", &g_mt_config, mtDetailPath);
    CMD3(CCC_Mask, "mt_object_handler", &g_mt_config, mtObjectHandler);
    CMD3(CCC_Mask, "mt_sound_player", &g_mt_config, mtSoundPlayer);
    CMD3(CCC_Mask, "mt_bullets", &g_mt_config, mtBullets);
    CMD3(CCC_Mask, "mt_script_gc", &g_mt_config, mtLUA_GC);
    CMD3(CCC_Mask, "mt_level_sounds", &g_mt_config, mtLevelSounds);
    CMD3(CCC_Mask, "mt_alife", &g_mt_config, mtALife);
    CMD3(CCC_Mask, "mt_map", &g_mt_config, mtMap);
#endif // MASTER_GOLD

#ifndef MASTER_GOLD
    CMD3(CCC_Mask, "ai_obstacles_avoiding", &psAI_Flags, aiObstaclesAvoiding);
    CMD3(CCC_Mask, "ai_obstacles_avoiding_static", &psAI_Flags, aiObstaclesAvoidingStatic);
    CMD3(CCC_Mask, "ai_use_smart_covers", &psAI_Flags, aiUseSmartCovers);
    CMD3(CCC_Mask, "ai_use_smart_covers_animation_slots", &psAI_Flags, (u32)aiUseSmartCoversAnimationSlot);
    CMD4(CCC_Float, "ai_smart_factor", &g_smart_cover_factor, 0.f, 1000000.f);
#endif // MASTER_GOLD

    CMD3(CCC_Mask, "lua_debug", &g_LuaDebug, 1);
    CMD4(CCC_Integer, "lua_dump_depth", &g_LuaDumpDepth, 0, 16);

    CMD1(CCC_LuaProfiler, CCC_LuaProfiler::COMMAND_LUA_PROFILER_STATUS);
    CMD1(CCC_LuaProfiler, CCC_LuaProfiler::COMMAND_LUA_PROFILER_START);
    CMD1(CCC_LuaProfiler, CCC_LuaProfiler::COMMAND_LUA_PROFILER_START_SAMPLING_MODE);
    CMD1(CCC_LuaProfiler, CCC_LuaProfiler::COMMAND_LUA_PROFILER_START_HOOK_MODE);
    CMD1(CCC_LuaProfiler, CCC_LuaProfiler::COMMAND_LUA_PROFILER_STOP);
    CMD1(CCC_LuaProfiler, CCC_LuaProfiler::COMMAND_LUA_PROFILER_RESET);
    CMD1(CCC_LuaProfiler, CCC_LuaProfiler::COMMAND_LUA_PROFILER_LOG);
    CMD1(CCC_LuaProfiler, CCC_LuaProfiler::COMMAND_LUA_PROFILER_SAVE);

    CMD1(CCC_LuaGCMethod, "lua_gc_method");
    CMD1(CCC_FallRollTest, "fall_roll_test");
    CMD4(CCC_Integer, "lua_gcstep", &psLUA_GCSTEP, 1, 1000);
    CMD4(CCC_Integer, "lua_gc_timeout", &psLUA_GCTIMEOUT, 1000, 16000);

#ifdef DEBUG
    CMD3(CCC_Mask, "ai_debug", &psAI_Flags, aiDebug);
    CMD3(CCC_Mask, "ai_dbg_brain", &psAI_Flags, aiBrain);
    CMD3(CCC_Mask, "ai_dbg_motion", &psAI_Flags, aiMotion);
    CMD3(CCC_Mask, "ai_dbg_frustum", &psAI_Flags, aiFrustum);
    CMD3(CCC_Mask, "ai_dbg_funcs", &psAI_Flags, aiFuncs);
    CMD3(CCC_Mask, "ai_dbg_alife", &psAI_Flags, aiALife);
    CMD3(CCC_Mask, "ai_dbg_goap", &psAI_Flags, aiGOAP);
    CMD3(CCC_Mask, "ai_dbg_goap_script", &psAI_Flags, aiGOAPScript);
    CMD3(CCC_Mask, "ai_dbg_goap_object", &psAI_Flags, aiGOAPObject);
    CMD3(CCC_Mask, "ai_dbg_cover", &psAI_Flags, aiCover);
    CMD3(CCC_Mask, "ai_dbg_anim", &psAI_Flags, aiAnimation);
    CMD3(CCC_Mask, "ai_dbg_vision", &psAI_Flags, aiVision);
    CMD3(CCC_Mask, "ai_dbg_monster", &psAI_Flags, aiMonsterDebug);
    CMD3(CCC_Mask, "ai_dbg_stalker", &psAI_Flags, aiStalker);
    CMD3(CCC_Mask, "ai_stats", &psAI_Flags, aiStats);
    CMD3(CCC_Mask, "ai_dbg_destroy", &psAI_Flags, aiDestroy);
    CMD3(CCC_Mask, "ai_dbg_serialize", &psAI_Flags, aiSerialize);
    CMD3(CCC_Mask, "ai_dbg_dialogs", &psAI_Flags, aiDialogs);
    CMD3(CCC_Mask, "ai_dbg_infoportion", &psAI_Flags, aiInfoPortion);

    CMD3(CCC_Mask, "ai_draw_game_graph", &psAI_Flags, aiDrawGameGraph);
    CMD3(CCC_Mask, "ai_draw_game_graph_stalkers", &psAI_Flags, aiDrawGameGraphStalkers);
    CMD3(CCC_Mask, "ai_draw_game_graph_objects", &psAI_Flags, aiDrawGameGraphObjects);
    CMD3(CCC_Mask, "ai_draw_game_graph_real_pos", &psAI_Flags, aiDrawGameGraphRealPos);

    // XXX: register from script engine
    // CMD3(CCC_Mask,				"lua_nil_object_access",	&psAI_Flags,	aiNilObjectAccess);

    CMD3(CCC_Mask, "ai_draw_visibility_rays", &psAI_Flags, aiDrawVisibilityRays);
    CMD3(CCC_Mask, "ai_animation_stats", &psAI_Flags, aiAnimationStats);

    /////////////////////////////////////////////HIT ANIMATION////////////////////////////////////////////////////
    // float						power_factor				= 2.f;
    // float						rotational_power_factor		= 3.f;
    // float						side_sensitivity_threshold	= 0.2f;
    // float						anim_channel_factor			= 3.f;

    CMD4(CCC_Float, "hit_anims_power", &ghit_anims_params.power_factor, 0.0f, 100.0f);
    CMD4(CCC_Float, "hit_anims_rotational_power", &ghit_anims_params.rotational_power_factor, 0.0f, 100.0f);
    CMD4(CCC_Float, "hit_anims_side_sensitivity_threshold", &ghit_anims_params.side_sensitivity_threshold, 0.0f, 10.0f);
    CMD4(CCC_Float, "hit_anims_channel_factor", &ghit_anims_params.anim_channel_factor, 0.0f, 100.0f);
    // float	block_blend					= 0.1f;
    // float	reduce_blend				= 0.5f;
    // float	reduce_power_factor			= 0.5f;
    CMD4(CCC_Float, "hit_anims_block_blend", &ghit_anims_params.block_blend, 0.f, 1.f);
    CMD4(CCC_Float, "hit_anims_reduce_blend", &ghit_anims_params.reduce_blend, 0.f, 1.f);
    CMD4(CCC_Float, "hit_anims_reduce_blend_factor", &ghit_anims_params.reduce_power_factor, 0.0f, 1.0f);
    CMD4(CCC_Integer, "hit_anims_tune", &tune_hit_anims, 0, 1);
/////////////////////////////////////////////HIT ANIMATION END////////////////////////////////////////////////////

    CMD1(CCC_DumpModelBones, "debug_dump_model_bones");

    CMD1(CCC_DrawGameGraphAll, "ai_draw_game_graph_all");
    CMD1(CCC_DrawGameGraphCurrent, "ai_draw_game_graph_current_level");
    CMD1(CCC_DrawGameGraphLevel, "ai_draw_game_graph_level");

    CMD4(CCC_Integer, "ai_dbg_inactive_time", &g_AI_inactive_time, 0, 1000000);

    CMD1(CCC_DebugNode, "ai_dbg_node");
#if defined(USE_DEBUGGER)
    CMD1(CCC_ScriptDbg, "script_debug_break");
    CMD1(CCC_ScriptDbg, "script_debug_stop");
    CMD1(CCC_ScriptDbg, "script_debug_restart");
#endif // #if defined(USE_DEBUGGER)

    CMD1(CCC_ShowMonsterInfo, "ai_monster_info");
    CMD1(CCC_DebugFonts, "debug_fonts");
    CMD1(CCC_TuneAttachableItem, "dbg_adjust_attachable_item");

    CMD1(CCC_ShowAnimationStats, "ai_show_animation_stats");
#endif // DEBUG

#ifndef MASTER_GOLD
    CMD3(CCC_Mask, "ai_ignore_actor", &psAI_Flags, aiIgnoreActor);
#endif // MASTER_GOLD

    CMD4(CCC_Integer, "ai_dead_vision_ms", &g_ai_dead_vision_ms, 0, 10000);

    // Physics
    CMD4(CCC_Float, "ph_ik_dist", &g_ph_ik_dist, 0.f, 300.f);
    CMD1(CCC_PHFps, "ph_frequency");
    CMD1(CCC_PHIterations, "ph_iterations");

#ifdef DEBUG
    CMD1(CCC_PHGravity, "ph_gravity");
    CMD4(CCC_FloatBlock, "ph_timefactor", &phTimefactor, 0.000001f, 1000.f);
    CMD4(CCC_FloatBlock, "ph_break_common_factor", &ph_console::phBreakCommonFactor, 0.f, 1000000000.f);
    CMD4(CCC_FloatBlock, "ph_rigid_break_weapon_factor", &ph_console::phRigidBreakWeaponFactor, 0.f, 1000000000.f);
    CMD4(CCC_Integer, "ph_tri_clear_disable_count", &ph_console::ph_tri_clear_disable_count, 0, 255);
    CMD4(CCC_FloatBlock, "ph_tri_query_ex_aabb_rate", &ph_console::ph_tri_query_ex_aabb_rate, 1.01f, 3.f);
#endif // DEBUG

#ifndef MASTER_GOLD
    CMD1(CCC_JumpToLevel, "jump_to_level");
    CMD3(CCC_Mask, "g_god", &psActorFlags, AF_GODMODE);
    CMD1(CCC_ToggleNoClip, "g_no_clip");
    CMD3(CCC_Mask, "g_unlimitedammo", &psActorFlags, AF_UNLIMITEDAMMO);
    CMD1(CCC_Spawn, "g_spawn");
    CMD1(CCC_SpawnToInventory, "g_spawn_to_inventory");
    CMD1(CCC_Script, "run_script");
    CMD1(CCC_ScriptCommand, "run_string");
    CMD1(CCC_TimeFactor, "time_factor");
#endif // MASTER_GOLD

    CMD3(CCC_Mask, "g_autopickup", &psActorFlags, AF_AUTOPICKUP);
    CMD3(CCC_Mask, "g_dynamic_music", &psActorFlags, AF_DYNAMIC_MUSIC);
    CMD3(CCC_Mask, "g_important_save", &psActorFlags, AF_IMPORTANT_SAVE);
    CMD3(CCC_Mask, "g_loading_stages", &psActorFlags, AF_LOADING_STAGES);
    CMD3(CCC_Mask, "g_always_use_attitude_sensors", &psActorFlags, AF_ALWAYS_USE_ATTITUDE_SENSORS);
    CMD3(CCC_Mask, "g_use_tracers", &psActorFlags, AF_USE_TRACERS);
    CMD3(CCC_Mask, "g_open_scopes", &psActorFlags, AF_OPEN_SCOPES);
    // Keep the legacy finite command and the positive UI option synchronized.
    CMD1(CCC_FiniteBolts, "g_finite_bolts");
    CMD1(CCC_InfiniteBolts, "g_infinite_bolts");

    // 3D PDA pipeline debugging: 1 = force the RT path with no presenter, 2 = also blit
    // $user$ui fullscreen for a pixel-for-pixel look at what the screen material samples.
    {
        extern int g_pda3d_dbg;
        CMD4(CCC_Integer, "pda3d_dbg", &g_pda3d_dbg, 0, 2);
    }
    CMD4(CCC_Integer, "g_inv_highlight_equipped", &g_inv_highlight_equipped, 0, 1);
    CMD4(CCC_Integer, "g_first_person_death", &g_first_person_death, 0, 1);
    CMD4(CCC_Integer, "g_unload_ammo_after_pick_up", &g_auto_ammo_unload, 0, 1);
    CMD4(CCC_Integer, "g_normalize_mouse_sens", &g_normalize_mouse_sens, 0, 1);
    CMD4(CCC_Integer, "g_normalize_upgrade_mouse_sens", &g_normalize_upgrade_mouse_sens, 0, 1);

    CMD4(CCC_Float, "g_look_intensity_min", &psLookIntensityMin, 10.f, 100.f);
    CMD4(CCC_Float, "g_look_intensity_max", &psLookIntensityMax, 10.f, 100.f);
    CMD4(CCC_Float, "g_look_intensity_step", &psLookIntensityStep, 0.f, 10.f);

    CMD4(CCC_Float, "g_cursor_intensity_min", &psCursorIntensityMin, 1.f, 100.f);
    CMD4(CCC_Float, "g_cursor_intensity_max", &psCursorIntensityMax, 1.f, 100.f);
    CMD4(CCC_Float, "g_cursor_intensity_step", &psCursorIntensityStep, 0.f, 10.f);

    CMD1(CCC_CleanupTasks, "dbg_cleanup_tasks");

#ifdef DEBUG
    CMD1(CCC_ShowSmartCastStats, "show_smart_cast_stats");
    CMD1(CCC_ClearSmartCastStats, "clear_smart_cast_stats");

    CMD3(CCC_Mask, "dbg_draw_actor_alive", &dbg_net_Draw_Flags, dbg_draw_actor_alive);
    CMD3(CCC_Mask, "dbg_draw_actor_dead", &dbg_net_Draw_Flags, dbg_draw_actor_dead);
    CMD3(CCC_Mask, "dbg_draw_customzone", &dbg_net_Draw_Flags, dbg_draw_customzone);
    CMD3(CCC_Mask, "dbg_draw_teamzone", &dbg_net_Draw_Flags, dbg_draw_teamzone);
    CMD3(CCC_Mask, "dbg_draw_invitem", &dbg_net_Draw_Flags, dbg_draw_invitem);
    CMD3(CCC_Mask, "dbg_draw_actor_phys", &dbg_net_Draw_Flags, dbg_draw_actor_phys);
    CMD3(CCC_Mask, "dbg_draw_customdetector", &dbg_net_Draw_Flags, dbg_draw_customdetector);
    CMD3(CCC_Mask, "dbg_destroy", &dbg_net_Draw_Flags, dbg_destroy);
    CMD3(CCC_Mask, "dbg_draw_autopickupbox", &dbg_net_Draw_Flags, dbg_draw_autopickupbox);
    CMD3(CCC_Mask, "dbg_draw_rp", &dbg_net_Draw_Flags, dbg_draw_rp);
    CMD3(CCC_Mask, "dbg_draw_climbable", &dbg_net_Draw_Flags, dbg_draw_climbable);
    CMD3(CCC_Mask, "dbg_draw_skeleton", &dbg_net_Draw_Flags, dbg_draw_skeleton);

    CMD3(CCC_Mask, "dbg_draw_ph_contacts", &ph_dbg_draw_mask, phDbgDrawContacts);
    CMD3(CCC_Mask, "dbg_draw_ph_enabled_aabbs", &ph_dbg_draw_mask, phDbgDrawEnabledAABBS);
    CMD3(CCC_Mask, "dbg_draw_ph_intersected_tries", &ph_dbg_draw_mask, phDBgDrawIntersectedTries);
    CMD3(CCC_Mask, "dbg_draw_ph_saved_tries", &ph_dbg_draw_mask, phDbgDrawSavedTries);
    CMD3(CCC_Mask, "dbg_draw_ph_tri_trace", &ph_dbg_draw_mask, phDbgDrawTriTrace);
    CMD3(CCC_Mask, "dbg_draw_ph_positive_tries", &ph_dbg_draw_mask, phDBgDrawPositiveTries);
    CMD3(CCC_Mask, "dbg_draw_ph_negative_tries", &ph_dbg_draw_mask, phDBgDrawNegativeTries);
    CMD3(CCC_Mask, "dbg_draw_ph_tri_test_aabb", &ph_dbg_draw_mask, phDbgDrawTriTestAABB);
    CMD3(CCC_Mask, "dbg_draw_ph_tries_changes_sign", &ph_dbg_draw_mask, phDBgDrawTriesChangesSign);
    CMD3(CCC_Mask, "dbg_draw_ph_tri_point", &ph_dbg_draw_mask, phDbgDrawTriPoint);
    CMD3(CCC_Mask, "dbg_draw_ph_explosion_position", &ph_dbg_draw_mask, phDbgDrawExplosionPos);
    CMD3(CCC_Mask, "dbg_draw_ph_statistics", &ph_dbg_draw_mask, phDbgDrawObjectStatistics);
    CMD3(CCC_Mask, "dbg_draw_ph_mass_centres", &ph_dbg_draw_mask, phDbgDrawMassCenters);
    CMD3(CCC_Mask, "dbg_draw_ph_death_boxes", &ph_dbg_draw_mask, phDbgDrawDeathActivationBox);
    CMD3(CCC_Mask, "dbg_draw_ph_hit_app_pos", &ph_dbg_draw_mask, phHitApplicationPoints);
    CMD3(CCC_Mask, "dbg_draw_ph_cashed_tries_stats", &ph_dbg_draw_mask, phDbgDrawCashedTriesStat);
    CMD3(CCC_Mask, "dbg_draw_ph_car_dynamics", &ph_dbg_draw_mask, phDbgDrawCarDynamics);
    CMD3(CCC_Mask, "dbg_draw_ph_car_plots", &ph_dbg_draw_mask, phDbgDrawCarPlots);
    CMD3(CCC_Mask, "dbg_ph_ladder", &ph_dbg_draw_mask, phDbgLadder);
    CMD3(CCC_Mask, "dbg_draw_ph_explosions", &ph_dbg_draw_mask, phDbgDrawExplosions);
    CMD3(CCC_Mask, "dbg_draw_car_plots_all_trans", &ph_dbg_draw_mask, phDbgDrawCarAllTrnsm);
    CMD3(CCC_Mask, "dbg_draw_ph_zbuffer_disable", &ph_dbg_draw_mask, phDbgDrawZDisable);
    CMD3(CCC_Mask, "dbg_ph_obj_collision_damage", &ph_dbg_draw_mask, phDbgDispObjCollisionDammage);
    CMD_RADIOGROUPMASK2("dbg_ph_ai_always_phmove", &ph_dbg_draw_mask, phDbgAlwaysUseAiPhMove, "dbg_ph_ai_never_phmove",
        &ph_dbg_draw_mask, phDbgNeverUseAiPhMove);
    CMD3(CCC_Mask, "dbg_ph_ik", &ph_dbg_draw_mask, phDbgIK);
    CMD3(CCC_Mask, "dbg_ph_ik_off", &ph_dbg_draw_mask1, phDbgIKOff);
    CMD3(CCC_Mask, "dbg_draw_ph_ik_goal", &ph_dbg_draw_mask, phDbgDrawIKGoal);
    CMD3(CCC_Mask, "dbg_ph_ik_limits", &ph_dbg_draw_mask, phDbgIKLimits);
    CMD3(CCC_Mask, "dbg_ph_character_control", &ph_dbg_draw_mask, phDbgCharacterControl);
    CMD3(CCC_Mask, "dbg_draw_ph_ray_motions", &ph_dbg_draw_mask, phDbgDrawRayMotions);
    CMD4(CCC_Float, "dbg_ph_vel_collid_damage_to_display", &dbg_vel_collid_damage_to_display, 0.f, 1000.f);
    CMD4(CCC_DbgBullets, "dbg_draw_bullet_hit", &g_bDrawBulletHit, 0, 1);
    CMD4(CCC_Integer, "dbg_draw_fb_crosshair", &g_bDrawFirstBulletCrosshair, 0, 1);
    CMD1(CCC_DbgPhTrackObj, "dbg_track_obj");
    CMD3(CCC_Mask, "dbg_ph_actor_restriction", &ph_dbg_draw_mask1, ph_m1_DbgActorRestriction);
    CMD3(CCC_Mask, "dbg_draw_ph_hit_anims", &ph_dbg_draw_mask1, phDbgHitAnims);
    CMD3(CCC_Mask, "dbg_draw_ph_ik_limits", &ph_dbg_draw_mask1, phDbgDrawIKLimits);
    CMD3(CCC_Mask, "dbg_draw_ph_ik_predict", &ph_dbg_draw_mask1, phDbgDrawIKPredict);
    CMD3(CCC_Mask, "dbg_draw_ph_ik_collision", &ph_dbg_draw_mask1, phDbgDrawIKCollision);
    CMD3(CCC_Mask, "dbg_draw_ph_ik_shift_object", &ph_dbg_draw_mask1, phDbgDrawIKSHiftObject);
    CMD3(CCC_Mask, "dbg_draw_ph_ik_blending", &ph_dbg_draw_mask1, phDbgDrawIKBlending);
    CMD1(CCC_DBGDrawCashedClear, "dbg_ph_cashed_clear");
    extern BOOL dbg_draw_character_bones;
    extern BOOL dbg_draw_character_physics;
    extern BOOL dbg_draw_character_binds;
    extern BOOL dbg_draw_character_physics_pones;
    extern BOOL ik_cam_shift;
    CMD4(CCC_Integer, "dbg_draw_character_bones", &dbg_draw_character_bones, FALSE, TRUE);
    CMD4(CCC_Integer, "dbg_draw_character_physics", &dbg_draw_character_physics, FALSE, TRUE);
    CMD4(CCC_Integer, "dbg_draw_character_binds", &dbg_draw_character_binds, FALSE, TRUE);
    CMD4(CCC_Integer, "dbg_draw_character_physics_pones", &dbg_draw_character_physics_pones, FALSE, TRUE);

    CMD4(CCC_Integer, "ik_cam_shift", &ik_cam_shift, FALSE, TRUE);

    extern float ik_cam_shift_tolerance;
    CMD4(CCC_Float, "ik_cam_shift_tolerance", &ik_cam_shift_tolerance, 0.f, 2.f);
    extern float ik_cam_shift_speed;
    CMD4(CCC_Float, "ik_cam_shift_speed", &ik_cam_shift_speed, 0.f, 1.f);
    extern float ik_cam_shift_interpolation;
    CMD4(CCC_Float, "ik_cam_shift_interpolation", &ik_cam_shift_interpolation, 1.f, 10.f);
    extern BOOL dbg_draw_doors;
    CMD4(CCC_Integer, "dbg_draw_doors", &dbg_draw_doors, FALSE, TRUE);

    /*
    extern int ik_allign_free_foot;
    extern int ik_local_blending;
    extern int ik_blend_free_foot;
    extern int ik_collide_blend;
        CMD4(CCC_Integer,	"ik_allign_free_foot"			,&ik_allign_free_foot,	0,	1);
        CMD4(CCC_Integer,	"ik_local_blending"				,&ik_local_blending,	0,	1);
        CMD4(CCC_Integer,	"ik_blend_free_foot"			,&ik_blend_free_foot,	0,	1);
        CMD4(CCC_Integer,	"ik_collide_blend"				,&ik_collide_blend,	0,	1);
    */
    extern BOOL dbg_draw_ragdoll_spawn;
    CMD4(CCC_Integer, "dbg_draw_ragdoll_spawn", &dbg_draw_ragdoll_spawn, FALSE, TRUE);
    extern BOOL debug_step_info;
    extern BOOL debug_step_info_load;
    CMD4(CCC_Integer, "debug_step_info", &debug_step_info, FALSE, TRUE);
    CMD4(CCC_Integer, "debug_step_info_load", &debug_step_info_load, FALSE, TRUE);
    extern BOOL debug_character_material_load;
    CMD4(CCC_Integer, "debug_character_material_load", &debug_character_material_load, FALSE, TRUE);
    extern XRPHYSICS_API BOOL dbg_draw_camera_collision;
    CMD4(CCC_Integer, "dbg_draw_camera_collision", &dbg_draw_camera_collision, FALSE, TRUE);
    extern XRPHYSICS_API float camera_collision_character_skin_depth;
    extern XRPHYSICS_API float camera_collision_character_shift_z;
    CMD4(CCC_FloatBlock, "camera_collision_character_shift_z", &camera_collision_character_shift_z, 0.f, 1.f);
    CMD4(CCC_FloatBlock, "camera_collision_character_skin_depth", &camera_collision_character_skin_depth, 0.f, 1.f);
    extern BOOL dbg_draw_animation_movement_controller;
    CMD4(CCC_Integer, "dbg_draw_animation_movement_controller", &dbg_draw_animation_movement_controller, FALSE, TRUE);

    /*
    enum
    {
        dbg_track_obj_blends_bp_0			= 1<< 0,
        dbg_track_obj_blends_bp_1			= 1<< 1,
        dbg_track_obj_blends_bp_2			= 1<< 2,
        dbg_track_obj_blends_bp_3			= 1<< 3,
        dbg_track_obj_blends_motion_name	= 1<< 4,
        dbg_track_obj_blends_time			= 1<< 5,
        dbg_track_obj_blends_ammount		= 1<< 6,
        dbg_track_obj_blends_mix_params		= 1<< 7,
        dbg_track_obj_blends_flags			= 1<< 8,
        dbg_track_obj_blends_state			= 1<< 9,
        dbg_track_obj_blends_dump			= 1<< 10
    };
    */
    extern Flags32 dbg_track_obj_flags;
    CMD3(CCC_Mask, "dbg_track_obj_blends_bp_0", &dbg_track_obj_flags, dbg_track_obj_blends_bp_0);
    CMD3(CCC_Mask, "dbg_track_obj_blends_bp_1", &dbg_track_obj_flags, dbg_track_obj_blends_bp_1);
    CMD3(CCC_Mask, "dbg_track_obj_blends_bp_2", &dbg_track_obj_flags, dbg_track_obj_blends_bp_2);
    CMD3(CCC_Mask, "dbg_track_obj_blends_bp_3", &dbg_track_obj_flags, dbg_track_obj_blends_bp_3);
    CMD3(CCC_Mask, "dbg_track_obj_blends_motion_name", &dbg_track_obj_flags, dbg_track_obj_blends_motion_name);
    CMD3(CCC_Mask, "dbg_track_obj_blends_time", &dbg_track_obj_flags, dbg_track_obj_blends_time);

    CMD3(CCC_Mask, "dbg_track_obj_blends_ammount", &dbg_track_obj_flags, dbg_track_obj_blends_ammount);
    CMD3(CCC_Mask, "dbg_track_obj_blends_mix_params", &dbg_track_obj_flags, dbg_track_obj_blends_mix_params);
    CMD3(CCC_Mask, "dbg_track_obj_blends_flags", &dbg_track_obj_flags, dbg_track_obj_blends_flags);
    CMD3(CCC_Mask, "dbg_track_obj_blends_state", &dbg_track_obj_flags, dbg_track_obj_blends_state);
    CMD3(CCC_Mask, "dbg_track_obj_blends_dump", &dbg_track_obj_flags, dbg_track_obj_blends_dump);

    CMD1(CCC_DbgVar, "dbg_var");

    extern float dbg_text_height_scale;
    CMD4(CCC_FloatBlock, "dbg_text_height_scale", &dbg_text_height_scale, 0.2f, 5.f);
#endif

#ifdef DEBUG
    CMD1(CCC_DumpInfos, "dump_infos");
    CMD1(CCC_DumpTasks, "dump_tasks");
    CMD1(CCC_DumpMap, "dump_map");
    CMD1(CCC_DumpCreatures, "dump_creatures");

#endif

    CMD3(CCC_Mask, "cl_dynamiccrosshair", &psHUD_Flags, HUD_CROSSHAIR_DYNAMIC);
    CMD1(CCC_MainMenu, "main_menu");

#ifndef MASTER_GOLD
    CMD1(CCC_StartTimeSingle, "start_time_single");
    CMD4(CCC_TimeFactorSingle, "time_factor_single", &g_fTimeFactor, 0.f, 1000.0f);
#endif // MASTER_GOLD

    g_uCommonFlags.zero();
    g_uCommonFlags.set(flAiUseTorchDynamicLights, TRUE);

    CMD3(CCC_Mask, "ai_use_torch_dynamic_lights", &g_uCommonFlags, flAiUseTorchDynamicLights);
    {
        // NPC torch shadow (the crawling black wedge at night) - see Torch.cpp.
        extern int ps_r__npc_torch_shadow;
        CMD4(CCC_Integer, "r__npc_torch_shadow", &ps_r__npc_torch_shadow, 0, 1);
    }

#ifndef MASTER_GOLD
    CMD4(CCC_Vector3, "psp_cam_offset", &CCameraLook2::m_cam_offset, Fvector().set(-1000, -1000, -1000),
        Fvector().set(1000, 1000, 1000));
#endif // MASTER_GOLD

    CMD1(CCC_GSCheckForUpdates, "check_for_updates");
#ifdef DEBUG
    CMD1(CCC_DumpObjects, "dump_all_objects");
    CMD3(CCC_String, "stalker_death_anim", dbg_stalker_death_anim, 32);
    CMD4(CCC_Integer, "death_anim_debug", &death_anim_debug, FALSE, TRUE);
    CMD4(CCC_Integer, "death_anim_velocity", &b_death_anim_velocity, FALSE, TRUE);
    CMD4(CCC_Integer, "dbg_imotion_draw_velocity", &dbg_imotion_draw_velocity, FALSE, TRUE);
    CMD4(CCC_Integer, "dbg_imotion_collide_debug", &dbg_imotion_collide_debug, FALSE, TRUE);

    CMD4(CCC_Integer, "dbg_imotion_draw_skeleton", &dbg_imotion_draw_skeleton, FALSE, TRUE);
    CMD4(CCC_Float, "dbg_imotion_draw_velocity_scale", &dbg_imotion_draw_velocity_scale, 0.0001f, 100.0f);

    CMD4(CCC_Integer, "dbg_show_ani_info", &g_ShowAnimationInfo, 0, 1);
    CMD4(CCC_Integer, "dbg_dump_physics_step", &ph_console::g_bDebugDumpPhysicsStep, 0, 1);
    CMD1(CCC_InvUpgradesHierarchy, "inv_upgrades_hierarchy");
    CMD1(CCC_InvUpgradesCurItem, "inv_upgrades_cur_item");
    CMD4(CCC_Integer, "inv_upgrades_log", &g_upgrades_log, 0, 1);
    CMD1(CCC_InvDropAllItems, "inv_drop_all_items");

    extern BOOL dbg_moving_bones_snd_player;
    CMD4(CCC_Integer, "dbg_bones_snd_player", &dbg_moving_bones_snd_player, FALSE, TRUE);
#endif
    CMD4(CCC_Float, "con_sensitive", &g_console_sensitive, 0.01f, 1.0f);
    CMD4(CCC_Integer, "wpn_aim_toggle", &b_toggle_weapon_aim, 0, 1);

    CMD1(CCC_UIStyle, "ui_style");
    CMD1(CCC_UIRestart, "ui_restart");

#ifdef DEBUG
    CMD4(CCC_Float, "ai_smart_cover_animation_speed_factor", &g_smart_cover_animation_speed_factor, .1f, 10.f);
    CMD4(CCC_Float, "air_resistance_epsilon", &air_resistance_epsilon, .0f, 1.f);
#endif // #ifdef DEBUG

    CMD4(CCC_Integer, "g_sleep_time", &psActorSleepTime, 1, 24);

    CMD4(CCC_Integer, "ai_use_old_vision", &g_ai_use_old_vision, 0, 1);

    CMD4(CCC_Integer, "ai_die_in_anomaly", &g_ai_die_in_anomaly, 0, 1); //Alundaio

    CMD4(CCC_Float, "ai_aim_predict_time", &g_aim_predict_time, 0.f, 10.f);

#ifdef DEBUG
    // extern BOOL g_use_new_ballistics;
    // CMD4(CCC_Integer,	"use_new_ballistics",	&g_use_new_ballistics, 0, 1);
    extern float g_bullet_time_factor;
    CMD4(CCC_Float, "g_bullet_time_factor", &g_bullet_time_factor, 0.f, 10.f);
#endif

#ifdef DEBUG
    extern BOOL g_ai_dbg_sight;
    CMD4(CCC_Integer, "ai_dbg_sight", &g_ai_dbg_sight, 0, 1);
#endif // #ifdef DEBUG

    //Alundaio: Scoped outside DEBUG
    extern BOOL g_ai_aim_use_smooth_aim;
    CMD4(CCC_Integer, "ai_aim_use_smooth_aim", &g_ai_aim_use_smooth_aim, 0, 1);

    extern float g_ai_aim_min_speed;
    CMD4(CCC_Float, "ai_aim_min_speed", &g_ai_aim_min_speed, 0.f, 10.f * PI);

    extern float g_ai_aim_min_angle;
    CMD4(CCC_Float, "ai_aim_min_angle", &g_ai_aim_min_angle, 0.f, 10.f * PI);

    extern float g_ai_aim_max_angle;
    CMD4(CCC_Float, "ai_aim_max_angle", &g_ai_aim_max_angle, 0.f, 10.f * PI);

#ifdef DEBUG
    extern BOOL g_debug_doors;
    CMD4(CCC_Integer, "ai_debug_doors", &g_debug_doors, 0, 1);
#endif // #ifdef DEBUG

    *g_last_saved_game = 0;

    CMD3(CCC_String, "slot_0", g_quick_use_slots[0], 32);
    CMD3(CCC_String, "slot_1", g_quick_use_slots[1], 32);
    CMD3(CCC_String, "slot_2", g_quick_use_slots[2], 32);
    CMD3(CCC_String, "slot_3", g_quick_use_slots[3], 32);

    CMD4(CCC_Integer, "auto_continue_on_load", &g_auto_continue_on_load, 0, 1);
    CMD4(CCC_Integer, "keypress_on_start", &g_keypress_on_start_legacy, 0, 1);
    CMD1(CCC_UI_Time_Factor, "ui_time_factor");
    CMD1(CCC_QaWaterGoto, "qa_water_goto");
    CMD1(CCC_QaWaterState, "qa_water_state");
    CMD1(CCC_QaWaterDive, "qa_water_dive");
    CMD1(CCC_QaWaterRing, "qa_water_ring");
    CMD1(CCC_QaRainShelter, "qa_rain_shelter");
    CMD1(CCC_VisorWipe, "visor_wipe");
    CMD1(CCC_QaHandsState, "qa_hands_state");
    CMD1(CCC_QaVisorState, "qa_visor_state");
    CMD1(CCC_QaHandsRig, "qa_hands_rig");
    CMD1(CCC_QaHudMotion, "qa_hud_motion");
    CMD1(CCC_QaVisorWet, "qa_visor_wet");
    CMD1(CCC_VisorRate, "visor_rate");
    CMD1(CCC_QaVisorBlob, "qa_visor_blob");
    da_opt_bench_register();
    da_profiler_register();
    CMD2(CCC_UI_Time_Dilation_Mode, "time_dilation_inventory", UITimeDilator::Inventory);
    CMD2(CCC_UI_Time_Dilation_Mode, "time_dilation_pda", UITimeDilator::Pda);

    register_mp_console_commands();
}
