// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#pragma once

// r__season: the texture set the world wears. Every season is an overlay folder under textures\seasons\
// whose files replace the base textures of the same name, with a levels\<level>\ subfolder for the
// textures of a level folder. Returns that folder ("autumn", "winter", "spring_early", "spring_mid",
// "spring_late", "summer"). The base textures are autumn, so the autumn folder only holds what the
// level archives cannot: the LOD atlases rendered from the base cards. The renderer picks a change up
// between frames and reloads the textures it touches.
ENGINE_API pcstr da_season_folder();
