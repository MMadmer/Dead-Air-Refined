// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#pragma once

// SHA-256, for everything that has to answer "are these the bytes the manifest asked for?" -
// the startup verifier, the downloader's per-part check and the commit.
//
// Like the manifest parser next to it, this deliberately depends on nothing but the standard
// library and the OS crypto provider, so the same implementation serves the game and the
// external updater without either pulling in the other's world.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace ContentHash
{
// An incremental digest, for callers whose bytes arrive over time - the downloader hashes a
// body as it streams and cannot wait for a finished file. Everything else here is written on
// top of it, so the tree has exactly one hashing loop.
class Stream
{
public:
    Stream();
    ~Stream();
    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;
    // Movable, because a download that has to restart mid-flight replaces its running digest
    // rather than trying to rewind one. A moved-from Stream is left empty; every method on it
    // then fails cleanly rather than dereferencing nothing.
    Stream(Stream&&) noexcept;
    Stream& operator=(Stream&&) noexcept;

    bool Open();
    bool Append(const void* data, std::size_t size);

    // Lowercase 64-hex, or empty if anything went wrong. Consumes the digest: a Stream is
    // finished once, never continued.
    std::string Finish();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// Lowercase 64-hex digest of the whole file, or an empty string on any failure - a missing
// file, an unreadable one, a provider that would not start. Callers treat empty as "does not
// match", which is the only safe reading.
//
// `cancel` is polled between reads. A single bundle can be hundreds of megabytes, so without
// it a shutdown that joins the verifier waits out the rest of the current file. A cancelled
// hash also returns an empty string, so a caller that can cancel must check the flag before
// concluding the bytes were wrong.
std::string File(const std::wstring& path, const std::atomic_bool& cancel);

// Digest of the first `bytes` bytes, or empty if the file is shorter than that. This is what
// makes a resumed download safe: the partial file on disk has to be proven to be a prefix of
// the bundle still being fetched, not of some other revision interrupted at the same offset.
std::string FilePrefix(const std::wstring& path, std::uint64_t bytes, const std::atomic_bool& cancel);

// Digest of a memory range. Used to recompute a manifest's own content-id, which is what
// catches a hand-edited manifest before its claims are acted on.
std::string Buffer(const void* data, std::size_t size);
}
