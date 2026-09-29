-- r__rt_debug: the ray tracing debug view over the frame (phase_rt_reflections, dxr.md 4.1).

function normal (shader, t_base, t_second, t_detail)
	shader:begin	("da_fullscreen","da_rt_debug")
			: fog	(false)
			: zb 	(false,false)
			: blend	(false, blend.one, blend.zero)

	shader:dx10texture	("s_rt_debug",	"$user$rt_debug")
end
