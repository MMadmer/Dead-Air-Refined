-- [DA] Duckweed-covered standing water. Reads the engine's SECOND optical profile (see
-- water_green.ps), so a level can carry a clear stream and a green pool at once.
--
-- The archive's version of this script never bound s_image, while the shader it selects reads
-- s_image for both the reflection march and the refracted background - so on the two levels that
-- use this material the surface was sampling whatever texture the previous draw had left in the
-- slot. A texture the .s script does not name is never bound: r_dx11Texture returns silently.
local tex_base                = "water\\water_water"
local tex_nmap                = "water\\water_normal"
local tex_dist                = "water\\water_dudv"
local tex_env0                = "$user$sky0"
local tex_env1                = "$user$sky1"

-- The duckweed mat itself, as the archive authored it.
local tex_leaves              = "water\\water_foam"

function normal                (shader, t_base, t_second, t_detail)
	shader	:begin		("water_soft","water_green")
    		:sorting	(2, false)
			:blend		(true,blend.srcalpha,blend.invsrcalpha)
			:zb			(true,false)
			:distort	(true)
			:fog		(true)

	shader:dx10texture	("s_base",		tex_base)
	shader:dx10texture	("s_nmap",		tex_nmap)
	shader:dx10texture	("s_env0",		tex_env0)
	shader:dx10texture	("s_env1",		tex_env1)
	shader:dx10texture	("s_position",	"$user$position")

	shader:dx10texture	("s_leaves",	tex_leaves)
	shader:dx10texture	("s_image",	"$user$ssr")	-- scene-grab RT for SSLR

	shader:dx10sampler	("smp_base")
	shader:dx10sampler	("smp_nofilter")
	shader:dx10sampler	("smp_rtlinear")
end

-- The screen-warp element is kept so the shader still has all five, but it no longer marks the
-- surface as distorting: the water shader now refracts its own background per pixel with the
-- real surface slope, and the warp pass was a second, uncoordinated refraction over the same
-- pixels. Turning it off here also drops a whole pass over every water surface.
function l_special        (shader, t_base, t_second, t_detail)
	shader	:begin                ("waterd_soft","waterd_soft")
			:sorting        (2, true)
			:blend                (true,blend.srcalpha,blend.invsrcalpha)
			:zb                (true,false)
			:fog                (false)
			:distort        (false)

	shader: dx10color_write_enable( true, true, true, false)

	shader:dx10texture	("s_base",		tex_base)
	shader:dx10texture	("s_distort",	tex_dist)
	shader:dx10texture	("s_position",	"$user$position")

	shader:dx10sampler	("smp_base")
	shader:dx10sampler	("smp_nofilter")
end
