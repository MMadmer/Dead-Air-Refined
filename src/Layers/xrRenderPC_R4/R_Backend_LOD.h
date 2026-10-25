#pragma once

namespace xray::render::RENDER_NAMESPACE
{
class R_LOD
{
public:
    R_constant* c_LOD;
    // da_fade (da_fade.h): the share of a visual's pixels drawn while it crosses its draw distance.
    R_constant* c_fade;

public:
    explicit R_LOD(CBackend& cmd_list_in);

    void unmap() { c_LOD = 0; c_fade = 0; }
    void set_LOD(R_constant* C) { c_LOD = C; }
    void set_LOD(float LOD);
    void set_fade(R_constant* C) { c_fade = C; set_fade(1.f); }
    // inverted: the complementary pixels (the level of detail fading in over the one fading out, da_autolod.h).
    void set_fade(float cover, bool inverted = false);

    CBackend& cmd_list;
};
} // namespace xray::render::RENDER_NAMESPACE
