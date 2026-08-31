#pragma once

// Where everything lives, answered once.
//
// Three processes need these paths - the engine at filesystem init, the game's content
// service, and the standalone updater - and only the engine has an FS to ask. Deriving them
// by hand in each place is how the game ends up reading a latch the updater wrote somewhere
// else, so all three take them from here.

#include <filesystem>

namespace ContentPaths
{
// Everything is relative to the installation root: the directory holding the executable and
// the engine DLLs, which is `{app}` to the installer.
struct Layout
{
    std::filesystem::path root;

    std::filesystem::path Meta() const { return root / L".dead-air-x64"; }
    std::filesystem::path Manifest() const { return Meta() / L"content-manifest.txt"; }
    std::filesystem::path State() const { return Meta() / L"content-state.txt"; }
    std::filesystem::path Latch() const { return Meta() / L"content-incomplete.txt"; }

    // Downloads land here and are only ever moved into `database\` after their hash matches,
    // which is what makes a partial file structurally unable to become a mounted bundle. It
    // sits under the meta directory so it shares a volume with nothing that matters and is
    // trivially findable by a player wondering where the gigabytes went.
    std::filesystem::path Cache() const { return Meta() / L"content-cache"; }

    // Deltas that failed a hash verdict with every I/O call succeeding. Consulted so a bad
    // delta is not fetched twice, and cleared by a commit against a new content-id.
    std::filesystem::path RejectedDeltas() const { return Meta() / L"rejected-deltas.txt"; }

    // Progress and result files for a fetch driven by another process (the installer wizard).
    std::filesystem::path FetchProgress() const { return Cache() / L"content-fetch-progress.txt"; }
    std::filesystem::path FetchResult() const { return Cache() / L"content-fetch-result.txt"; }

    // Where mounted bundles live. Fixed, not read from fsgame.ltx: the installer and the
    // uninstaller write here by absolute path, so a content system that resolved it from data
    // could fetch into a directory the game never mounts and re-download forever.
    std::filesystem::path Database() const { return root / L"database"; }
};

// The named mutex a content fetch holds for its lifetime. The installer's wizard page polls it
// to tell "still working" from "died without writing a result".
inline constexpr wchar_t FetchMutexName[] = L"Local\\DeadAirRefined.ContentFetch";

// The named mutex the running game holds. Inno's AppMutex checks it so an install or uninstall
// refuses while the game has files open.
inline constexpr wchar_t GameMutexName[] = L"Local\\DeadAirRefined.Game";
}
