#include "stdafx.h"

#include "SoundRender_Core.h"
#include "SoundRender_Source.h"

CSoundRender_Source* CSoundRender_Core::i_create_source(pcstr name)
{
    // Optional dialogue sounds may omit the sound name.
    if (!name || !*name)
        return nullptr;

    // Dialogue sound names can use the full engine path capacity.
    string_path id;
    xr_strcpy(id, name);
    xr_strlwr(id);
    if (strext(id))
        * strext(id) = 0;

    {
        ScopeLock scope(&s_sources_lock);
        const auto it = s_sources.find(id);
        if (it != s_sources.end())
        {
            return it->second;
        }
    }

    // Load a _new one
    CSoundRender_Source source;
    if (source.load(id))
    {
        ScopeLock scope(&s_sources_lock);
        CSoundRender_Source* S = xr_new<CSoundRender_Source>(std::move(source));
        const auto [it, inserted] = s_sources.emplace(id, S);
        if (!inserted)
        {
            xr_delete(S);
            return it->second;
        }
        return S;
    }

    return nullptr;
}

void CSoundRender_Core::i_destroy_source(CSoundRender_Source* S)
{
    // No actual destroy at all
}

void CSoundRender_Core::i_create_all_sources()
{
    CTimer timer;
    timer.Start();

    FS_FileSet files;
    FS.file_list(files, "$game_sounds$", FS_ListFiles, "*.ogg");

    size_t sourcesBefore;
    {
        ScopeLock scope(&s_sources_lock);
        sourcesBefore = s_sources.size();
        s_sources.reserve(s_sources.size() + files.size());
    }

    for (const FS_File& file : files)
        i_create_source(file.name.c_str());

    size_t sourcesAfter;
    {
        ScopeLock scope(&s_sources_lock);
        sourcesAfter = s_sources.size();
    }

    Msg("* SOUND: precached %zu of %zu discovered sources in %.0f ms", sourcesAfter - sourcesBefore, files.size(),
        timer.GetElapsed_sec() * 1000.0f);
}
