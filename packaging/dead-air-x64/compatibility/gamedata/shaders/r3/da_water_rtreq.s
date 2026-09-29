-- The ray traced reflections' requests of the water surfaces (phase_rt_reflections, dxr.md 3.7): the
-- forward water meshes again, water.vs and water.ps up to the reflection vector, into
-- $user$rt_refl_req. Depth-tested less-equal against the scene without writing it (the water writes
-- no depth either), no blending, two-sided like the water (1 is D3DCULL_NONE). The textures are the
-- ones the surface normal reads.

function normal (shader, t_base, t_second, t_detail)
	shader:begin	("water_soft","water_rtreq")
			: fog	(false)
			: zb 	(true,false)
			: blend	(false, blend.one, blend.zero)
			: dx10cullmode	(1)

	shader:dx10texture	("s_nmap",			"water\\water_normal")
	shader:dx10texture	("s_position",		"$user$position")
	shader:dx10texture	("s_water_field",	"$user$water_field")
	shader:dx10texture	("s_water_ripple",	"$user$water_ripple0")

	shader:dx10sampler	("smp_base")
	shader:dx10sampler	("smp_nofilter")
	shader:dx10sampler	("smp_rtlinear")
end
