-- Splash crowns (dxRainRender::Render, the rain.dm batches).
--
-- New: the crowns were drawn with the streaks' blender because rain.dm carries "effects\rain"
-- as its own shader name. dxRainRender picks this one up instead when it is present, and falls
-- back to the detail model's when it is not - so an engine ahead of its gamedata still runs.
--
-- Same states as the streaks. What differs is the pixel shader: no soft depth fade, because a
-- crown's base is flush with the surface it stands on, and the crown converges to the haze
-- like the surface it is rather than dissolving out of it like a streak.

function normal		(shader, t_base, t_second, t_detail)
	shader	: begin	("da_rain_splash","da_rain_splash")
			: zb	(true,false)
			: blend	(true,blend.srcalpha,blend.invsrcalpha)
			: aref 	(true,0)

	shader 	: dx10texture	("s_base", t_base)
	shader 	: dx10sampler	("smp_base")
end
