function normal         (shader, t_base, t_second, t_detail)
        shader:begin    ("clouds","clouds")
                        : fog               (false)
-- [DA] Z-test returned to the r1/r2 state (the DX11 port shipped zb(false,false) with a
-- "TODO: check if this is ok" - it never was checked). With the depth prepass already laid
-- down, the old renderers clipped the cloud deck against mountains honestly; without the
-- test the deck draws behind ridgelines and reads as a flat card pasted over the horizon.
                        : zb                (true,false)
                        : sorting        	(3, true)
                        : blend             (true, blend.srcalpha,blend.invsrcalpha)

	shader:dx10texture	("s_clouds0", "null")
	shader:dx10texture	("s_clouds1", "null")
	shader:dx10texture	("s_tonemap", "$user$tonemap")

	shader:dx10sampler	("smp_base")
end
