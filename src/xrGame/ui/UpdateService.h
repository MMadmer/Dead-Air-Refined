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

struct Snapshot
{
    State state{State::Idle};
    xr_string version;
    xr_string message;
    xr_string changesEn;
    xr_string changesRu;
    u64 downloadedBytes{};
    u64 totalBytes{};
};

void StartCheck();
bool StartDownload();
bool RestartAndApply();
void Dismiss();
Snapshot GetSnapshot();
void Shutdown();
}
