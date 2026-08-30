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
    // Optional headline of the release, shown above the change list. Empty when the release
    // notes carry no theme section.
    xr_string themeEn;
    xr_string themeRu;
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
    xr_string majorThemeEn;
    xr_string majorThemeRu;
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

// Whether the major-release notice may appear. It covers ONLY that notice: updates inside the
// installed major line are always checked, because those are the ones a player is expected to
// take. Mirrored by the Game options checkbox and by the one inside the notice itself, and
// persisted through the console command `dar_major_update_notice`.
bool MajorNoticeEnabled();
void SetMajorNoticeEnabled(bool enabled);
}
