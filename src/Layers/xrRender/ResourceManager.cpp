// TextureManager.cpp: implementation of the CResourceManager class.
//
//////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#pragma hdrstop

#include "xrCore/Threading/ParallelForEach.hpp"

#include "ResourceManager.h"
#include "tss.h"
#include "Blender.h"
#include "Blender_Recorder.h"

#include <condition_variable>
#include <optional>

namespace xray::render::RENDER_NAMESPACE
{
#if defined(USE_DX11)
namespace texture_residency
{
using ExclusionMap = xr_flat_hash_map<const CTexture*, u8>;

std::optional<ExclusionMap>& deferred_upload_exclusions()
{
    static std::optional<ExclusionMap> exclusions;
    return exclusions;
}

void mark_requested(const CTexture* texture)
{
    if (auto& exclusions = deferred_upload_exclusions())
        exclusions->erase(texture);
}

void forget(const CTexture* texture)
{
    mark_requested(texture);
}

void exclude(const CTexture* texture)
{
    auto& exclusions = deferred_upload_exclusions();
    if (!exclusions)
    {
        exclusions.emplace();
        exclusions->reserve(2048);
    }
    exclusions->try_emplace(texture, 0);
}

bool is_excluded(const CTexture* texture)
{
    const auto& exclusions = deferred_upload_exclusions();
    return exclusions && exclusions->contains(texture);
}

void clear()
{
    deferred_upload_exclusions().reset();
}
}
#endif

namespace
{
class TextureUploadPool final
{
public:
    TextureUploadPool()
    {
        workers.reserve(maximumWorkers);
        for (size_t workerIndex = 0; workerIndex < maximumWorkers; ++workerIndex)
            workers.emplace_back(&TextureUploadPool::WorkerMain, this, workerIndex);
    }

    ~TextureUploadPool()
    {
        {
            std::lock_guard lock(mutex);
            stopping = true;
        }
        workAvailable.notify_all();

        for (auto& worker : workers)
            worker.join();
    }

    void Start(xr_vector<CTexture*>&& batch, size_t requestedWorkers)
    {
        std::unique_lock lock(mutex);
        VERIFY(!workersRemaining);

        textures = std::move(batch);
        workerLimit = std::min(requestedWorkers, maximumWorkers);
        workersRemaining = workerLimit;
        nextTexture.store(0, std::memory_order_relaxed);
        ++generation;
        workAvailable.notify_all();
    }

    void Wait()
    {
        std::unique_lock lock(mutex);
        if (!workersRemaining)
            return;
        completed.wait(lock, [this] { return !workersRemaining; });
        textures.clear();
    }

    bool IsRunning()
    {
        std::lock_guard lock(mutex);
        return workersRemaining != 0;
    }

    void Run(xr_vector<CTexture*>&& batch, size_t requestedWorkers)
    {
        Start(std::move(batch), requestedWorkers);
        Wait();
    }

private:
    void WorkerMain(size_t workerIndex)
    {
        size_t observedGeneration{};
        for (;;)
        {
            std::unique_lock lock(mutex);
            workAvailable.wait(lock, [this, observedGeneration]
            {
                return stopping || generation != observedGeneration;
            });

            if (stopping)
                return;

            observedGeneration = generation;
            if (workerIndex >= workerLimit)
                continue;

            const auto* batch = &textures;
            lock.unlock();

            for (size_t index = nextTexture.fetch_add(1, std::memory_order_relaxed);
                 index < batch->size();
                 index = nextTexture.fetch_add(1, std::memory_order_relaxed))
            {
                (*batch)[index]->Load();
            }

            lock.lock();
            if (!--workersRemaining)
                completed.notify_one();
        }
    }

private:
    static constexpr size_t maximumWorkers = 4;

