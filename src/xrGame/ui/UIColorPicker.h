// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#pragma once

#include "xrUICore/Windows/UIWindow.h"

class CUIXml;
class CUIStatic;
class CUIEditBox;

// A colour picker after Unreal's: a wheel of hue and saturation, a bar of saturation and one of value, the old
// colour beside the new, a slider and a field each for R, G and B (linear, 0..1), H (degrees), S and V (0..1), and
// the colour's hex, linear and sRGB. The colour is opaque. The parts' places come from the layout (its node's
// <wheel>, <saturation>, <value>, <old>, <new>, <row_r> .. <row_v> with <caption> <track> <field>, <hex_linear>
// and <hex_srgb> with <caption> <field>).
class CUIColorPicker final : public CUIWindow
{
public:
    CUIColorPicker();

    void InitPicker(CUIXml& xml, pcstr path);

    // sRGB colours: the one being set and the one it is set from (the old swatch)
    void SetColor(u32 color);
    void SetOldColor(u32 color) { m_old = color; }
    u32 Color() const;
    // Ends what the player is doing here: a drag, a field being typed into (its value taken when kept).
    void FinishInput(bool keep);

    // the player changed the colour
    std::function<void(u32)> onChange;

    void Draw() override;
    bool OnMouseAction(float x, float y, EUIMessages mouse_action) override;
    void SendMessage(CUIWindow* pWnd, s16 msg, void* pData = nullptr) override;
    pcstr GetDebugType() override { return "CUIColorPicker"; }

private:
    enum Row : u8
    {
        RowR,
        RowG,
        RowB,
        RowH,
        RowS,
        RowV,
        RowCount
    };
    enum Part : u8
    {
        PartNone,
        PartWheel,
        PartSaturation,
        PartValue,
        PartTrack, // + the row
    };

    void Drag(const Fvector2& at);
    void Changed();
    void RefreshFields();
    void Commit(CUIEditBox* field);
    // the colour as linear RGB, and set from it (the hue kept where the colour has none)
    Fvector3 Linear() const;
    void SetLinear(const Fvector3& rgb);
    void SetRgb(u32 color);

    float m_h{}, m_s{}, m_v{};
    u32 m_old{ 0xFF000000 };

    Frect m_wheel{}, m_sat{}, m_val{}, m_oldRect{}, m_newRect{};
    Frect m_tracks[RowCount]{};
    CUIEditBox* m_fields[RowCount]{};
    CUIEditBox* m_hexLinear{};
    CUIEditBox* m_hexSrgb{};
    u8 m_drag{ PartNone };
};
