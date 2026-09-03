-- The visible cloud deck (clouds.vs / clouds.ps), drawn over the sky before combine. Reads the
-- cloud map rendered this frame, so the deck and the shadow it casts are one field.

function normal (shader, t_base, t_second, t_detail)
	shader:begin	("clouds","clouds")
			: fog	(false)
			: zb 	(false,false)
			: sorting	(3, true)
			: blend	(true, blend.srcalpha, blend.invsrcalpha)

	shader:dx10texture	("s_cloud_map", "$user$cloud_map")
	shader:dx10texture	("s_tonemap",   "$user$tonemap")

	shader:dx10sampler	("smp_rtlinear")
	shader:dx10sampler	("smp_nofilter")
end
