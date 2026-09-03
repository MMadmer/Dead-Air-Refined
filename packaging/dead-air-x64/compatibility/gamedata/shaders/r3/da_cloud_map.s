-- The cloud deck field, rendered once per frame into $user$cloud_map (phase_cloud_map). What
-- it holds and who reads it is documented in da_clouds.h.

function normal (shader, t_base, t_second, t_detail)
	shader:begin	("da_fullscreen","da_cloud_map")
			: fog	(false)
			: zb 	(false,false)
			: blend	(false, blend.one, blend.zero)
end
