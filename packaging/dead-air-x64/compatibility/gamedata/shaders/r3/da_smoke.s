-- The shader fire's smoke plume (CDaFireEffect::render_smoke): eroded, lit billboards,
-- alpha-blended, soft against the G-buffer depth, sorted with the other transparent effects.

function normal (shader, t_base, t_second, t_detail)
	shader:begin	("da_smoke","da_smoke")
			: sorting	(3, true)
			: blend		(true, blend.srcalpha, blend.invsrcalpha)
			: zb 		(true,false)
			: fog		(false)
	shader:dx10texture	("s_position",		"$user$position")
	shader:dx10texture	("s_cloud_detail",	"da\\da_cloud_detail")
	shader:dx10sampler	("smp_nofilter")
	shader:dx10sampler	("smp_linear")
end
