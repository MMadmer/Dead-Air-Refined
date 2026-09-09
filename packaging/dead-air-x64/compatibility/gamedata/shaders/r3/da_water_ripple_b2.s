-- One fixed step of the interaction ripple field (phase_water_ripple). What it solves and why
-- each part of it is the way it is: da_water_ripple.ps.

function normal (shader, t_base, t_second, t_detail)
	-- da_fullscreen, not one of the stock stubs: those declare a vertex layout that does not
	-- match the FVF::F_TL geometry these passes are drawn with, and DirectX drops a mismatched
	-- draw in silence - no error, no picture, the target simply keeps what it held.
	shader:begin	("da_fullscreen","da_water_ripple")
			: fog	(false)
			: zb 	(false,false)
			-- The step REPLACES the state; there is nothing to blend with.
			: blend	(false, blend.one, blend.zero)

	-- Band 2 of the ripple field (1.2 m rings): da_water_ripple.s with this band's previous
	-- half. The pass writes into $user$water_ripple2 and copies the result back into this
	-- half, so this name is always "one step ago" and never the target being written.
	shader:dx10texture	("s_water_ripple_prev",	"$user$water_ripple2p")
	-- The baked field: its coverage channel is what makes a bank reflect.
	shader:dx10texture	("s_water_field",	"$user$water_field")

	shader:dx10sampler	("smp_nofilter")
	shader:dx10sampler	("smp_rtlinear")
end
