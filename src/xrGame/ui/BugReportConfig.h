#pragma once

namespace BugReportConfig
{
inline constexpr wchar_t Host[] = L"dead-air-refined-madmer.amvera.io";
inline constexpr wchar_t SubmitPath[] = L"/api/v1/reports";
inline constexpr u32 TitleMaximum = 200;
inline constexpr u32 DescriptionMaximum = 10000;

#if __has_include("BugReportSecrets.local.h")
#include "BugReportSecrets.local.h"
#else
inline xr_string UploadToken() { return {}; }
#endif
}
