-- The shader fire (CDaFireEffect): a flame volume marched in the pixel shader over a
-- camera-facing quad. Premultiplied colour over the scene (ONE, INVSRCALPHA), the scene depth
-- read from the G-buffer in the shader (zb off), sorted with the other transparent effects.
-- The noise volumes are the cloud deck's (tools/graphics/gen_cloud_noise.py).

function normal (shader, t_base, t_second, t_detail)
	shader:begin	("da_fire","da_fire")
			: sorting	(3, true)
			: blend		(true, blend.one, blend.invsrcalpha)
			: zb 		(false,false)
			: fog		(false)
	shader:dx10texture	("s_position",		"$user$position")
	shader:dx10texture	("s_cloud_shape",	"da\\da_cloud_shape")
	shader:dx10sampler	("smp_nofilter")
	shader:dx10sampler	("smp_linear")
end
