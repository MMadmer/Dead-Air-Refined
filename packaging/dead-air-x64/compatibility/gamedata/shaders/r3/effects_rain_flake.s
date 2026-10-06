-- Copyright (c) 2026 XFined-Ray
-- Released under the MIT licence, see License.txt

-- Snowflakes (dxRainRender::RenderFlakes, the snow mode of the rain sheet; docs/dead-air/WINTER.md).
--
-- The streaks' vertex stage and states; the flake itself is drawn by da_rain_flake.ps, so no texture
-- is bound for it. The G-buffer names are there for the soft depth fade, for the same reason the
-- streaks name all three (effects_rain.s): a slot no script names keeps whatever the previous draw
-- bound to it.

function normal		(shader, t_base, t_second, t_detail)
	shader	: begin	("da_rain","da_rain_flake")
			: zb	(true,false)
			: blend	(true,blend.srcalpha,blend.invsrcalpha)
			: aref 	(true,0)

	shader	: dx10texture	("s_position", "$user$position")
	shader	: dx10texture	("s_diffuse",  "$user$albedo")
	shader	: dx10texture	("s_normal",   "$user$normal")

	shader	: dx10sampler	("smp_nofilter")
end
