// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

// The FSR3 DX11 host headers next to ffx-api and Streamline. ffx_framegeneration.h cannot join them:
// both define FfxFrameGenerationConfig.
#include <ffx_api.h>
#include <ffx_api.hpp>
#include <ffx_api_loader.h>
#include <ffx_api_types.h>
#include <dx12/ffx_api_dx12.h>
#include <dx12/ffx_api_dx12.hpp>
#include <ffx_upscale.h>
#include <ffx_upscale.hpp>

#include <FidelityFX/host/ffx_fsr3upscaler.h>
#include <FidelityFX/host/backends/dx11/ffx_dx11.h>

#include <sl.h>
#include <sl_consts.h>
#include <sl_core_api.h>
#include <sl_core_types.h>
#include <sl_dlss.h>
#include <sl_dlss_g.h>
#include <sl_reflex.h>
#include <sl_pcl.h>
#include <sl_hooks.h>
#include <sl_helpers.h>
#include <sl_matrix_helpers.h>
// sl_security.h defines functions and globals: one translation unit per module (sdk_smoke.cpp).
#include <sl_version.h>

int headers_dx11_marker()
{
    ffxDispatchDescUpscale upscale{};
    upscale.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    FfxFsr3UpscalerDispatchDescription fsr3{};
    sl::DLSSOptions dlss{};
    sl::DLSSGOptions dlssg{};
    return static_cast<int>(upscale.header.type != 0) + static_cast<int>(fsr3.reset) + static_cast<int>(dlss.mode) + static_cast<int>(dlssg.mode);
}
