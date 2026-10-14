// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#pragma once

#include "xrUICore/Buttons/UIButton.h"
#include "xrUICore/ScrollView/UIScrollView.h"

class CUIXml;

#include "xrUICore/Static/UIShapeStyle.h"

namespace UIShape
{
// `rect` is absolute, in layout units. The shader is created on first use.
XRUICORE_API void Draw(ui_shader& shader, const Frect& rect, const SUIShapeStyle& style);
// The same with the shader every skinned window shares (UICore keeps it).
XRUICORE_API void Draw(const Frect& rect, const SUIShapeStyle& style);
// Many boxes of one size and one style in one draw: the cells of an item grid. `centres` are
// absolute, in layout units; `colors` (ARGB, one per centre) replace the style's fill when given.
XRUICORE_API void DrawBatch(const Fvector2& size, const xr_vector<Fvector2>& centres, const xr_vector<u32>* colors,
    const SUIShapeStyle& style);
// fill="#rrggbb[aa]|$name" line border radius soft fraction segment gap kind="box|star|...|trefoil|..."
// of the node; what the node leaves out keeps its value.
XRUICORE_API void Read(const CUIXml& xml, pcstr path, size_t index, SUIShapeStyle& style);
// "#rrggbb", "#rrggbbaa" or "$name", a colour of the theme (ui_theme.xml)
XRUICORE_API u32 ParseColor(pcstr text, u32 fallback);
XRUICORE_API SUIShapeStyle::Kind ParseKind(pcstr text, SUIShapeStyle::Kind fallback);
// the same without a word in the log: false when `text` names no shape
XRUICORE_API bool FindKind(pcstr text, SUIShapeStyle::Kind& kind);

// A window's skin from its layout node: shape="style" names a look of the theme; a <shape> child
// takes style="..." and its own attributes over it, and <highlight/> <pushed/> <checked/>
// <disabled/> children for the states. Null when the node asks for neither.
XRUICORE_API std::unique_ptr<SUIShapeSkin> ReadSkin(const CUIXml& xml, pcstr path, size_t index);
// The attributes of `path` over every look of `skin` that does not set them itself, and its state
// children over their looks: a state keeps what it sets, like a CSS rule over its parent's.
XRUICORE_API void ReadSkinNode(const CUIXml& xml, pcstr path, size_t index, SUIShapeSkin& skin);

// The looks of a button, in CUIShapeButton::Look order: <normal/> <highlight/> <pushed/>
// <checked/> <disabled/> under `path`, each a shape with text="#rrggbb[aa]" for the caption; a
// look the layout leaves out is the normal one.
inline constexpr size_t LookCount = SUIShapeSkin::Count;
XRUICORE_API void ReadLooks(
    const CUIXml& xml, pcstr path, size_t index, SUIShapeStyle (&looks)[LookCount], u32 (&text)[LookCount], u32 defaultText);
} // namespace UIShape

// The named looks and colours every layout may use: ui_theme.xml, then every ui_theme_*.xml in
// name order, a later definition of a name replacing the earlier one. A mod restyles the
// interface with one small file of its own.
namespace UITheme
{
XRUICORE_API const SUIShapeSkin* Skin(pcstr name);
XRUICORE_API bool Color(pcstr name, u32& color);
// read again on the next use: the UI is being rebuilt
XRUICORE_API void Reset();
} // namespace UITheme

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
