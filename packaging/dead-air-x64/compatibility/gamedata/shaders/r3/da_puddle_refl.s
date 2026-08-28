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

	shader:dx10sampler	("smp_nofilter")
	shader:dx10sampler	("smp_rtlinear")
end
