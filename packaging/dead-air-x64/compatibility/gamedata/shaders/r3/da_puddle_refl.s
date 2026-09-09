-- [DA_PORT] Отражения в лужах: полноэкранный проход поверх освещённого кадра.
-- Что и зачем — в da_puddle_refl.ps.

function normal (shader, t_base, t_second, t_detail)
	-- da_fullscreen, а не стоковые заглушки: те объявляют раскладку вершин, не совпадающую с
	-- геометрией FVF::F_TL, которой рисуются такие проходы, а несовпадение DirectX отбрасывает
	-- молча — без ошибки и без картинки.
	shader:begin	("da_fullscreen","da_puddle_refl")
			: fog	(false)
			: zb 	(false,false)
			-- Складываем с кадром по альфе, которую посчитал сам шейдер: отражение добавляется
			-- только там, где лужа и куда луч действительно попал.
			: blend	(true, blend.one, blend.invsrcalpha)

	-- Освещённый кадр без воды — та же копия, которую читает водяной шейдер.
	shader:dx10texture	("s_image",    "$user$ssr")
	-- G-буфер: из него берутся глубина и полусферическая освещённость.
	shader:dx10texture	("s_position", "$user$position")
	-- The baked puddle fill map: how deep rain would stand at this texel, from the level load's
	-- own sweep over the terrain heights. Bound here so that this pass and the terrain G-buffer
	-- pass (uber_deffer.cpp binds the same name to the same texture) place puddles from the SAME
	-- data: the two are required to agree bit for bit, and a mask that differs between them
	-- paints the reflection outside the water.
	shader:dx10texture	("s_puddle_fill", "$user$puddle_fill")
	-- The ripple field: a puddle's rings live in the same field as the lake's (da_puddles.h
	-- reads it through da_wf_ripple_slope). The G-buffer half binds the same name to the same
	-- texture in uber_deffer.cpp, for the same bit-for-bit reason as the fill map above.
	shader:dx10texture	("s_water_ripple", "$user$water_ripple0")

	shader:dx10sampler	("smp_nofilter")
	shader:dx10sampler	("smp_rtlinear")
end
