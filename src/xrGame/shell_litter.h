#pragma once

// Persistent brass on the ground. The ejected-shell PARTICLE is a flight animation that the
// asset kills on its first contact - so shells used to vanish the moment they touched the
// floor. Instead of fighting the particle asset, every drop also lands a small brass decal
// through the wallmark engine (which already knows budgets, distance culling and TTL):
// the flight is still the particle, what STAYS is the decal. Capped by r__shell_decals -
// wired to the graphics preset ladder, console = session override.

class ENGINE_API IGameObject;

namespace shell_litter
{
// Called from CShootingObject::OnShellDrop. eject_pos is the particle spawn point
// (world for NPC weapons; for HUD weapons it is hud-space, so the caller passes hud_mode
// and the landing ray starts near the camera instead).
void queue(const Fvector& eject_pos, bool hud_mode);
// Per-frame: lands due shells (the short delay stands in for the flight time).
void update();
} // namespace shell_litter
