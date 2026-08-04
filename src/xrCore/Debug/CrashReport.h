#pragma once

#include "xrCore/xr_types.h"

struct CrashRuntimeTelemetry
{
    size_t textureBytes{};
    size_t modelBytes{};
    size_t soundBytes{};
    size_t luaBytes{};
    u32 alifeObjects{};
    u32 onlineObjects{};
    u32 pendingReleaseObjects{};
    float fps{};
    float frameMilliseconds{};
    float renderMilliseconds{};
};

struct _EXCEPTION_POINTERS;

namespace CrashReporter
{
XRCORE_API void Initialize();
XRCORE_API void OnFilesystemInitialized();
XRCORE_API void SetRenderer(pcstr renderer);
XRCORE_API void SetGpuInfo(pcstr name, u32 vendorId, u32 deviceId, u64 dedicatedBytes, u64 sharedBytes,
    u32 featureLevel, u64 driverVersion);
XRCORE_API void SetLocation(pcstr location);
XRCORE_API void SetSavePath(pcstr path);
XRCORE_API void BeginOperation(pcstr operation);
XRCORE_API void EndOperation();
XRCORE_API void UpdateTelemetry(const CrashRuntimeTelemetry& telemetry);
XRCORE_API void SamplePerformance(float fps, float frameMilliseconds, float renderMilliseconds);
XRCORE_API bool WriteCrash(_EXCEPTION_POINTERS* exceptionPointers);
XRCORE_API bool WriteSession();
XRCORE_API pcstr LatestReportPath();
XRCORE_API pcstr PendingCrashReportPath();
XRCORE_API bool MarkCrashReportHandled(pcstr reportPath);
}
