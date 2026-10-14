// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#pragma once

// Small documents the game takes from the mod's repository at each start (the branch `remote` of
// MMadmer/Dead-Air-Refined, tools/remote/remote_config.py publishes them): the menu's community panel
// (HTML) and the addresses of the mod's services. They lie encrypted on GitHub and in the game's folder
// (appdata\remote), and only the game reads them; the copy the last start kept serves until a newer one
// comes and while GitHub is out of reach. A build without the key (RemoteConfigSecrets.local.h) reads none.
namespace RemoteConfig
{
enum class Document : u8
{
    Menu, // menu.dar: the community panel, HTML
    Services, // services.dar: "key = value" lines
    Count
};

// The kept copies, then GitHub in the background. Once; Stop waits for the background.
void Start();
void Stop();

// The document's text (UTF-8), empty when there is none; `revision` changes whenever another one comes.
xr_string Text(Document document, u32* revision = nullptr);
// the revision alone, without a copy of the text
u32 Revision(Document document);

// A value of the services document, empty when it says nothing of `key`.
xr_string Service(pcstr key);
} // namespace RemoteConfig
