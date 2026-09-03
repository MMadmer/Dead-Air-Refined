-- The deck on the sky (phase_clouds_composite): premultiplied colour over the skybox, the
-- transmittance in alpha (ONE, SRCALPHA). The sky pixels are selected by the stencil in C++.

function normal (shader, t_base, t_second, t_detail)
	shader:begin	("da_fullscreen","da_clouds")
			: fog	(false)
			: zb 	(false,false)
			: blend	(true, blend.one, blend.srcalpha)
	shader:dx10texture	("s_cloud_map",	"$user$cloud_map")
	shader:dx10texture	("s_clouds",	"$user$clouds0")
	shader:dx10texture	("s_tonemap",	"$user$tonemap")
	shader:dx10sampler	("smp_linear")
	shader:dx10sampler	("smp_rtlinear")
	shader:dx10sampler	("smp_nofilter")
end
