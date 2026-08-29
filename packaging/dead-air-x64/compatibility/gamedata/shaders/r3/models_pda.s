-- 3D PDA screen material (the Gunslinger original, adapted; docs/dead-air/pda-3d-port-plan.md).
-- The model's screen subset names this engine shader in its OGF; the body subset keeps an
-- ordinary model shader and goes deferred (sun, dynamic lights, hud shadow, rain).
-- The screen itself is a forward, sorted, self-lit overlay on top of the lit frame:
-- our combiner MULTIPLIES albedo by light, so binding the UI into the G-buffer would
-- square the image - forward is the correct equation here, and a lit LCD должен светиться
-- сам, а не чернеть ночью.
--   s_base = the model's own dead-screen texture (shown at heavy interference)
--   s_vp2  = $user$ui - the PDA dialog rasterized this frame (CRender::BeforeWorldRender)
--   s_load = the boot sequence (animated .seq)
-- distort(true) of the original is dropped: our mapDistort reads only E[4], the flag would
-- be inert at best and steals the element in a service pass at worst (plan R13).
function normal (shader, t_base, t_second, t_detail)
    shader:begin ("model_def_lplanes","model_pda_screen")
      : fog      (true)
      : zb       (true,false)
      : blend    (true,blend.srcalpha,blend.invsrcalpha)
      : aref     (true,0)
      : sorting  (2,true)
    shader:dx10texture ("s_base", t_base)
    shader:dx10texture ("s_vp2",  "$user$ui")
    shader:dx10texture ("s_load", "ui\\ui_pda_loadscreen")
    shader:dx10sampler ("smp_base")
    shader:dx10sampler ("smp_rtlinear")
end
