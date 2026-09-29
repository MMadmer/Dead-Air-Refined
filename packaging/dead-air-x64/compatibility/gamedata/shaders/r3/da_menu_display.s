-- The menu's distortion (distort.s) over the display-size targets an upscaler leaves free:
-- the generic targets are render-sized while one runs, so the menu is drawn into
-- $user$post_display and its distortion mask into $user$upscaled.

function normal		(shader, t_base, t_second, t_detail)
	shader:begin	("stub_notransform_t","distort")
			: fog	(false)
			: zb 	(false,false)
	shader:dx10texture	("s_base", "$user$post_display")
	shader:dx10texture	("s_distort", "$user$upscaled")

	shader:dx10sampler	("smp_rtlinear");
end
