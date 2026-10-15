// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#pragma once

// What the xms_ui_shape shader draws: a box, a star, a stroke glyph or a solid pictogram, computed
// per pixel at the screen's own resolution. Nothing here is a texture, so nothing blurs at any
// size. Lengths are in the 768-line units of every layout; colours are ARGB.
struct SUIShapeStyle
{
    // The order is the shader's: xms_ui_shape.ps branches on the number.
    enum class Kind : u8
    {
        Box,
        Star,
        Search,
        Check,
        Cross,
        Chevron,
        // solid pictograms
        GasMask,
        Trefoil,
        Grid,
        Pistol,
        Helmet,
        Cutlery,
        Medkit,
        Gem,
        Gear,
        Sort,
        Biohazard,
        Bolt,
        Flame,
        Psi,
        Claw,
        Bullet,
        Impact,
        Burst,
        Heart,
        Drop,
        Runner,
        Weight,
        Clock,
        Wrench,
        Crosshair,
        Hand,
        Gauge,
        Shield,
        Cartridge,
        Moon,
        Upgrade,
        Pin,
        Keyboard,
        Scales,
        Count
    };

    enum class Badge : u8
    {
        None,
        Done,
        Failed,
    };

    Kind kind{Kind::Box};
    u32 fill{};          // ARGB
    u32 line{};          // ARGB: the border; the empty part of a star; a pictogram's glow
    float border{};      // the border's width; a glyph's stroke; a pictogram's glow
    float radius{};      // a box's corner radius
    float soft{};        // how far the edge fades: 0 is crisp, more makes a shadow
    float fraction{1.f}; // the filled part of a star, from the left
    // A box cut into segments along its width: one every `segment`, `gap` of it left empty.
    float segment{};
    float gap{};
    // a gas mask's badge on its cheek and the badge's scale (a pop-in overshoots 1)
    Badge badge{Badge::None};
    float badgeScale{1.f};

    bool Visible() const { return (fill >> 24) || ((line >> 24) && (border > 0.f || kind == Kind::Star)); }
    bool Pictogram() const { return kind >= Kind::GasMask; }
};

// A window's look drawn by the shader in place of its texture. A button has a look per state; a
// window without states only ever shows `normal`.
struct SUIShapeSkin
{
    enum Look : u8
    {
        Normal,
        Highlight, // under the cursor or the keyboard focus
        Pushed,
        Checked, // a switch that is on: a tab, a filter
        Disabled,
        Count,
    };

    // UIShape::ReadSkinNode's attribute bits; TextSet marks a look that colours the caption
    static constexpr u16 TextSet = 1 << 15;

    SUIShapeStyle looks[Count];
    u32 text[Count]{};
    // the attributes each look sets itself: a state keeps those whatever its normal says
    u16 set[Count]{};

    bool HasText(Look look) const { return set[look] & TextSet; }
};
