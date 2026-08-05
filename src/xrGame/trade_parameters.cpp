////////////////////////////////////////////////////////////////////////////
//	Module 		: trade_parameters.cpp
//	Created 	: 13.01.2006
//  Modified 	: 13.01.2006
//	Author		: Dmitriy Iassenev
//	Description : trade parameters class
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "trade_parameters.h"
#include "xrCommon/xr_hash_map.h"

namespace
{
struct LegacyTradeExponentState
{
    float buy{};
    float sell{};
    bool hasBuy{};
    bool hasSell{};
};

// External state restores the legacy Lua controls without changing CTradeParameters layout.
xr_flat_hash_map<const CTradeParameters*, LegacyTradeExponentState> legacyTradeExponents;
}

CTradeParameters* CTradeParameters::m_instance = 0;

CTradeParameters::~CTradeParameters()
{
    legacyTradeExponents.erase(this);
}

void CTradeParameters::set_buy_item_exponent(float factor)
{
    R_ASSERT2(std::isfinite(factor), "Trade buy-item exponent must be finite");
    LegacyTradeExponentState& state = legacyTradeExponents[this];
    state.buy = factor;
    state.hasBuy = true;
}

void CTradeParameters::set_sell_item_exponent(float factor)
{
    R_ASSERT2(std::isfinite(factor), "Trade sell-item exponent must be finite");
    LegacyTradeExponentState& state = legacyTradeExponents[this];
    state.sell = factor;
    state.hasSell = true;
}

float CTradeParameters::item_condition_exponent(bool buying, float fallback) const
{
    const auto state = legacyTradeExponents.find(this);
    if (state == legacyTradeExponents.end())
        return fallback;

    const bool hasOverride = buying ? state->second.hasBuy : state->second.hasSell;
    if (!hasOverride)
        return fallback;

    const float exponent = buying ? state->second.buy : state->second.sell;
    return exponent > 0.0f ? exponent : 0.75f;
}

void CTradeParameters::process(action_show, CInifile& ini_file, const shared_str& section)
{
    VERIFY(ini_file.section_exist(section));
    m_show.clear();
    CInifile::Sect& S = ini_file.r_section(section);
    auto I = S.Data.cbegin();
    auto E = S.Data.cend();
    for (; I != E; ++I)
        if (!(*I).second.size())
            m_show.disable((*I).first);
}
