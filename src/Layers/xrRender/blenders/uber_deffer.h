#pragma once

namespace xray::render::RENDER_NAMESPACE
{
void uber_deffer(CBlender_Compile& C, bool hq, LPCSTR _vspec, LPCSTR _pspec, BOOL _aref, LPCSTR _detail_replace = 0,
    bool DO_NOT_FINISH = false, bool zwrite = true);
void uber_shadow(CBlender_Compile& C, LPCSTR _vspec, LPCSTR detailReplace = nullptr);
} // namespace xray::render::RENDER_NAMESPACE
