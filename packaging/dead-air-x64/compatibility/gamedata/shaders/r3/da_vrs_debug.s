-- r__vrs_debug 1: the frame's shading rates over the finished frame (phase_vrs_debug).

function normal (shader, t_base, t_second, t_detail)
	shader:begin	("da_fullscreen","da_vrs_debug")
			: fog	(false)
			: zb 	(false,false)
			: blend	(true, blend.srcalpha, blend.invsrcalpha)
	shader:dx10texture	("s_vrs_rate",	"$user$vrs_prev")
end
