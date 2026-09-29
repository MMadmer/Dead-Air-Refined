#pragma once

// The mod catalog's trust and the protected mode it switches on. Contract:
// docs/dead-air/MOD_CATALOG.md. Signatures are ECDSA P-256 over SHA-256, verified with
// Windows CNG - no cryptography of our own.

#include "xms_core.h"

#include <memory>
#include <string_view>

namespace XMS
{
namespace Catalog
{
struct PublicKey
{
    u8 xy[64]{};
    xr_string id; // 16 lowercase hex digits of SHA-256(xy)
    bool valid{};
};

XRCORE_API bool Sha256(const void* data, size_t size, u8 digest[32]);
XRCORE_API xr_string Sha256Hex(const void* data, size_t size);
XRCORE_API bool Base64UrlDecode(std::string_view text, xr_vector<u8>& out);
XRCORE_API xr_string Base64UrlEncode(const u8* data, size_t size);
// "p256:<base64url of X||Y>"
XRCORE_API bool ParsePublicKey(std::string_view text, PublicKey& out);
XRCORE_API bool VerifyP256(const PublicKey& key, const void* message, size_t size, const u8 signature[64]);

// A signed text file (3.1): the bytes before the [signature] header and what that section says.
struct Signed
{
    std::string_view body;
    xr_string keyText;
    xr_string keyId;
    u8 signature[64]{};
};
XRCORE_API bool SplitSigned(std::string_view text, Signed& out, xr_string& error);
// The text carries a valid signature by `key` (compared by id, then verified).
XRCORE_API bool VerifySigned(std::string_view text, const PublicKey& key, xr_string& error);

struct Listing
{
    xr_string id;
    xr_string github;
    xr_string minVersion;
    xr_string date;
    PublicKey key;
};

// subject: a module id, or "key:<key id>"; version: "*" or one version
struct Revocation
{
    xr_string subject;
    xr_string version;
    xr_string reason;
};

struct VtPolicy
{
    xr_vector<xr_string> trusted; // engine names, lower case
    u32 blockTrusted{1};
    u32 warnTrusted{1};
    u32 blockOthers{5};
    u32 warnOthers{2};
};

// The review inbox (MOD_CATALOG 12.3). No inbox_url: reviews are shown, not written.
struct ReviewPolicy
{
    xr_string inboxUrl;
    xr_string inboxField;
    xr_string salt;
    u32 identityBits{24};
    u32 reviewBits{20};
};

struct Index
{
    u32 format{};
    u64 serial{};
    xr_string issued;
    xr_vector<xr_string> mirrors;
    VtPolicy vt;
    ReviewPolicy reviews;
    xr_vector<Listing> mods;
    // withdrawn by their authors: not offered any more, the id still bound to its repository and key
    xr_vector<Listing> withdrawn;
    xr_vector<Revocation> revoked;

