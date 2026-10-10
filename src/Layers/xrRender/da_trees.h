// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt
//
// The upgraded level trees (docs/dead-air/TREES.md). A stock level tree of a kind the data knows (its FLOD of tree
// parts, keyed like the shrubs' bushes) stands where it stood, its exact placement kept: its stock trunk and limbs
// rebuilt on the skeleton taken from its own mesh (gamedata/meshes/da/trees/*.dtsk, tubes with the stock rings'
// profiles, the stock triangles where they do not fit), new branches of a real ChEZ species grafted on them inside its
// crown, real leaves, needles, flowers, catkins, fruit and cones on them following the species' year day by day. Far
// out its own stock billboard's eight views from the module's atlas. The trees are a population of the vegetation core
// (da_veg.h: the hooks of every pass, da_trees.cpp: the population).
#pragma once

namespace xray::render::RENDER_NAMESPACE
{
// r__trees_stats (QA): the preset's tiers, the trees, templates and memory, this frame's tiers, elements and chunks per
// draw and pass, the billboards, the trees per kind and species.
void da_trees_stats();
// r__trees_species_pin <name|off> (QA): every tree grows that species (from the next level load).
void da_trees_pin_species(pcstr name);
// r__trees_kind_pin <kind key|off> (QA): only the trees of that stock kind are upgraded, the others stand stock (from the
// next level load).
void da_trees_pin_kind(pcstr key);
// r__trees_probe [ray] (QA): the trees nearest the camera - kind, species, distance, template, tier, youth -, or with
// `ray` the ones the view's ray passes through, their foliage's parts too.
void da_trees_probe(pcstr args);

// ---- The billboard bake's entry points (da_trees_bake.cpp) ----------------------------------------------------------
// The module's billboard atlas pair: render targets by these names (A8R8G8B8; albedo + alpha, and the normal in rgb with
// the hemisphere light in alpha, as the stock level_lods and level_lods_nm), `cell` pixels a view, eight views a cell
// set in one row, a facet's uv inset by `gutter` pixels on every side of its cell; `mips` levels (1 without the bake);
// this frame's billboard distance R (m) and the preset's pixel floor. The cell sets: a template's own (0 .. templates),
// then a second set of the templates some of whose trees stand dead among living ones, in the dead look (templates ..
// sets). `material`: the name the trees' billboard shader (da_tree_lod.ps) samples the bake's foliage atlas by.
struct DaTreeBbAtlas
{
    pcstr albedo{}, normal{}, material{};
    u32 width{}, height{}, cell{};
    u32 templates{}, sets{};
    u32 gutter{}, mips{};
    float R{}, kpx{};
};
// One view of a template's billboard: its facet's rect in the atlas (uv edges u0 v0 u1 v1, the cell's gutter round it),
// its facet's quad in the template's model space (the stock vertex order), each corner's uv in the atlas and the facet's
// normal (the view's direction).
struct DaTreeBbView
{
    Fvector4 rect{};
    Fvector corner[4]{};
    Fvector2 uv[4]{};
    Fvector normal{};
};
// A cell set of a template's billboard: the template, its stock kind and species row (the vegetation tables'), the
// world transform of the tree its facets were taken from, its eight views, whether its cells are to be baked (set at
// load; the bake clears it) and hold a picture; the look the bake gives it - its trees' mean youth, dead (the dead look:
// every tree of a template standing dead, or a template's second set), the Red Forest's red needles - and the mean over
// its trees of their records' hemisphere light over their billboard facets' vertex light.
struct DaTreeBbTemplate
{
    u32 tmpl{};
    u64 kind{};
    u32 species{};
    Fmatrix placement_xform{};
    DaTreeBbView view[8]{};
    bool dirty{}, filled{};
    float youth{}, hemi{1.f};
    bool dead{}, red{};
};
// The atlas of this level's trees (false: no trees or no atlas); a cell set (false: none, e.g. past the atlas's room);
// its dirty flag set or cleared (cleared: its cells hold its picture).
bool da_trees_bb_atlas(DaTreeBbAtlas& out);
bool da_trees_bb_template(u32 set, DaTreeBbTemplate& out);
void da_trees_bb_mark(u32 set, bool dirty);
} // namespace xray::render::RENDER_NAMESPACE
