#pragma once

// SHA-256, for everything that has to answer "are these the bytes the manifest asked for?" -
// the startup verifier, and later the downloader's per-part check and the commit.
//
// Like the manifest parser next to it, this deliberately depends on nothing but the standard
// library and the OS crypto provider, so the same implementation serves the game and the
// external updater without either pulling in the other's world.

#include <atomic>
#include <cstddef>
#include <string>

namespace ContentHash
{
// Lowercase 64-hex digest of the whole file, or an empty string on any failure - a missing
// file, an unreadable one, a provider that would not start. Callers treat empty as "does not
// match", which is the only safe reading.
//
// `cancel` is polled between reads. A single bundle can be hundreds of megabytes, so without
// it a shutdown that joins the verifier waits out the rest of the current file. A cancelled
// hash also returns an empty string, so a caller that can cancel must check the flag before
// concluding the bytes were wrong.
std::string File(const std::wstring& path, const std::atomic_bool& cancel);

// Digest of a memory range. Used to recompute a manifest's own content-id, which is what
// catches a hand-edited manifest before its claims are acted on.
std::string Buffer(const void* data, std::size_t size);
}
