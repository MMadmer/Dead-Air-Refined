-- The ray traced reflections' requests of the rain puddles (phase_rt_reflections, dxr.md 3.7): a
-- fullscreen pass over the G-buffer into $user$rt_refl_req, the puddle mask and ripple normal of
-- da_puddle_refl.ps. No blending: every pixel writes its request (zero where there is no puddle).

function normal (shader, t_base, t_second, t_detail)
	shader:begin	("da_fullscreen","da_puddle_refl_req")
			: fog	(false)
			: zb 	(false,false)
			: blend	(false, blend.one, blend.zero)

	shader:dx10texture	("s_position",		"$user$position")
	shader:dx10texture	("s_puddle_fill",	"$user$puddle_fill")
	shader:dx10texture	("s_water_ripple",	"$user$water_ripple0")

	shader:dx10sampler	("smp_nofilter")
	shader:dx10sampler	("smp_rtlinear")
end
