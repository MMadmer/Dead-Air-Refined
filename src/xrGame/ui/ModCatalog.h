// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#pragma once

// The Mod Browser's side of the mod catalog (docs/dead-air/MOD_CATALOG.md): the catalog from its
// mirrors, the cards, thumbnails, VirusTotal verdicts, ratings and reviews, and writing a review.
// What the catalog trusts - the index, its keys, protected mode - is XMS::Catalog in xrCore.

#include "ModRelease.h"
#include "xrCore/XMS/xms_core.h"

#include <memory>

namespace ModCatalog
{
enum class Verdict : u8
{
    Unknown, // not scanned, or no result yet
    Clean,
    Warn,
    Block
};

// A release is judged by its most decisive package: block, warn, not scanned, clean. The counts and
// names are that package's, the date is the oldest analysis of any package of the release.
struct VirusTotal
{
    Verdict verdict{Verdict::Unknown};
    u32 engines{};
    u32 trustedMalicious{};
    u32 trustedSuspicious{};
    u32 othersMalicious{};
    xr_string date;      // YYYY-MM-DD
    xr_string flaggedBy; // engine names
    xr_string package;   // SHA-256 of the package the verdict comes from
    bool live{};         // asked with the player's own key just now, not read from the catalog
    bool unknown{};      // VirusTotal has never seen a package of the release
};

struct Rating
{
    u32 count{};
    u32 average100{}; // average stars times 100
    u32 stars[5]{};
};

struct Review
{
    xr_string keyId;
    xr_string name;    // UTF-8
    xr_string text;    // UTF-8, plain: control characters and UI colour codes removed
    xr_string version;
    xr_string date;
    u32 stars{};
};

struct Entry
{
    xr_string id;
    xr_string github;
    xr_string listed;     // the listing date
    xr_string minVersion; // the lowest listed version
    xr_string revoked;    // the reason, empty when the module is not revoked
    bool hasCard{};       // a card signed by the module's author key is known
    ModRelease::Descriptor release;
    xr_string thumbnailTexture; // "xms_thumbs\<sha256>" once the picture is published, else empty
    Rating rating;
    VirusTotal vt;
};

enum class Status : u8
{
    Idle,
    Loading,
    Ready,
    Offline, // no mirror answered: the cached catalog
    Failed   // no mirror answered and nothing is cached
};

struct Snapshot
{
    Status status{Status::Idle};
    xr_string error;
    u64 serial{};
    bool reviewsWritable{};
    u32 generation{}; // grows whenever anything here changes
    xr_vector<Entry> entries;

    const Entry* Find(std::string_view id) const;
};

// Loads the catalog again: the index from the mirrors, verified and accepted (rollback refused),
// then cards, VirusTotal results and ratings. The cached copy is shown at once and while no
// mirror answers.
void Refresh();
std::shared_ptr<const Snapshot> Current();

// The worker fetches the thumbnail of a listed module when asked; the UI thread publishes what
// arrived (the texture loader reads the VFS only) with MountThumbnails, once per frame is fine.
void WantThumbnail(pcstr id);
void MountThumbnails();

// The main menu's frame: publishes thumbnails, and runs the console commands that waited for the
// catalog (xms_catalog after <command>) once a load has finished.
void Tick();
void RunWhenLoaded(const xr_string& command);
// An installed module's own picture, published from its folder: the texture name, or empty
// when the module has none or it is not a valid thumbnail.
xr_string InstalledThumbnail(const XMS::Module& module);

struct Reviews
{
    xr_string id;
    Status status{Status::Idle};
    xr_string error;
    xr_vector<Review> list;
};
void WantReviews(pcstr id);
std::shared_ptr<const Reviews> GetReviews(pcstr id);

// VirusTotal with the player's own key (Windows Credential Manager; used for nothing else).
bool HasVirusTotalKey();
bool SetVirusTotalKey(pcstr key, xr_string& error);
void ForgetVirusTotalKey();
// A live lookup of the module's package; the answer lands in the entry's vt.
void CheckVirusTotal(pcstr id);
bool VirusTotalBusy(pcstr id);
Verdict Judge(u32 trustedMalicious, u32 trustedSuspicious, u32 othersMalicious);

// Writing a review (MOD_CATALOG 12).
enum class PostState : u8
{
    Idle,
    Identity, // the device identity's proof of work, once per identity
    Working,  // the review's proof of work
    Sending,
    Sent,
    Failed
};
struct PostStatus
{
    PostState state{PostState::Idle};
    xr_string id;
    xr_string error;
};
inline constexpr size_t ReviewMinimum = 30;
inline constexpr size_t ReviewMaximum = 2000;
inline constexpr size_t ReviewNameMaximum = 32;
// `text` and `name` are UTF-8. False with a reason when the review cannot be sent at all.
bool PostReview(pcstr id, u32 stars, const xr_string& text, const xr_string& name, xr_string& error);
PostStatus GetPostStatus();
// Characters of a text as a reader counts them (UTF-8 code points, trimmed).
size_t TextLength(std::string_view utf8);

// The VirusTotal page of a package, opened in the browser.
bool OpenVirusTotalReport(pcstr id);
// The card's website (the manifest's allow-list), for a module that is not installed.
bool OpenWebsite(pcstr id);

void Shutdown();
} // namespace ModCatalog
