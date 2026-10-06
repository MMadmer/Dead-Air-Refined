#pragma once

namespace xray::render::RENDER_NAMESPACE
{
// mesh_ms: the pass takes that mesh shader (vertex slot) and the grass amplification shader (geometry
// slot) instead of the vertex shader (DirectX 12 mesh grass, mesh.md 3.6); false when the material
// has no mesh variant (bump, lightmap, HQ) - then nothing was compiled.
// ps_define: a macro of the caller's own variant of the pixel shader (flat, non-bump materials only).
bool uber_deffer(CBlender_Compile& C, bool hq, LPCSTR _vspec, LPCSTR _pspec, BOOL _aref, LPCSTR _detail_replace = 0,
    bool DO_NOT_FINISH = false, bool zwrite = true, LPCSTR mesh_ms = nullptr, LPCSTR ps_define = nullptr);
void uber_shadow(CBlender_Compile& C, LPCSTR _vspec, LPCSTR detailReplace = nullptr);
// The shadow-pass element of mesh grass (mesh.md 3.10): the mesh shader, the grass amplification
// shader and the alpha-test-only pixel shader over the detail texture; the caller finishes the pass.
void uber_detail_mesh_depth(CBlender_Compile& C, LPCSTR mesh_ms);
} // namespace xray::render::RENDER_NAMESPACE
