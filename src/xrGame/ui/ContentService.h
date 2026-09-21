// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#pragma once

// The content integrity service.
//
// Content is not optional and has no opt-out, so this service never asks whether the player
// wants content - only whether the installation actually has it. Two passes, deliberately
// split by cost:
//
//   Initialize()  synchronous, at game start. Parses the installed manifest and stats every
//                 bundle it declares. Missing or wrong-sized files are the realistic failure
//                 (a runtime-only patch, an interrupted install) and this catches them for
//                 the price of a few dozen stats, in time for the play gate.
//   StartVerify() on a worker thread, from the main menu. Hashes what Initialize() accepted,
//                 using content-state.txt so a warm installation costs one stat per bundle
//                 instead of several GB of reading.
//
// Nothing here downloads. Repair arrives in a later batch; until then an incomplete
// installation is reported, latched and refused, which is the honest state.
namespace ContentService
{
enum class State : u8
{
    // Initialize() has not run yet.
    Unknown,
    // A repair is in flight: downloading, or installing what was downloaded.
    Repairing,
    // A repair finished. The bundles are in place but the filesystem was initialised without
    // them, so the game has to be restarted before it can use them.
    Repaired,
    // Everything the manifest declares is present, and hashing has confirmed it (or is still
    // running - see Verifying).
    Complete,
    // A full hash pass is in flight. The installation looked complete to Initialize().
    Verifying,
    // Something is missing, truncated, stale or corrupt. The latch is set and play is refused.
    Incomplete,
    // There is no readable manifest, so the installation cannot say what it should contain.
    // Distinct from Incomplete because the repair path has to fetch the manifest first.
    Recovery
};

struct Snapshot
{
    State state{State::Unknown};
    xr_string version;
    xr_string contentId;
    u32 declared{};
    u32 present{};
    u64 missingBytes{};
    // One line per problem, already human-readable, for the dialog, the log and the crash
    // report. Ordered as found, so the first line is the most useful one to show. Every entry
    // here blocks play.
    xr_vector<xr_string> problems;

    // What a running repair is doing, and how far along it is. Both zero when none is running.
    xr_string activity;
    u64 repairDone{};
    u64 repairTotal{};

    // Wrong on disk but harmless to a session - a leftover bundle from an older revision that
    // is not mounted and not needed. Reported and cleaned up, never a reason to refuse a game.
    xr_vector<xr_string> notices;
};

// Cheap and synchronous. Safe to call before the renderer exists; must run before anything
// can start a level.
void Initialize();

// Full hash pass on a worker thread, at most once per session. Does nothing on an
// installation already known to be broken - there is no point hashing what the cheap pass
// already condemned, and the permissive Verifying state must never be reachable from it. The
// exception is a leftover latch on an otherwise clean install: that is precisely the question
// a hash pass exists to settle, and it runs with the gate still shut.
void StartVerify();

// Forces a full re-hash, ignoring content-state.txt, on a worker thread. Behind
// `dar_content_verify`; the verdict lands in the log when the pass finishes.
void ForceVerify();

Snapshot GetSnapshot();

// The play gate's single question, answerable from any thread without blocking. True whenever
// the installation must not load a level: content missing, corrupt, or unknowable.
bool PlayBlocked();

// Human-readable reason for the refusal, for the console and the dialog. Empty when nothing
// is wrong.
xr_string BlockReason();

// Fetches whatever is missing and installs it, on a worker thread. Reports progress through
// the snapshot; the state ends at Repaired or back at Incomplete.
//
// A repair always ends in a relaunch. The filesystem indexes archives once, at startup, and
// unmounting or adding one afterwards is not something the engine supports - so bundles that
// arrive during a session are not usable in that session, and pretending otherwise would trade
// a clear "restart the game" for an unexplained missing-asset crash.
void StartRepair();

// True while a repair is running, so the UI can refuse to start a second one.
bool RepairRunning();

// Starts a fresh copy of the game with this process's command line. Returns false if it could
// not, in which case the caller must say so rather than quitting into nothing.
bool Relaunch();

// Dumps the current picture to the log. Behind `dar_content_state`.
void LogState();

void Shutdown();
}
