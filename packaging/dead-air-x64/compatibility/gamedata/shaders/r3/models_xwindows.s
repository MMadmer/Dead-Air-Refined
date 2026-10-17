function l_special  (shader, t_base, t_second, t_detail)
  shader:begin  ("model_distort4glass","particle_distort")
      : sorting   (3, true)
      : blend     (true,blend.srcalpha,blend.invsrcalpha)
      : zb        (true,false)
      : fog       (false)
      : distort   (true)
--  shader:sampler  ("s_base")      :texture  (t_base)
--  shader:sampler  ("s_distort")   :texture  ("pfx\\pfx_dist_glass")
	shader: dx10texture ("s_base", t_base)
	shader: dx10texture ("s_distort", "pfx\\pfx_dist_glass") --:texture  (t_base) -- ("pfx\\pfx_dist_glass2")
	shader: dx10sampler ("smp_linear")
end

-- The glass itself (model_env_lq.ps): the weather's sky through Fresnel, the sun's glint and the dirt lit like the
-- scene, through its exposure. The stock cube stays bound for r__particle_light 0.
function normal   (shader, t_base, t_second, t_detail)
  shader:begin    ("model_env_lq","model_env_lq")
      : fog       (true)
      : zb        (true,false)
      : blend     (true,blend.srcalpha,blend.invsrcalpha)
      : aref      (true,0)
      : sorting   (3,true)
	shader: dx10texture ("s_base", t_base)
	shader:	dx10texture ("s_env", "sky\\sky_5_cube")
	shader:	dx10texture ("sky_s0", "$user$sky0")
	shader:	dx10texture ("sky_s1", "$user$sky1")
	-- The light the glass takes (da_particle_light.h): the froxel volume's sun term, the exposure, the irradiance cubes.
	shader:	dx10texture ("s_froxel_cur", "$user$da_froxel_cur")
	shader:	dx10texture ("s_tonemap", "$user$tonemap")
	shader:	dx10texture ("env_s0", "$user$env_s0")
	shader:	dx10texture ("env_s1", "$user$env_s1")

	shader: dx10sampler ("smp_base")
	shader: dx10sampler ("smp_rtlinear")
end
