-- One fixed step of the water standing on the actor's visor (phase_visor_drops). What it
-- solves, and why a drop on glass is not a pattern: da_visor_drops.ps.

function normal (shader, t_base, t_second, t_detail)
	-- da_fullscreen, not one of the stock stubs: those declare a vertex layout that does not
	-- match the FVF::F_TL geometry these passes are drawn with, and DirectX drops a mismatched
	-- draw in silence - no error, no picture, the target simply keeps what it held.
	shader:begin	("da_fullscreen","da_visor_drops")
			: fog	(false)
			: zb 	(false,false)
			-- The step REPLACES the state; there is nothing to blend with.
			: blend	(false, blend.one, blend.zero)

	-- The state one step ago. The pass writes into the scratch target and the result is copied
	-- back into this name, so it is never the target being written.
	shader:dx10texture	("s_visor_prev",	"$user$visor_drops0")

	shader:dx10sampler	("smp_nofilter")
	shader:dx10sampler	("smp_rtlinear")
end
