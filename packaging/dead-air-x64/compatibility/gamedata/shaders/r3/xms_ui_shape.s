-- XMS UI shapes (xrUICore UIShape): computed per pixel in xms_ui_shape.ps, premultiplied over
-- whatever the UI drew before. The texture the UI hands over is not sampled.
function normal		(shader, t_base, t_second, t_detail)
	shader:begin	("stub_notransform_t","xms_ui_shape")
			: fog		(false)
			: zb 		(false,false)
			: blend		(true,blend.one,blend.invsrcalpha)
end
