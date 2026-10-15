// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#pragma once

namespace BugReportConfig
{
// The server as this build knows it; the services document of the mod's repository (RemoteConfig, report_host
// and report_path) overrides both, so a server that moves keeps its reports coming.
inline constexpr wchar_t Host[] = L"dead-air-refined-madmer.mia0.amvera.tech";
inline constexpr wchar_t SubmitPath[] = L"/api/v1/reports";
inline constexpr u32 TitleMaximum = 200;
inline constexpr u32 DescriptionMaximum = 10000;

#if __has_include("BugReportSecrets.local.h")
#include "BugReportSecrets.local.h"
#else
inline xr_string UploadToken() { return {}; }
#endif
}