    xr_vector<std::thread> workers;
    std::mutex mutex;
    std::condition_variable workAvailable;
    std::condition_variable completed;
    xr_vector<CTexture*> textures;
    std::atomic_size_t nextTexture{};
    size_t workerLimit{};
    size_t workersRemaining{};
    size_t generation{};
    bool stopping{};
};

TextureUploadPool*& texture_upload_pool()
{
    static TextureUploadPool* pool{};
    return pool;
}

size_t texture_upload_worker_count(size_t textureCount)
{
    constexpr size_t taskMemoryBudget = 128 * 1024 * 1024;
    constexpr size_t maximumLoaderBudget = 512 * 1024 * 1024;
    const auto memory = Memory.statistics();
    const size_t availableBudget = std::min(memory.largestFreeBlock / 8, maximumLoaderBudget);
    const size_t budgetedWorkers = std::clamp(availableBudget / taskMemoryBudget, size_t{2}, size_t{4});
    return std::min(budgetedWorkers, textureCount);
}

#if defined(USE_DX11)
xr_vector<CTexture*> collect_unloaded_textures(const CResourceManager::map_Texture& resources)
{
    xr_vector<CTexture*> textures;
    textures.reserve(resources.size());
    for (const auto& [name, texture] : resources)
    {
        UNUSED(name);
        // Unloaded wrappers from prior levels stay cold until a new texture request revives them.
        if (!texture->flags.bLoaded && !texture_residency::is_excluded(texture))
            textures.push_back(texture);
    }
    return textures;
}
#endif
}

//	Already defined in Texture.cpp
void fix_texture_name(pstr fn);
/*
void fix_texture_name(LPSTR fn)
{
    LPSTR _ext = strext(fn);
    if (_ext &&
        (0==xr_stricmp(_ext, ".tga") ||
        0==xr_stricmp(_ext, ".dds") ||
        0==xr_stricmp(_ext, ".bmp") ||
        0==xr_stricmp(_ext, ".ogm")))
        *_ext = 0;
}
*/

//--------------------------------------------------------------------------------------------------------------
IBlender* CResourceManager::_GetBlender(LPCSTR Name)
{
    R_ASSERT(Name && Name[0]);

    pstr N = pstr(Name);
    map_Blender::iterator I = m_blenders.find(N);

    if (I == m_blenders.end())
    {
        Msg("! Shader '%s' not found in library.", Name);
        return nullptr;
    }

    return I->second;
}

IBlender* CResourceManager::_FindBlender(LPCSTR Name)
{
    if (!(Name && Name[0]))
        return nullptr;

    pstr N = pstr(Name);
    map_Blender::iterator I = m_blenders.find(N);
    if (I == m_blenders.end())
        return nullptr;
    else
        return I->second;
}

void CResourceManager::ED_UpdateBlender(LPCSTR Name, IBlender* data)
{
    pstr N = pstr(Name);
    map_Blender::iterator I = m_blenders.find(N);
    if (I != m_blenders.end())
    {
        R_ASSERT(data->getDescription().CLS == I->second->getDescription().CLS);
        xr_delete(I->second);
        I->second = data;
    }
    else
    {
        m_blenders.emplace(xr_strdup(Name), data);
    }
}

//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////
void CResourceManager::_ParseList(sh_list& dest, LPCSTR names)
{
    if (nullptr == names || 0 == names[0])
        names = "$null";

    dest.clear();
    char* P = (char*)names;
    svector<char, 128> N;

    while (*P)
    {
        if (*P == ',')
        {
            // flush
            N.push_back(0);
            xr_strlwr(N.begin());

            fix_texture_name(N.begin());
            //. andy			if (strext(N.begin())) *strext(N.begin())=0;
            dest.push_back(N.begin());
            N.clear();
        }
        else
        {
            N.push_back(*P);
        }
        P++;
    }
    if (N.size())
    {
        // flush
        N.push_back(0);
        xr_strlwr(N.begin());

        fix_texture_name(N.begin());
        //. andy		if (strext(N.begin())) *strext(N.begin())=0;
        dest.push_back(N.begin());
    }
}

ShaderElement* CResourceManager::_CreateElement(ShaderElement&& S)
{
    if (S.passes.empty())
        return nullptr;

    // Search equal in shaders array
    for (ShaderElement* elem : v_elements)
        if (S.equal(*elem))
            return elem;

    // Create _new_ entry
    ShaderElement* N = v_elements.emplace_back(xr_new<ShaderElement>(std::move(S)));
    N->dwFlags |= xr_resource_flagged::RF_REGISTERED;
    return N;
}

void CResourceManager::_DeleteElement(const ShaderElement* S)
{
    if (0 == (S->dwFlags & xr_resource_flagged::RF_REGISTERED))
        return;
    if (reclaim(v_elements, S))
        return;
    Msg("! ERROR: Failed to find compiled 'shader-element'");
}

Shader* CResourceManager::_cpp_Create(
    IBlender* B, LPCSTR s_shader, LPCSTR s_textures, LPCSTR s_constants, LPCSTR s_matrices)
{
    CBlender_Compile C;
    Shader S;

    //.
    // if (strstr(s_shader,"transparent"))	__asm int 3;

    // Access to template
    C.BT = B;
    C.bFFP = RImplementation.o.ffp;
    C.bDetail = FALSE;
#ifdef _EDITOR
    if (!C.BT)
    {
        ELog.Msg(mtError, "Can't find shader '%s'", s_shader);
        return 0;
    }
    C.bFFP = true;
#else
    UNUSED(s_shader);
#endif

    // Parse names
    _ParseList(C.L_textures, s_textures);
    _ParseList(C.L_constants, s_constants);
    _ParseList(C.L_matrices, s_matrices);

    // Compile element	(LOD0 - HQ)
    {
        C.iElement = SE_R1_NORMAL_HQ;
        C.bDetail = m_textures_description.GetDetailTexture(C.L_textures[0], C.detail_texture, C.detail_scaler);
        ShaderElement E;
        C._cpp_Compile(&E);
        S.E[SE_R1_NORMAL_HQ] = _CreateElement(std::move(E));
    }

    // Compile element	(LOD1)
    {
        C.iElement = SE_R1_NORMAL_LQ;
        C.bDetail = m_textures_description.GetDetailTexture(C.L_textures[0], C.detail_texture, C.detail_scaler);
        ShaderElement E;
        C._cpp_Compile(&E);
        S.E[SE_R1_NORMAL_LQ] = _CreateElement(std::move(E));
    }

    // Compile element
    {
        C.iElement = SE_R1_LPOINT;
        C.bDetail = m_textures_description.GetDetailTexture(C.L_textures[0], C.detail_texture, C.detail_scaler);
        ShaderElement E;
        C._cpp_Compile(&E);
        S.E[SE_R1_LPOINT] = _CreateElement(std::move(E));
    }

    // Compile element
    {
        C.iElement = SE_R1_LSPOT;
        C.bDetail = m_textures_description.GetDetailTexture(C.L_textures[0], C.detail_texture, C.detail_scaler);
        ShaderElement E;
        C._cpp_Compile(&E);
        S.E[SE_R1_LSPOT] = _CreateElement(std::move(E));
    }

    // Compile element
    {
        C.iElement = SE_R1_LMODELS;
        C.bDetail = TRUE; //.$$$ HACK :)
        ShaderElement E;
        C._cpp_Compile(&E);
        S.E[SE_R1_LMODELS] = _CreateElement(std::move(E));
    }

    // Compile element
    {
        C.iElement = 5;
        C.bDetail = FALSE;
        ShaderElement E;
        C._cpp_Compile(&E);
        S.E[5] = _CreateElement(std::move(E));
    }

    // Search equal in shaders array
    for (u32 it = 0; it < v_shaders.size(); it++)
        if (S.equal(v_shaders[it]))
            return v_shaders[it];

    // Create _new_ entry
    Shader* N = v_shaders.emplace_back(xr_new<Shader>(std::move(S)));
    N->dwFlags |= xr_resource_flagged::RF_REGISTERED;
    return N;
}

Shader* CResourceManager::_cpp_Create(LPCSTR s_shader, LPCSTR s_textures, LPCSTR s_constants, LPCSTR s_matrices)
{
    if (!GEnv.isDedicatedServer)
    {
        IBlender* pBlender = _GetBlender(s_shader ? s_shader : "null");
        if (!pBlender)
            return nullptr;
        return _cpp_Create(pBlender, s_shader, s_textures, s_constants, s_matrices);
    }
    return nullptr;
}

IReader* open_shader(pcstr shader)
{
    string_path shaderPath;

    FS.update_path(shaderPath, "$game_shaders$", RImplementation.getShaderPath());
    xr_strcat(shaderPath, shader);

    return FS.r_open(shaderPath);
}

void CResourceManager::CompatibilityCheck()
{
    ZoneScoped;

    // Check Shoker HQ Geometry Fix support
    {
        IReader* skinh = open_shader("skin.h");
        R_ASSERT3(skinh, "Can't open shader", "skin.h");
        xr_string str(static_cast<pcstr>(skinh->pointer()), skinh->length());

        bool hq_skinning = true;
        // search for (12.f / 32768.f)
        const auto check = [&](cpcstr searchBegin, cpcstr searchEnd) -> bool
        {
            cpcstr begin = strstr(str.c_str(), searchBegin);
            if (!begin)
                return false;

            cpcstr end = strstr(begin, searchEnd);
            if (!end)
                return false;

            str.assign(begin, end);
            pcstr ptr = str.data();

            if ((ptr = strstr(ptr, "12.")))     // 12.f or 12.0
            {
                if ((ptr = strstr(ptr, "/")))   // /
                    if (strstr(ptr, "32768."))  // 32768.f or 32768.0
                    {
                        hq_skinning = false;    // found
                        return true;
                    }
            }
            return false;
        };
        if (!check("u_position", "sbones_array"))
        {
            check("skinning_pos", "skinning_0");
        }
        RImplementation.m_hq_skinning = hq_skinning;
        FS.r_close(skinh);
    }
}

Shader* CResourceManager::Create(IBlender* B, LPCSTR s_shader, LPCSTR s_textures, LPCSTR s_constants, LPCSTR s_matrices)
{
    if (GEnv.isDedicatedServer)
        return nullptr;

    return _cpp_Create(B, s_shader, s_textures, s_constants, s_matrices);
}

Shader* CResourceManager::Create(LPCSTR s_shader, LPCSTR s_textures, LPCSTR s_constants, LPCSTR s_matrices)
{
    if (!GEnv.isDedicatedServer)
    {
#if defined(USE_DX9)
        const bool useCppBlender = RImplementation.o.ffp && _GetBlender(s_shader);
        if (!useCppBlender && _lua_HasShader(s_shader))
            return _lua_Create(s_shader, s_textures);
        return _cpp_Create(s_shader, s_textures, s_constants, s_matrices);
#else // TODO: DX11: When all shaders are ready switch to common path
        if (_lua_HasShader(s_shader))
            return _lua_Create(s_shader, s_textures);
        else
        {
            Shader* pShader = _cpp_Create(s_shader, s_textures, s_constants, s_matrices);
            if (pShader)
                return pShader;
            else
            {
                if (_lua_HasShader("stub_default"))
                    return _lua_Create("stub_default", s_textures);
                else
                {
                    FATAL("Can't find stub_default.s");
                    return 0;
                }
            }
        }
#endif
    }
    return nullptr;
}

void CResourceManager::Delete(const Shader* S)
{
    if (0 == (S->dwFlags & xr_resource_flagged::RF_REGISTERED))
        return;
    if (reclaim(v_shaders, S))
        return;
    Msg("! ERROR: Failed to find complete shader");
}

void CResourceManager::BeginDeferredUpload()
{
    if (!Device.b_is_Ready)
        return;

#if defined(USE_DX11)
    auto*& uploadPool = texture_upload_pool();
    if (!uploadPool)
        uploadPool = xr_new<TextureUploadPool>();
    if (uploadPool->IsRunning())
        return;

    auto textures = collect_unloaded_textures(m_textures);
    if (textures.empty())
        return;

    const size_t workerCount = texture_upload_worker_count(textures.size());
    Msg("* Early texture upload: %zu textures, %zu workers", textures.size(), workerCount);
    uploadPool->Start(std::move(textures), workerCount);
#endif
}

void CResourceManager::DeferredUpload()
{
    if (!Device.b_is_Ready)
        return;

    ZoneScoped;

#if defined(USE_DX11)
    auto*& uploadPool = texture_upload_pool();
    if (!uploadPool)
        uploadPool = xr_new<TextureUploadPool>();
    uploadPool->Wait();

    auto textures = collect_unloaded_textures(m_textures);
    if (textures.empty())
        return;

    const size_t workerCount = texture_upload_worker_count(textures.size());
    Msg("* Final texture upload: %zu textures, %zu workers", textures.size(), workerCount);
    uploadPool->Run(std::move(textures), workerCount);
#elif defined(USE_OGL) // XXX: OGL: Set additional contexts for all worker threads?
    for (auto& texture : m_textures)
        texture.second->Load();
#else
#   error No graphics API selected or enabled!
#endif
}

void CResourceManager::ShutdownTextureUploadPool()
{
#if defined(USE_DX11)
    auto*& uploadPool = texture_upload_pool();
    if (uploadPool)
        uploadPool->Wait();
    xr_delete(uploadPool);
#endif
}

void CResourceManager::DeferredUnload()
{
    if (!Device.b_is_Ready)
        return;

    ZoneScoped;

#if defined(USE_DX11)
    auto*& uploadPool = texture_upload_pool();
    if (uploadPool)
        uploadPool->Wait();
    for (auto& texture : m_textures)
        texture.second->Unload();
#elif defined(USE_OGL) // XXX: OGL: Set additional contexts for all worker threads?
    for (auto& texture : m_textures)
        texture.second->Unload();
#else
#   error No graphics API selected or enabled!
#endif
}

void CResourceManager::BeginLevelTextureTracking()
{
#if defined(USE_DX11)
    m_level_persistent_textures.clear();

    for (const auto& texture : m_textures)
    {
        if (texture.second->flags.bLoaded)
            m_level_persistent_textures.emplace(texture.first);
    }
#endif
}

void CResourceManager::UnloadLevelTextures()
{
#if defined(USE_DX11)
    auto*& uploadPool = texture_upload_pool();
    if (uploadPool)
        uploadPool->Wait();

    u32 unloaded_count = 0;
    u64 unloaded_memory = 0;

    for (const auto& texture : m_textures)
    {
        CTexture* resource = texture.second;
        const bool was_loaded_before_level = m_level_persistent_textures.find(texture.first) !=
            m_level_persistent_textures.end();
        if (was_loaded_before_level)
            continue;

        texture_residency::exclude(resource);
        if (!resource->flags.bLoaded)
            continue;

        unloaded_memory += resource->flags.MemoryUsage;
        resource->Unload();
        ++unloaded_count;
    }

    m_level_persistent_textures.clear();
    Msg("* Level texture unload: %u textures, %llu MiB released", unloaded_count, unloaded_memory / 1048576);
#endif
}

#ifdef _EDITOR
void CResourceManager::ED_UpdateTextures(AStringVec* names)
{
    // 1. Unload
    if (names)
    {
        for (u32 nid = 0; nid < names->size(); nid++)
        {
            auto I = m_textures.find((*names)[nid].c_str());
            if (I != m_textures.end())
                I->second->Unload();
        }
    }
    else
    {
        for (auto t = m_textures.begin(); t != m_textures.end(); t++)
            t->second->Unload();
    }

    // 2. Load
    // DeferredUpload	();
}
#endif

void CResourceManager::_GetMemoryUsage(u32& m_base, u32& c_base, u32& m_lmaps, u32& c_lmaps)
{
    m_base = c_base = m_lmaps = c_lmaps = 0;

    map_Texture::iterator I = m_textures.begin();
    map_Texture::iterator E = m_textures.end();
    for (; I != E; ++I)
    {
        u32 m = I->second->flags.MemoryUsage;
        if (strstr(I->first, "lmap"))
        {
            c_lmaps++;
            m_lmaps += m;
        }
        else
        {
            c_base++;
            m_base += m;
        }
    }
}
void CResourceManager::_DumpMemoryUsage()
{
    xr_multimap<u32, std::pair<u32, shared_str>> mtex;

    // sort
    {
        map_Texture::iterator I = m_textures.begin();
        map_Texture::iterator E = m_textures.end();
        for (; I != E; ++I)
        {
            u32 m = I->second->flags.MemoryUsage;
            shared_str n = I->second->cName;
            mtex.emplace(m, std::make_pair(I->second->ref_count.load(), n));
        }
    }

    // dump
    {
        xr_multimap<u32, std::pair<u32, shared_str>>::iterator I = mtex.begin();
        xr_multimap<u32, std::pair<u32, shared_str>>::iterator E = mtex.end();
        for (; I != E; ++I)
            Msg("* %4.1f : [%4d] %s", float(I->first) / 1024.f, I->second.first, I->second.second.c_str());
    }
}

void CResourceManager::Evict()
{
}
/*
BOOL	CResourceManager::_GetDetailTexture(LPCSTR Name,LPCSTR& T, R_constant_setup* &CS)
{
    LPSTR N = LPSTR(Name);
    map_TD::iterator I = m_td.find	(N);
    if (I!=m_td.end())
    {
        T	= I->second.T;
        CS	= I->second.cs;
        return TRUE;
    } else {
        return FALSE;
    }
}*/
} // namespace xray::render::RENDER_NAMESPACE
