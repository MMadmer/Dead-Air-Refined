-- models\selflight_det2: the self-lit detail shader the Anomaly-made hud models of the
-- animation module ask for (cigarettes, cigars). Dead Air has models\selflight_det only, and
-- a missing script logged an error and fell back to a stub. Same passes as selflight_det: the
-- R3 model path takes no detail texture, so the two names render identically.
function normal		(shader, t_base, t_second, t_detail)
	shader:begin	("deffer_model_flat","deffer_base_flat")
			: fog		(false)
			: emissive 	(true)
--	shader:sampler	("s_base")      :texture	(t_base)
	shader:dx10texture	("s_base",	t_base)
	shader:dx10sampler	("smp_base")
	shader:dx10stencil	( 	true, cmp_func.always, 
							255 , 127, 
							stencil_op.keep, stencil_op.replace, stencil_op.keep)
	shader:dx10stencil_ref	(1)
	shader: dx10color_write_enable( true, true, true, false)
end

function l_special	(shader, t_base, t_second, t_detail)
	shader:begin	("shadow_direct_model",	"accum_emissivel")
			: zb 		(true,false)
			: fog		(false)
			: emissive 	(true)
	shader: dx10color_write_enable( true, true, true, false)
end
