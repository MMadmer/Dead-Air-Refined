#pragma once

// Update checks and downloads for XMS modules, behind the Mods menu.
// Contract: docs/dead-air/MOD_UPDATES.md, section 3.

namespace ModUpdateService
{
enum class State : u8
{
    NoSource,    // the manifest names no [update] github
    Unchecked,
    Checking,
    Current,
    NoRelease,   // the repository publishes no descriptor for this module
    Available,
    Blocked,     // the release needs a newer game
    CheckFailed,
    Queued,
    Preparing,   // hashing installed files to find what the release already has here
    Downloading,
    Staged,      // swapped in by the next start
    Failed
};

struct ModuleStatus
{
    xr_string id;
    State state{State::NoSource};
    // Available, Blocked, and every state of an update in flight
    xr_string version;
    // The release carries the version that is already installed, with other content: the author
    // took it down and published it again. The same update, told to the player in other words -
    // "1.0.0 is available" over an installed 1.0.0 reads like a fault.
    bool reissue{};
    // Blocked only: the game version the release asks for. Empty when it is the descriptor
    // schema this build cannot read, which no version number on our side can express.
    xr_string requiresGame;
    // log-grade reason behind CheckFailed and Failed
    xr_string message;
    // Available: what the update is expected to download. Preparing: bytes of installed files
    // hashed. Downloading: packed bytes fetched.
    u64 downloadedBytes{};
    u64 totalBytes{};
};

struct Snapshot
{
    // one entry per XMS::Modules() entry, same order
    xr_vector<ModuleStatus> modules;
    bool busy{};
    // the queue ran dry with at least one module staged and the player has not been asked yet
    bool restartPrompt{};
};

// Once per process from the main menu, and again from the Mods window: a pass only touches
// modules that were never checked or whose last check failed.
void StartCheck();

bool StartUpdate(pcstr moduleId);
// Number of modules queued.
u32 StartUpdateAll();

// The console's way in (xms_update): checks first, then takes what turned out to be available.
// An empty id means every module. Unlike the buttons it needs no finished check to act on.
void RequestUpdate(pcstr moduleId);
// One log line per module (xms_update_status).
void LogStatus();

// Cheap enough for every frame: the main menu reopens the Mods window for a prompt that came
// due after the player had left it.
bool RestartPromptPending();
void AcknowledgeRestartPrompt();
// Starts a fresh copy of the game and quits this one. False when the copy could not be started.
bool RestartGame();

// Opens the module's page. The URL was validated when the manifest was read and is validated
// again here: the check is the whole point of the feature, so it sits next to the launch.
bool OpenWebsite(pcstr moduleId);

Snapshot GetSnapshot();
void Shutdown();
}
