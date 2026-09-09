-- Rain streaks (dxRainRender::Render, the SH_Rain quads).
--
-- Loose override of the archive copy, which pointed both stages at stub_default: a texture
-- fetch times the vertex colour, and nothing else in the whole shader. The pair here fogs the
-- streak with the same haze every other pass uses, gives it the sky and the sun's forward
-- lobe, and softens it against the scene so it stops clipping against silhouettes.
--
-- Splashes used to be drawn with THIS blender as well (rain.dm names "effects\rain"); they
-- have their own now - effects_rain_splash.s - which is what lets a streak carry a soft depth
-- fade that a crown standing on the ground must not have.

function normal		(shader, t_base, t_second, t_detail)
	shader	: begin	("da_rain","da_rain")
			: zb	(true,false)
			: blend	(true,blend.srcalpha,blend.invsrcalpha)
			: aref 	(true,0)

	shader 	: dx10texture	("s_base",     t_base)
	-- The soft fade goes through gbuffer_load_data, and that reads the whole G-buffer, not just
	-- the depth: s_diffuse always, s_normal on the path without GBUFFER_OPTIMIZATION. Only the
	-- depth is used here, but a slot no script names keeps whatever the previous draw bound to
	-- it - which is the failure that has already shipped twice in this tree - so all three are
	-- named. A name the compiler stripped is skipped silently by the recorder, so this is free.
	shader	: dx10texture	("s_position", "$user$position")
	shader	: dx10texture	("s_diffuse",  "$user$albedo")
	shader	: dx10texture	("s_normal",   "$user$normal")

	shader 	: dx10sampler	("smp_base")
	shader	: dx10sampler	("smp_nofilter")
end
