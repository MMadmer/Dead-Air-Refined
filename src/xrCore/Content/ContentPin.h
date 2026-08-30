#pragma once

#include "xrCore/xrCore.h"
#include "xrCore/xrstring.h"
#include "xrCommon/xr_vector.h"

// The mount gate.
//
// Content bundles are ordinary archives sitting in `database\` next to everything else, so
// without a gate the filesystem would happily mount whatever is there: a bundle left behind
// by a previous version, a half-written download, a file renamed by hand. Each of those is a
// different flavour of "the player is running content the build was never tested against",
// and the symptom is a missing-asset crash somewhere far from the cause.
//
// So: a bundle-named archive is mounted only when the installed content manifest declares
// that exact name and size. Anything else is skipped and recorded, which turns a silent
// wrong-content boot into a reported, repairable state.
//
// This lives in xrCore because it has to run inside CLocatorAPI::_initialize, long before the
// game DLL exists. Only the three exported queries below cross into xrGame; the gate itself
// stays internal.
namespace ContentPin
{
// Resolves the metadata directory and reads `content-manifest.txt` from it. Call once, at the
// top of filesystem initialisation, before any archive is opened.
void Load(pcstr fsRoot);

// Does this file name have the bundle shape? Callers use it to decide whether an archive
// failure deserves the soft-fail path below or the usual assert.
bool IsBundle(pcstr fileName);

// Should this archive be mounted? Non-bundle names always pass - the gate has no opinion about
// the stock archives or a third-party mod's.
bool ShouldMount(pcstr fileName, size_t size);

// For the caller that gets further than the gate can see: `LoadArchive` normally asserts its
// way out of a malformed index, which for a content bundle is the one realistic corruption
// (a flipped bit is size-preserving, so name+size cannot catch it). It soft-fails instead and
// reports the file here, converting an unrecoverable boot crash into the repair path.
void RecordSkipped(pcstr fileName, pcstr reason);

// True when a manifest was found and parsed. False means the installation cannot say what
// content it should have, which is an incomplete installation rather than a neutral state -
// a build that ships content always ships its manifest.
XRCORE_API bool ManifestLoaded();

// Bundle-shaped archives that were refused, by file name, for the repair UI and the diagnostic
// report. Empty on a healthy installation.
XRCORE_API const xr_vector<shared_str>& Skipped();

// Why each of them was refused, index for index with Skipped().
XRCORE_API const xr_vector<shared_str>& SkipReasons();

// `<fs_root>\.dead-air-x64\`, with a trailing separator. Home of content-manifest.txt,
// content-state.txt and content-incomplete.txt. Valid whether or not the manifest parsed, so
// the recovery path can still find where its files belong.
XRCORE_API pcstr MetaDirectory();
}