    XRCORE_API const Listing* Find(std::string_view id) const;
    XRCORE_API const Listing* FindWithdrawn(std::string_view id) const;
    // What revokes this module, this version of it or its key; nullptr when nothing does.
    XRCORE_API const Revocation* Revoked(std::string_view id, std::string_view version, std::string_view keyId) const;
};

inline constexpr u32 SupportedFormat = 1;
inline constexpr size_t MaximumIndexBytes = 1024 * 1024;

// The maintainer keys the game trusts. Under -qa_catalog: DAR_QA_CATALOG_KEY and nothing else.
XRCORE_API const xr_vector<PublicKey>& MaintainerKeys();
// Where the catalog is served, before the mirrors a signed index adds. Under -qa_catalog:
// DAR_QA_CATALOG_BASE (a loopback http URL) and nothing else.
XRCORE_API const xr_vector<xr_string>& BuiltInMirrors();
XRCORE_API bool QaMode();

// Verifies `text` with a maintainer key and parses it. `format` above SupportedFormat parses
// the [catalog] section only and returns false with error "format".
XRCORE_API bool ReadIndex(std::string_view text, Index& out, xr_string& error);

// modules\.catalog\ with a trailing delimiter; empty until XMS is initialised.
XRCORE_API xr_string CacheRoot();

// The accepted catalog of this process: the cached one read at start-up, replaced by Accept.
// Empty when there is none. Shared, because the Mods menu's worker replaces it while the UI
// reads it.
XRCORE_API std::shared_ptr<const Index> Current();
XRCORE_API u64 AcceptedSerial();
// Stores a verified index as the accepted one. Refused when its serial is below the accepted
// serial (rollback); an equal serial is accepted (the same publication from another mirror).
XRCORE_API bool Accept(std::string_view verifiedText, const Index& index, xr_string& error);

// Modules installed through the Mod Browser (MOD_CATALOG 13), and the ones installed by hand that
// were found to be a release of the catalog byte for byte (MOD_CATALOG 14.1): one record for both.
XRCORE_API bool InstalledFromBrowser(pcstr id);
XRCORE_API void MarkInstalled(pcstr id, bool installed);

// A module the catalog lists under the module's own repository - however it got onto the disk.
XRCORE_API bool Listed(const Module& m);
// A module its author withdrew from the catalog, under the module's own repository.
XRCORE_API bool Withdrawn(const Module& m);
// The catalog's module and not merely one with the same id: installed through the browser, or
// listed under the module's own repository.
XRCORE_API bool IsCatalogModule(const Module& m);
// The reason the catalog revokes this installed module, listed or withdrawn; empty when it does not.
XRCORE_API xr_string RevocationReason(const Module& m);

// Protected mode (9.1), decided once at start-up.
XRCORE_API bool ProtectedMode();
// Why it is on or off, for the log, the console and the Mods window.
XRCORE_API xr_string ProtectedReason();
// The player's switch. On can always be chosen; off only while no catalog module is enabled,
// and it takes effect at the next start like every other part of the decision.
XRCORE_API bool ProtectionLatched();
XRCORE_API bool SetProtectionLatched(bool on, xr_string& error);
// A catalog module is enabled now: protection cannot be switched off.
XRCORE_API bool CatalogModuleEnabled();

// Called by InitializeAndMount: Begin before the staged changes are applied (they mark what
// the browser installed), Evaluate after discovery - it reads the cached catalog, disables
// revoked modules and makes the protected mode decision.
void Begin(pcstr game_root);
void Evaluate(xr_vector<Module>& modules, pcstr game_root, pcstr app_data_root);
} // namespace Catalog

namespace Protect
{
enum class Access : u8
{
    Read,
    Write,     // create, change, delete or rename a file
    WriteDir,  // make, remove or rename a folder
};

// Whether a script may touch `path` (9.3). Always true outside protected mode.
XRCORE_API bool Allow(pcstr path, Access access, xr_string& why);

// Who gave the console command being run (9.2). Player: typed into the console, user.ltx, the
// command line. Script: a script's get_console():execute(). Other: everything else - a deferred
// event, a key bound to a command, the engine itself - and it is checked like a script.
enum class Origin : u8
{
    Other,
    Player,
    Script,
};

// Sets the origin for the calls below it on this thread; nested commands (cfg_load) inherit it.
class XRCORE_API OriginScope
{
public:
    explicit OriginScope(Origin origin);
    ~OriginScope();
    OriginScope(const OriginScope&) = delete;
    OriginScope& operator=(const OriginScope&) = delete;

private:
    Origin previous;
};

XRCORE_API Origin CurrentOrigin();

// Whether the console may run this command now (9.2). Always true outside protected mode and
// for the player's own commands.
XRCORE_API bool AllowConsole(pcstr command, xr_string& why);
// One log line per refusal: "! [protect] <what> refused: <why>".
XRCORE_API void Refused(pcstr what, pcstr why, pcstr where = nullptr);
} // namespace Protect
} // namespace XMS
