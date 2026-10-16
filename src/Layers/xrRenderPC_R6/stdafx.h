// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// The Vulkan module compiles the R4 sources unchanged (the render code talks the D3D11 interfaces,
// xrVulkan implements them), so its precompiled header is R4's.
#pragma once

#include "../xrRenderPC_R4/stdafx.h"

// The shared sources define a few helpers outside the render namespace; in a static build all modules
// link into one binary, so this module's copies get names of their own.
#define dx11_debug_bind_info_queue r6_debug_bind_info_queue
#define dx11_debug_drain_messages r6_debug_drain_messages
