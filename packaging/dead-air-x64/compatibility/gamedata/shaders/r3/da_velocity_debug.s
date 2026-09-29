-- r__motion_vectors 1: the G-buffer's motion vectors laid over the finished frame (phase_velocity_debug).

function normal (shader, t_base, t_second, t_detail)
	shader:begin	("da_fullscreen","da_velocity_debug")
			: fog	(false)
			: zb 	(false,false)
			: blend	(true, blend.srcalpha, blend.invsrcalpha)
	shader:dx10texture	("s_velocity",	"$user$velocity")
	shader:dx10sampler	("smp_nofilter")
end
