#pragma once

namespace UpdateService
{
enum class State : u8
{
    Idle,
    Checking,
    Current,
    Available,
    Downloading,
    Ready,
    Dismissed,
    CheckFailed,
    DownloadFailed,
    ApplyFailed
};

// Which asset the offer is going to pull. A patch only carries the files that differ from the
// previous release; the applier reconstructs the rest from the installation.
enum class Payload : u8
{
    Full,
    Patch
};

struct Snapshot
{
    State state{State::Idle};
    xr_string version;
    xr_string message;
    xr_string changesEn;
    xr_string changesRu;
    u64 downloadedBytes{};
    u64 totalBytes{};
    Payload payload{Payload::Full};

    // A release with a HIGHER MAJOR than the installed one. It is not an update: a new major
    // breaks compatibility with saves and mods, so it is announced separately and installed
    // by hand into its own folder. Independent of everything above - a player on 1.99.0 can
    // be offered 1.99.1 here AND told that 2.1.0 exists.
    xr_string majorVersion;
    xr_string majorUrl;
    xr_string majorChangesEn;
    xr_string majorChangesRu;
    bool majorDismissed{};
};

void StartCheck();
bool StartDownload();
bool RestartAndApply();
void Dismiss();
// Hides the major-release notice for this session only; the checkbox inside it is what turns
// checking off for good (SetChecksEnabled).
void DismissMajor();
void OpenMajorReleasePage();
Snapshot GetSnapshot();
void Shutdown();

// User-facing switch, mirrored by the Game options checkbox and by the one inside the
// major-release notice. Persisted through the console command `dar_update_check`.
bool ChecksEnabled();
void SetChecksEnabled(bool enabled);
}
