#pragma once

#include "xrCore/xr_types.h"
#include "xrCommon/xr_vector.h"

namespace SaveTransactionMarker
{
inline constexpr pcstr suffix = ".txn";
inline constexpr u32 customDataExpected = 1u << 0;
inline constexpr u32 previousMainPresent = 1u << 1;
inline constexpr u32 previousCustomPresent = 1u << 2;
inline constexpr u32 previousSidecarPresent = 1u << 3;
inline constexpr u32 legacyCustomCapture = 1u << 4;

struct State
{
    u64 saveId{};
    u32 flags{};
};

enum class ReadResult
{
    Missing,
    Valid,
    Invalid,
    Error
};

void build(const State& state, xr_vector<u8>& result);
[[nodiscard]] ReadResult read(pcstr path, State& state);
}
