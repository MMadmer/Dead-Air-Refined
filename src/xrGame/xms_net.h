// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#pragma once

// The network for a module's scripts: xms.http (docs/dead-air/MODDING.md, "The network").
// Requests to public https servers only, off the frame, with every size bounded; the answer comes
// back to the script on the game's own thread. The game answers only while an enabled module
// carries the network mark (XMS::NetMark), so what players are told is what can happen.

struct lua_State;

namespace XmsNet
{
// The natives the xms bootstrap builds xms.http from.
void Register(lua_State* L);

// The game's thread, every frame: hands finished requests to the scripts that asked.
void Pump();

// Whether `url` is an address a script may ask: https, a public host by its name, no user part.
bool PublicAddress(pcstr url, xr_string& why);
} // namespace XmsNet
