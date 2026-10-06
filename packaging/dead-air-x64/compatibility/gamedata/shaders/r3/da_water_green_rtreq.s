-- The requests of the second water material (water_green.ps, profile slot B); see da_water_rtreq.s.

function normal (shader, t_base, t_second, t_detail)
	shader:begin	("water_soft","water_green_rtreq")
			: fog	(false)
			: zb 	(true,false)
			: blend	(false, blend.one, blend.zero)
			: dx10cullmode	(1)

	shader:dx10texture	("s_nmap",			"water\\water_normal")
	shader:dx10texture	("s_position",		"$user$position")
	shader:dx10texture	("s_water_field",	"$user$water_field")
	shader:dx10texture	("s_water_class",	"$user$water_class")	-- running / standing water: the season's ice
	shader:dx10texture	("s_water_ripple",	"$user$water_ripple0")

	shader:dx10sampler	("smp_base")
	shader:dx10sampler	("smp_nofilter")
	shader:dx10sampler	("smp_rtlinear")
end
