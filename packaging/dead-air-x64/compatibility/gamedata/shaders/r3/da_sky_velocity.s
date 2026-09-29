-- Motion vectors of the sky (phase_sky_velocity): a full-screen pass into $user$velocity that
-- keeps only the pixels the G-buffer never covered. No depth, no blending.

function normal (shader, t_base, t_second, t_detail)
	shader:begin	("da_fullscreen","da_sky_velocity")
			: fog	(false)
			: zb 	(false,false)
			: blend	(false, blend.one, blend.zero)
	shader:dx10texture	("s_position",	"$user$position")
end
