-- Motion vectors of the water surface (phase_water_velocity): the forward water meshes redrawn
-- into $user$velocity, depth-tested less-equal against the scene (the water writes no depth, so
-- this keeps exactly the pixels the water covers), no depth write, no blending. Two-sided like
-- the water itself (1 is D3DCULL_NONE: the lua API exports no cull enum).

function normal (shader, t_base, t_second, t_detail)
	shader:begin	("da_water_velocity","da_water_velocity")
			: fog	(false)
			: zb 	(true,false)
			: blend	(false, blend.one, blend.zero)
			: dx10cullmode	(1)
end
