// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#pragma once

// The network mark (docs/dead-air/MOD_CATALOG.md 10.4): whether a module's code works with the
// network. It is worked out from the module's own files - nobody states it -, shown where players
// pick and manage modules, and it is what the game's network API for scripts answers by.

#include "xms_core.h"

#include <string_view>

namespace XMS::NetMark
{
// Whether Lua text works with the network: the one rule, shared with the mod catalog and with
// whatever builds modules. `line` gets the first line that does, from 1.
XRCORE_API bool TextUsesNetwork(std::string_view text, u32* line = nullptr);

// Whether a file of a module is code the rule reads, by its name.
XRCORE_API bool CodeFile(std::string_view name);

// Whether the module's code does. Its code files are read once per run, at the first question,
// and the answer is kept; `where` gets "<file>:<line>" of the first use.
XRCORE_API bool ModuleUsesNetwork(const Module& module, xr_string* where = nullptr);

// Whether any enabled module does.
XRCORE_API bool AnyEnabledModuleUsesNetwork();
} // namespace XMS::NetMark
