# The level's geometry by clusters and the automatic levels of detail (1.6.3)

Two systems after the cluster pipelines and the automatic levels of detail of the shipped engines (Karis et al., the
virtualized geometry talk of SIGGRAPH 2021, and the engines' reducers and screen sizes), built on this engine's own
geometry:

- **The cluster hierarchy** of the level's static visuals: every visual's triangles cut into clusters, the clusters
  simplified group by group into a directed acyclic graph of levels of detail, and each view's cut of it chosen on the GPU
  per cluster by its error on the screen and its place in the view, then drawn with one indirect draw a shader pass and
  vertex buffer.
- **The automatic levels of detail** of the model meshes that ship without their own: a chain of coarser index lists
  reduced from the full mesh, switched by the model's size on the screen and crossfaded by a dither.

Sources: `src/Layers/xrRender/da_mesh_simplify.{h,cpp}` (the simplifier), `da_graph_partition.{h,cpp}` (the graph
partitioner), `da_cluster_dag.{h,cpp}` (the hierarchy of one mesh), `da_clusters.{h,cpp}` (the level, the views, the draws),
`da_autolod.{h,cpp}` (the chains); shaders `r3/da_cluster.h`, `r3/da_cluster_cull.cs`, `r3/da_cluster_args.cs`,
`r3/da_cluster_expand.cs`, `r3/da_fade.h` (the complementary dither). Hooks: `r2_loader.cpp` (load, unload),
`r2_R_render.cpp` (the frame, the main view), `render_phase_sun.cpp` (the cascades), `r2_R_lights.cpp` (the lamp faces),
`r__dsgraph_render.cpp` (the draws), `FSkinned.cpp` (the chains of the skinned and rigid meshes). The checks outside the
engine: `tools/clusters/cluster_check.cpp` (`build_check.ps1`).

## The hierarchy

**Clusters.** A mesh's triangles go to the graph partitioner as a graph of their shared edges (each a weight of 260) and
of locality links (a weight of 1): the triangles sorted along a Morton curve of their centres (ten bits an axis over the
mesh's bounds), and one whose connected piece takes fewer than 128 places of that order linked to the five nearest of
other pieces within 16 steps each way, skipping past its own piece's runs - loose pieces join the clusters of their
neighbours instead of each its own. The partitioner (`da_graph_partition.h`) bisects recursively, multilevel: heavy-edge
matching down to some fifty nodes, greedy growing from a few seeds, Fiduccia-Mattheyses refinement on the way back up.
Each bisection splits the part count rounded from the size halfway between the bounds (124 and 128 triangles: 126) in
two halves and holds the left side to its share within a tenth of a percent where the parts are tight, a fifth where
they are loose (the groups). A cluster holds 128 triangles at the most and 256 vertices (a part over that halves along
its order until it fits).

**Groups and levels.** A level's clusters are grouped by 8 to 32 along their shared borders: the cluster graph's edges
weigh 16 a shared edge plus 4 (12 a shared edge between siblings of one generating group, so the next level's groups
cross the last level's borders), with the same locality links; 32 or fewer clusters form one group. Each group's
triangles are simplified to two triangles under the cluster size a parent (126 aimed at), a parent for every two clusters'
worth of the group's triangles, with every position on an edge the group shares with another group of its level locked
(and a mesh's open border always: the next visual's border lies on it); the result is cut into the parents. A cut that
needs more parents than that simplifies again from the group's own triangles aiming two lower, down to half the cluster
size. A group that keeps more than 85 % of its triangles is stuck (locked all round): its clusters go on into the next
level as they are, grouped again there with their neighbours, whose borders that level locks (left as roots, a border
between them and a neighbour's parents would be no group's and simplify away: a crack). A level where no group simplifies,
or that does not shrink, leaves what is left as roots.

**Errors and spheres.** A first-level cluster's error is nought, its sphere its own. A group's error is the largest of its
children's and its simplification's; its sphere bounds its children's. The parents take the group's error and sphere as
their own, the children take them as their parents'; a root's parents' error is 1e10. Every child of a group and every
parent of it share one pair of values, so the cut - a cluster drawn when its own error on the screen is within the
threshold and its parents' is not - switches a group's children and its parents together, and leaves no crack between
neighbours. A sphere inside another never projects larger (the projection's bound below), so the errors on the screen
rise monotonically up the graph. `cluster_check` builds a closed sphere with a texture seam, a box of six faces with hard
normals, a terrain grid and a soup of loose triangles, and checks every cut from four distances at two thresholds for open
edges: none on the closed meshes.

**The simplifier** (`da_mesh_simplify.h`). Greedy edge collapse ordered by quadrics (Garland and Heckbert): per vertex - a
wedge, a corner's position with its attributes - the sum of its triangles' squared distances to their planes and, per
attribute, to the triangle's linear field of it (Hoppe's attribute quadric, its compact form), each weighted by the
triangle's area; a seam (a material, a texture set's mirroring) keeps its sides apart; a border line, open or a seam, is
held to its line at twice its length (the cluster builder's weight); a degree over 24 costs 0.5 a triangle. Positions
are scaled so the mean triangle is a quarter of a unit across, the weights read against that: a normal 1, a tangent frame
1/16, a colour 1/16 (the hemisphere term), a texture set one over 128 of its mean triangle in texture space. The link
condition (a position next to both ends of the edge is the far corner of one of its triangles) keeps the surface a
manifold; a collapse that would turn a triangle over or move a locked position is never taken.

**The one adaptation.** Every vertex of every level is one of the mesh's own: a collapse moves one end onto the other and
the cost picks the kept end. The level's vertices carry baked light - the hemisphere term in the normal's fourth byte, the
lightmap's coordinates, the vertex colours - that a moved vertex would have to interpolate, and every level reuses the
level's own vertex buffer: only its indices are new (the hierarchy of Cordon is 21.5 MB, with optimal placement it would
have been over 100 MB of new vertices). The error of a simplification is measured, not estimated: each removed position's
distance to the triangles round the position it went into (the one-sided Hausdorff distance over a neighbourhood, an
upper bound of the true one), in metres.

## The level

At the level's load (`da_clusters_level_load`, after every level visual is known) the candidates are gathered on the main
thread: the level's own normal and progressive visuals over its vertex and index buffers (a progressive visual by its
first sliding window: the full detail), each with its fast geometry where it has one (`level.geomX`: the positions alone,
welded - the shadow maps' opaque casters, as the stock draws take them). Worker threads under the game's priority read
`level.geom` and `level.geomX` back (the GPU buffers keep no copy) and build both hierarchies of every visual, the
biggest first; the result is kept in `$app_data_root$\cache\clusters\<level>_<key>_<version>.bin` (the key over the
visuals' ranges and the file's size; `kVersion` in `da_clusters.cpp`). Until the hierarchy is on the GPU the stock draws
draw the level. Cordon: 3634 visuals, 1.11 million triangles, 38071 clusters, built in 14 s with both geometries, read
back in 10 ms.

## The views

`da_clusters_cull` runs for a view on its own context after its caster build and before its targets are set: the main
view before the G-buffer, a sun cascade before its map (on the cascade's context, which may record in parallel), a lamp
face before its maps. It takes the view's cluster visuals out of its shader pass lists - not a visual fading across its
draw distance (the dither of `da_fade.h` is per visual), not a translucent, decal or second-priority pass - and gives
each its instance (its first cluster, its clusters, its draw: a pass and a vertex buffer of the view) and its rows of the
cull's table (a row a 64 clusters), in the frame's pools at offsets the CPU allots (a view that does not fit keeps the
stock path; so does a view on a worker context where the driver has no command lists of its own). A shadow map's pass
whose vertex shader reads the position alone takes the visual's fast hierarchy.

- **The cull** (`da_cluster_cull.cs`): a lane a cluster keeps it where the cut takes it - its own error times the view's
  scale over its sphere's projected scale within one, its parents' past it - and its box is inside the frustum (a shadow
  map's sides only: a caster clamped onto the map's depth range still casts). The projected scale of a sphere is the least
  of the depth times the cosine off the axis over it (a length across the ray at depth z and angle t covers f / (z cos t)
  of the screen), bounded below by the nearest depth squared over the farthest distance; an orthographic view's is one.
  The view's scale: half the render height times the projection's y term over the threshold - 1 pixel in the main view,
  2 texels in the shadow maps (the shipped default: a shadow's cut one level coarser). A kept cluster takes the room for
  its triangles in its draw's list, rounded to an even count.
- **The draws' arguments** (`da_cluster_args.cs`): one group lays the view's draws out in the frame's index pool (their
  counts summed in order, the view's whole taken from the pool's top at once), writes each draw's DrawIndexedInstanced
  arguments and the expansion's dispatch.
- **The indices** (`da_cluster_expand.cs`): a group a kept cluster, a lane two of its triangles, writes their six 16-bit
  indices - each corner's byte in the cluster's vertex table, the table's word the vertex's place in the level's vertex
  buffer - into its draw's list.
- **The draws**: `render_graph` draws a pass's cluster draws right after the pass is set, one indirect draw a vertex
  buffer over the level's own buffer and the frame's index pool, then the visuals left on the list as before. A lamp's
  shadow cache fill draws them into the light's slot and the live map's call after it draws none of them again.
- **The two passes** (`r__clusters_hzb`): in the main view the first pass tests the clusters against the last frame's
  depth pyramid with its own camera (`da_hzb_keep_previous`); the hidden go to a second list, tested again against the
  pyramid of the frame's own first draws and drawn after them - nothing appears a frame late. Off by default (below).

## The automatic levels of detail

A skinned or rigid model mesh (`CSkeletonX_ST`: one without the sliding windows of a progressive mesh, software skinning
aside) of 400 triangles or more (`r__autolod_min_tris`) gets a chain on worker threads after its load: each level
reduced from the full one, not from the last, to a half, a quarter and an eighth of its triangles (a floor of 64; a level
that saves under a tenth over the last ends the chain) by the reducer's weights - a normal 16, the tangent and binormal
0.1, the texture set 0.5 over its range - in centimetres, the border lines 512 times their length, the degree penalty 100
past 24, the bones' regions kept apart (a triangle's seam id its first corner's strongest bone); every level the mesh's
own vertices, bones and weights. The levels' indices become the chain's index buffer over the mesh's vertex buffer; the
chain is kept in `$app_data_root$\cache\autolod\<hash>_<version>.bin` and shared by every copy of the model.

**Screen sizes.** The full level's is 2; a level's is where its deviation covers `r__autolod_px` pixels of a 1920 x 1080
view of 90 degrees: 1.7778 R / max(0.01 m, 960 deviation / pixels + R), R the bounding sphere's radius, never over half
the last level's. A model's size on the screen is its sphere's diameter over the view's smaller extent; it takes the
coarsest level whose size it still reaches (a coarser one only a twentieth under it: the dead band against a model
standing on a threshold). The pixels are 2 by default: the shipped reducers set 8 to 12 against an estimate of the
deviation several times smaller than the measured one.

**The crossfade.** A change in the G-buffer crosses over half a second: the new level keeps the pixels a dither leaves the
old one (`da_fade.h`: the same interleaved gradient noise, the new level the complementary pixels - none drawn twice,
none missing), TAA resolving it. The shadow maps take the camera's level at once.

## Cost

The QA rig (`tools/qa/water/veg_cost.lua`: modes interleaved at each heading, a 2.5 s settle, 3 s windows of
`r__gpu_log`; `tools/qa/Parse-VegCost.py`), Cordon's village (`trees` save), Extreme, July noon, the GPU's milliseconds
summed over two headings (the first mode after a turn of the camera reads high: compare the later ones):

| run | stock frame | clusters frame | stock scene | clusters scene | stock sun | clusters sun |
| --- | --- | --- | --- | --- | --- | --- |
| DirectX 11, every view | 36.39 / 36.86 | 37.49 | 2.29 / 2.32 | 2.45 | 8.63 / 8.64 | 9.06 |
| DirectX 11, the main view | 36.39 / 36.86 | 36.49 / 37.35 | 2.29 / 2.32 | 2.39 / 2.59 | 8.63 / 8.64 | 8.60 / 8.64 |
| DirectX 11, the cascades | 36.39 / 36.86 | 36.66 | 2.29 / 2.32 | 2.27 | 8.63 / 8.64 | 8.88 |
| DirectX 12, the main view (one heading) | 17.46 | 17.25 | 2.02 | 2.01 | 4.99 | 4.91 |
| Vulkan, the main view (one heading) | 17.65 / 17.82 | 17.44 / 17.16 | 2.80 / 2.85 | 2.83 / 2.97 | 3.71 / 3.76 | 3.65 / 3.60 |

The triangles (pipeline statistics, `[tris]`): the main view 342 thousand -> 261 thousand (-24 %), the sun 4.37 million ->
4.10 million (the static casters -270 thousand of some 0.8 million; the rest is the vegetation's), the main view's 839
static visuals' draws -> 142 indirect draws. The hierarchy cuts the triangles it is given, but the level is low in
polygons - 1.1 million triangles for the whole of Cordon - and there is little to cut: in the main view the gain stays
within the run's noise on DirectX 11 and is 0.2 ms of the frame on DirectX 12 and Vulkan; in the cascades the cull's three
dispatches a cascade and their transitions between compute and drawing cost 0.25-0.4 ms on DirectX 11 against 0.1 ms of
triangles saved, and the main view's second pass and its pyramid cost more than they hid (the trees, the level's best
occluders, draw after the static geometry). The defaults follow the numbers: the main view by clusters, the shadow maps
and the two passes off (`r__clusters_views 1`, `r__clusters_hzb 0`), the switches kept for content denser than the stock
levels. The automatic levels of detail cost nothing measurable at the village's few characters; 246 chains on Cordon,
815 thousand triangles in, 713 thousand in their coarser levels.

Rejected for the vegetation's shadows (the sun's 2.6-4.3 million triangles are 80-90 % trees and shrubs): culling the
casters by the receivers the view sees (the virtual shadow maps' page marking adapted to cascades) - the volumetric light,
the lit particles, the water and the glass read the sun's maps along the view rays and on surfaces that are not in the
depth buffer, and a mask from the depth alone would take their casters away.

## Console

- `r__clusters 0|1` (default 1): the cluster path; 0 the stock draws (a hierarchy is built at the next level load only
  while it is on).
- `r__clusters_views <mask>` (QA, default 1): the views by clusters - 1 the main view, 2 the sun's cascades, 4 the lamp
  faces.
- `r__clusters_px <0.25..16>` (default 1): the main view's error on the screen, pixels.
- `r__clusters_shadow_px <0.25..32>` (default 2): the shadow maps', texels.
- `r__clusters_hzb 0|1` (QA, default 0): the main view's two passes against the depth pyramid.
- `r__clusters_fast 0|1` (QA, default 1): 0 the shadow maps take the full geometry's hierarchy.
- `r__clusters_min_tris <0..4096>` (default 0): visuals under this draw whole (from the next level load).
- `r__clusters_stats`: the level's hierarchy, then two frames - views, instances, candidates, draws, the views left to the
  stock path - and a readback of the GPU's counters (the clusters kept and hidden, the index pool's use, `FULL` past it).
- `r__autolod 0|1` (default 1), `r__autolod_min_tris <64..65535>` (default 400, from the next model load),
  `r__autolod_px <0.25..32>` (default 2), `r__autolod_scale <0.05..20>` (QA, default 1: the screen sizes scaled),
  `r__autolod_stats`: the chains built, read from the cache, on the GPU, the meshes left whole, the triangles.
- `r__gpu_log` adds a `[tris]` line: the pipeline statistics of the G-buffer, the sun and the local lights (primitives into
  the input assembler, to the rasterizer and drawn, vertex and pixel shader runs), per frame.

The level log prints `* [clusters] <level>: V visuals, T triangles -> C clusters (T' triangles over L levels, S stuck
groups), M MB, built in / from the disk cache in X ms`.
