#pragma once

#ifdef XRAY_STATIC_BUILD
#    define ENGINE_API
#else
#    ifdef ENGINE_BUILD
#        define ENGINE_API XR_EXPORT
#    else
#        define ENGINE_API XR_IMPORT
#    endif
#endif

#include "pure.h"
#include "EngineAPI.h"
#include "EventAPI.h"
#include "xrSheduler.h"
#include "xrSound/Sound.h"

// TODO: this should be in render configuration
#define R__NUM_SUN_CASCADES         (3u) // csm/s.ligts
// Was 1. The sibling engine measured the light phase: ~120 shadowed-lamp walks over 4 parallel
// contexts run as 31 waves with 30 allocation misses, and waiting on lists was the costliest part
// of the phase. The sun cascades take three contexts; everything else goes to the lamps, so five
// aux contexts make eight parallel ones (ported from cfc1f78, frame 5.27 -> 5.02 ms there).
// Note: this macro sizes vis_data::marker inside every visual - a full rebuild is required.
#define R__NUM_AUX_CONTEXTS         (5u) // rain/s.lights
#define R__NUM_PARALLEL_CONTEXTS    (R__NUM_SUN_CASCADES + R__NUM_AUX_CONTEXTS)
#define R__NUM_CONTEXTS             (R__NUM_PARALLEL_CONTEXTS + 1/* imm */)

class ENGINE_API CEngine final : public pureFrame, public IEventReceiver
{
    EVENT eQuit;

public:
    // DLL api stuff
    CEngineAPI External;
    CEventAPI Event;
    CSheduler Sheduler;
    CSoundManager Sound;

    void Initialize(GameModule* game, const std::array<RendererModule*, 1>& modules);
    void Destroy();

    void OnEvent(EVENT E, u64 P1, u64 P2) override;
    void OnFrame() override;

    CEngine();
    ~CEngine();
};

ENGINE_API extern CEngine Engine;
