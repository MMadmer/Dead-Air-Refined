// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt
//
// r__native_selftest (docs/dead-air/dx12-features/infra.md 3.7): the native D3D12 paths of the
// layer exercised once in game, each part logged as ok, failed or skipped.
#pragma once

namespace xray::render::RENDER_NAMESPACE
{
// Render thread, in game. The R4 module reports that the test is DirectX 12 only.
void da_native_selftest_run();
} // namespace xray::render::RENDER_NAMESPACE
