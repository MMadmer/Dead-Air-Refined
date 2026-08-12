#pragma once

#define DAR_VERSION_MAJOR 1
#define DAR_VERSION_MINOR 3
#define DAR_VERSION_PATCH 2
#define DAR_VERSION_BUILD 0
#define DAR_VERSION_RESOURCE DAR_VERSION_MAJOR, DAR_VERSION_MINOR, DAR_VERSION_PATCH, DAR_VERSION_BUILD
#define DAR_VERSION_STRING "1.3.2"
#define DAR_WIDEN_IMPL(value) L##value
#define DAR_WIDEN(value) DAR_WIDEN_IMPL(value)
#define DAR_VERSION_WSTRING DAR_WIDEN(DAR_VERSION_STRING)

#ifndef RC_INVOKED
namespace DeadAirRefined
{
inline constexpr char Version[] = DAR_VERSION_STRING;
inline constexpr wchar_t VersionWide[] = DAR_VERSION_WSTRING;
inline constexpr char ProductName[] = "Dead Air: Refined";
}
#endif
