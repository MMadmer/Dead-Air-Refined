// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#pragma once

#include "xrUICore/Buttons/UIButton.h"
#include "xrUICore/ScrollView/UIScrollView.h"

class CUIXml;

// Boxes, stars and small glyphs computed per pixel by the xms_ui_shape shader at the screen's
// own resolution: nothing is a texture, so nothing is stretched or blurred at any size. Lengths
// are in the 768-line units of every layout; a border is drawn as whole pixels.
struct XRUICORE_API SUIShapeStyle
{
    enum class Kind : u8
    {
        Box,
        Star,
        Search,
        Check,
        Cross,
        Chevron,
        GasMask,
    };

    Kind kind{Kind::Box};
    u32 fill{};          // ARGB
    u32 line{};          // ARGB: the border; the empty part of a star
    float border{};      // the border's width; a glyph's stroke
    float radius{};      // a box's corner radius
    float soft{};        // how far the edge fades: 0 is crisp, more makes a shadow
    float fraction{1.f}; // the filled part of a star, from the left
    // a gas mask's badge on its cheek and the badge's scale (a pop-in overshoots 1)
    enum class Badge : u8
    {
        None,
        Done,
        Failed,
    };
    Badge badge{Badge::None};
    float badgeScale{1.f};

    bool Visible() const { return (fill >> 24) || ((line >> 24) && (border > 0.f || kind == Kind::Star)); }
};

namespace UIShape
{
// `rect` is absolute, in layout units. The shader is created on first use.
XRUICORE_API void Draw(ui_shader& shader, const Frect& rect, const SUIShapeStyle& style);
// fill="#rrggbb[aa]" line="#rrggbb[aa]" border radius soft fraction kind="box|star|search|check|
// cross|chevron|gasmask" of the node; what the node leaves out keeps its value.
XRUICORE_API void Read(const CUIXml& xml, pcstr path, size_t index, SUIShapeStyle& style);
XRUICORE_API u32 ParseColor(pcstr text, u32 fallback);

// The looks of a button, in CUIShapeButton::Look order: <normal/> <highlight/> <pushed/>
// <checked/> <disabled/> under `path`, each a shape with text="#rrggbb[aa]" for the caption; a
// look the layout leaves out is the normal one.
inline constexpr size_t LookCount = 5;
XRUICORE_API void ReadLooks(
    const CUIXml& xml, pcstr path, size_t index, SUIShapeStyle (&looks)[LookCount], u32 (&text)[LookCount], u32 defaultText);
} // namespace UIShape

// A shape as a window: a panel, a separator, a shadow, a badge, an icon.
class XRUICORE_API CUIShape final : public CUIWindow
{
public:
    CUIShape() : CUIWindow("CUIShape") {}

    SUIShapeStyle style;

    void Draw() override;
    pcstr GetDebugType() override { return "CUIShape"; }

private:
    ui_shader m_shader;
};

// A scroll view whose scrollbar is a thin shape at its right edge instead of the textured stock
// one. The stock bar still keeps the range: the wheel and a drag of the content scroll it.
class XRUICORE_API CUIShapeScrollView final : public CUIScrollView
{
public:
    SUIShapeStyle track;
    SUIShapeStyle thumb;
    float barWidth{2.f}; // in 768-line units

    void Draw() override;
    // the scroll view's own attributes, then <track/> and <thumb/> and bar_width
    void InitLooks(const CUIXml& xml, pcstr path, size_t index);
    pcstr GetDebugType() override { return "CUIShapeScrollView"; }

private:
    ui_shader m_shader;
};

// A button drawn as a shape, with a look per state. Checked is a tab or a filter that is on.
class XRUICORE_API CUIShapeButton final : public CUIButton
{
public:
    enum Look : u8
    {
        lookNormal,
        lookHighlight, // under the cursor or the keyboard focus
        lookPushed,
        lookChecked,
        lookDisabled,
        lookCount,
    };
    static_assert(lookCount == UIShape::LookCount);

    CUIShapeButton();

    SUIShapeStyle looks[lookCount];
    u32 textColors[lookCount]{};
    // a glyph drawn at the left of the caption, in the caption's colour; Kind::Box is none
    SUIShapeStyle::Kind icon{SUIShapeStyle::Kind::Box};
    float iconSize{};
    bool checked{};

    void Draw() override;
    // UIShape::ReadLooks, and icon="search" icon_size="12" on the button's own node
    void InitLooks(const CUIXml& xml, pcstr path, size_t index);
    Look CurrentLook() const;
    pcstr GetDebugType() override { return "CUIShapeButton"; }

private:
    ui_shader m_shader;
};
