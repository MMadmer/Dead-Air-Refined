#pragma once
#include "xrCore/xrCore.h"

struct XRCORE_API SPPInfo
{
    struct SColor
    {
        // Zero-init: instances get constructed and mixed before validate() ever runs.
        float r = 0.f, g = 0.f, b = 0.f;
        SColor() {}
        SColor(float _r, float _g, float _b) : r(_r), g(_g), b(_b) {}
        IC operator u32() const
        {
            int _r = clampr(iFloor(r * 255.f + .5f), 0, 255);
            int _g = clampr(iFloor(g * 255.f + .5f), 0, 255);
            int _b = clampr(iFloor(b * 255.f + .5f), 0, 255);
            return color_rgba(_r, _g, _b, 0);
        }

        IC operator const Fvector&() const { return *reinterpret_cast<const Fvector*>(this); }
        IC SColor& operator+=(const SColor& ppi)
        {
            r += ppi.r;
            g += ppi.g;
            b += ppi.b;
            return *this;
        }
        IC SColor& operator-=(const SColor& ppi)
        {
            r -= ppi.r;
            g -= ppi.g;
            b -= ppi.b;
            return *this;
        }
        IC SColor& set(float _r, float _g, float _b)
        {
            r = _r;
            g = _g;
            b = _b;
            return *this;
        }
    };
    float blur = 0.f, gray = 0.f;
    struct SDuality
    {
        float h = 0.f, v = 0.f;
        SDuality() {}
        SDuality(float _h, float _v) : h(_h), v(_v) {}
        IC SDuality& set(float _h, float _v)
        {
            h = _h;
            v = _v;
            return *this;
        }
    } duality;
    struct SNoise
    {
        float intensity = 0.f, grain = 0.f;
        float fps = 0.f;
        SNoise() {}
        SNoise(float _i, float _g, float _f) : intensity(_i), grain(_g), fps(_f) {}
        IC SNoise& set(float _i, float _g, float _f)
        {
            intensity = _i;
            grain = _g;
            fps = _f;
            return *this;
        }
    } noise;

    SColor color_base;
    SColor color_gray;
    SColor color_add;
    float cm_influence = 0.f;
    float cm_interpolate = 0.f;
    shared_str cm_tex1;
    shared_str cm_tex2;

    SPPInfo& add(const SPPInfo& ppi);
    SPPInfo& sub(const SPPInfo& ppi);
    void normalize();
    SPPInfo();
    SPPInfo& lerp(const SPPInfo& def, const SPPInfo& to, float factor);
    void validate(pcstr str);
};
