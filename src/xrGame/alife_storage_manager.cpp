////////////////////////////////////////////////////////////////////////////
//	Module 		: alife_storage_manager.cpp
//	Created 	: 25.12.2002
//  Modified 	: 12.05.2004
//	Author		: Dmitriy Iassenev
//	Description : ALife Simulator storage manager
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "alife_storage_manager.h"
#include "alife_simulator_header.h"
#include "alife_time_manager.h"
#include "alife_spawn_registry.h"
#include "alife_object_registry.h"
#include "alife_graph_registry.h"
#include "alife_group_registry.h"
#include "alife_registry_container.h"
#include "xrServer.h"
#include "Level.h"
#include "saved_game_wrapper.h"
#include "xrEngine/IGame_Persistent.h"
#include "xrCore/Threading/ThreadUtil.h"
#include "autosave_manager.h"
#include "UIGameCustom.h"
#include "xrUICore/Static/UIStatic.h"

#include <array>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

using namespace ALife;

extern string_path g_last_saved_game;

namespace
{
constexpr u32 saveSidecarMagic = 0x31564F53;
constexpr u32 saveSidecarVersion = 1;

struct AsyncSaveJob
{
    std::string saveName;
    std::string finalName;
    std::string customTempName;
    std::unique_ptr<CMemoryWriter> sourceStream;
    std::vector<u8> customData;
    u64 saveId{};
    bool customDataExpected{};
    bool customTempPrepared{};
    float captureMilliseconds{};
};

struct AsyncSaveCompletion
{
    std::string saveName;
    std::string finalName;
    u32 sourceSize{};
    u32 compressedSize{};
    float captureMilliseconds{};
    float backgroundMilliseconds{};
    bool succeeded{};
};

bool native_file_exists(pcstr path)
{
#if defined(XR_PLATFORM_WINDOWS)
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
#else
    return access(path, F_OK) == 0;
#endif
}

bool native_remove_file(pcstr path)
{
    if (!native_file_exists(path))
        return true;
#if defined(XR_PLATFORM_WINDOWS)
    return DeleteFileA(path) != FALSE;
#else
    return unlink(path) == 0;
#endif
}

bool native_replace_file(pcstr source, pcstr destination)
{
#if defined(XR_PLATFORM_WINDOWS)
    return MoveFileExA(source, destination, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
#else
    return rename(source, destination) == 0;
#endif
}

bool native_flush_file(pcstr path)
{
#if defined(XR_PLATFORM_WINDOWS)
    const HANDLE file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;

    const bool flushed = FlushFileBuffers(file) != FALSE;
    CloseHandle(file);
    return flushed;
#else
    const int file = open(path, O_RDONLY);
    if (file < 0)
        return false;

    const bool flushed = fsync(file) == 0;
    close(file);
    return flushed;
#endif
}

bool native_write_file(pcstr path, const void* data, size_t size)
{
#if defined(XR_PLATFORM_WINDOWS)
    const HANDLE file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;

    const auto* cursor = static_cast<const u8*>(data);
    size_t remaining = size;
    bool succeeded = true;
    while (remaining)
    {
        const DWORD blockSize = static_cast<DWORD>(std::min<size_t>(remaining, std::numeric_limits<DWORD>::max()));
        DWORD written = 0;
        if (!WriteFile(file, cursor, blockSize, &written, nullptr) || written != blockSize)
        {
            succeeded = false;
            break;
        }
        cursor += written;
        remaining -= written;
    }

    succeeded = succeeded && FlushFileBuffers(file) != FALSE;
    CloseHandle(file);
    return succeeded;
#else
    const int file = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (file < 0)
        return false;

    const auto* cursor = static_cast<const u8*>(data);
    size_t remaining = size;
    bool succeeded = true;
    while (remaining)
    {
        const ssize_t written = write(file, cursor, remaining);
        if (written <= 0)
        {
            succeeded = false;
            break;
        }
        cursor += written;
        remaining -= written;
    }

    succeeded = succeeded && fsync(file) == 0;
    close(file);
    return succeeded;
#endif
}

void refresh_save_index()
{
    FS.get_path("$game_saves$")->m_Flags.set(FS_Path::flNeedRescan, true);
    FS.m_Flags.set(CLocatorAPI::flNeedCheck, true);
    FS.rescan_pathes();
    FS.m_Flags.set(CLocatorAPI::flNeedCheck, false);
}

void make_companion_save_name(pcstr scopName, pcstr extensionName, string_path& companionName)
{
    xr_strcpy(companionName, scopName);
    if (char* extension = strrchr(companionName, '.'))
        xr_strcpy(extension, sizeof(companionName) - (extension - companionName), extensionName);
}

void make_custom_save_name(pcstr scopName, string_path& scocName)
{
    make_companion_save_name(scopName, ".scoc", scocName);
}

bool read_file_signature(pcstr path, u64& size, u32& checksum)
{
#if defined(XR_PLATFORM_WINDOWS)
    const HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;

    LARGE_INTEGER fileSize;
    if (!GetFileSizeEx(file, &fileSize) || fileSize.QuadPart < 0 ||
        static_cast<u64>(fileSize.QuadPart) > std::numeric_limits<u32>::max())
    {
        CloseHandle(file);
        return false;
    }

    size = static_cast<u64>(fileSize.QuadPart);
    std::vector<u8> contents(static_cast<size_t>(size));
    DWORD bytesRead = 0;
    const bool read = size == 0 ||
        (ReadFile(file, contents.data(), static_cast<DWORD>(size), &bytesRead, nullptr) &&
            bytesRead == static_cast<DWORD>(size));
    CloseHandle(file);
    if (!read)
        return false;
#else
    const int file = open(path, O_RDONLY);
    if (file < 0)
        return false;

    struct stat fileInfo;
    if (fstat(file, &fileInfo) != 0 || fileInfo.st_size < 0 ||
        static_cast<u64>(fileInfo.st_size) > std::numeric_limits<u32>::max())
    {
        close(file);
        return false;
    }

    size = static_cast<u64>(fileInfo.st_size);
    std::vector<u8> contents(static_cast<size_t>(size));
    size_t remaining = contents.size();
    u8* cursor = contents.data();
    while (remaining)
    {
        const ssize_t bytesRead = read(file, cursor, remaining);
        if (bytesRead <= 0)
        {
            close(file);
            return false;
        }
        cursor += bytesRead;
        remaining -= bytesRead;
    }
    close(file);
#endif

    checksum = crc32(contents.data(), static_cast<u32>(contents.size()));
    return true;
}

bool write_save_sidecar(pcstr scopName, bool customDataExpected, u64 saveId, u32 sourceSize, u32 compressedSize)
{
    u64 scopSize = 0;
    u32 scopChecksum = 0;
    if (!read_file_signature(scopName, scopSize, scopChecksum))
        return false;

    u64 scocSize = 0;
    u32 scocChecksum = 0;
    if (customDataExpected)
    {
        string_path scocName;
        make_custom_save_name(scopName, scocName);
        if (!read_file_signature(scocName, scocSize, scocChecksum))
            return false;
    }

    string_path sidecarName;
    make_companion_save_name(scopName, ".scov", sidecarName);
    string_path sidecarTemp;
    strconcat(sizeof(sidecarTemp), sidecarTemp, sidecarName, ".tmp");

    std::array<u8, 48> contents{};
    size_t offset = 0;
    const auto writeValue = [&contents, &offset](const auto& value)
    {
        memcpy(contents.data() + offset, &value, sizeof(value));
        offset += sizeof(value);
    };
    writeValue(saveSidecarMagic);
    writeValue(saveSidecarVersion);
    writeValue(saveId);
    writeValue(sourceSize);
    writeValue(compressedSize);
    writeValue(scopSize);
    writeValue(scopChecksum);
    writeValue(scocSize);
    writeValue(scocChecksum);

    if (!native_write_file(sidecarTemp, contents.data(), contents.size()) ||
        !native_replace_file(sidecarTemp, sidecarName))
    {
        native_remove_file(sidecarTemp);
        return false;
    }

    return true;
}

bool commit_save_pair(pcstr scopName, pcstr scopTemp, pcstr scocTemp, bool customDataExpected)
{
    string_path scopBackup;
    strconcat(sizeof(scopBackup), scopBackup, scopName, ".bak");

    string_path scocName;
    make_custom_save_name(scopName, scocName);
    string_path scocBackup;
    strconcat(sizeof(scocBackup), scocBackup, scocName, ".bak");

    if (!native_file_exists(scopTemp) || (customDataExpected && !native_file_exists(scocTemp)))
        return false;
    if (!native_flush_file(scopTemp) || (customDataExpected && !native_flush_file(scocTemp)))
        return false;

    const bool hadScop = native_file_exists(scopName);
    const bool hadScoc = native_file_exists(scocName);

    if (hadScop && !native_replace_file(scopName, scopBackup))
        return false;
    if (hadScoc && !native_replace_file(scocName, scocBackup))
    {
        if (hadScop)
            native_replace_file(scopBackup, scopName);
        return false;
    }

    if (!native_replace_file(scopTemp, scopName))
    {
        if (hadScop)
            native_replace_file(scopBackup, scopName);
        if (hadScoc)
            native_replace_file(scocBackup, scocName);
        return false;
    }

    if (customDataExpected && !native_replace_file(scocTemp, scocName))
    {
        native_remove_file(scopName);
        if (hadScop)
            native_replace_file(scopBackup, scopName);
        if (hadScoc)
            native_replace_file(scocBackup, scocName);
        return false;
    }

    return true;
}

AsyncSaveCompletion execute_save_job(AsyncSaveJob&& job)
{
    CTimer timer;
    timer.Start();

    AsyncSaveCompletion completion;
    completion.saveName = job.saveName;
    completion.finalName = job.finalName;
    const u8* sourceData = static_cast<const u8*>(job.sourceStream->pointer());
    const size_t sourceSize = job.sourceStream->tell();
    completion.sourceSize = static_cast<u32>(sourceSize);
    completion.captureMilliseconds = job.captureMilliseconds;

    const size_t compressionCapacity = rtc_csize(completion.sourceSize);
    std::vector<u8> compressedData(compressionCapacity);
    completion.compressedSize = static_cast<u32>(rtc_compress(
        compressedData.data(), compressedData.size(), sourceData, sourceSize));
    compressedData.resize(completion.compressedSize);

    std::vector<u8> validationData(sourceSize);
    const size_t decodedSize = rtc_decompress(
        validationData.data(), validationData.size(), compressedData.data(), compressedData.size());
    if (decodedSize != sourceSize || memcmp(validationData.data(), sourceData, sourceSize) != 0)
    {
        completion.backgroundMilliseconds = timer.GetElapsed_sec() * 1000.f;
        return completion;
    }

    std::vector<u8> saveFile(3 * sizeof(u32) + compressedData.size());
    const u32 marker = u32(-1);
    const u32 version = ALIFE_VERSION;
    memcpy(saveFile.data(), &marker, sizeof(marker));
    memcpy(saveFile.data() + sizeof(marker), &version, sizeof(version));
    memcpy(saveFile.data() + 2 * sizeof(u32), &completion.sourceSize, sizeof(completion.sourceSize));
    memcpy(saveFile.data() + 3 * sizeof(u32), compressedData.data(), compressedData.size());

    string_path saveIdText;
    xr_sprintf(saveIdText, "%016llx", static_cast<unsigned long long>(job.saveId));
    std::string scopTemp = job.finalName + "." + saveIdText + ".tmp";

    if (!native_write_file(scopTemp.c_str(), saveFile.data(), saveFile.size()) ||
        (job.customDataExpected && !job.customTempPrepared &&
            !native_write_file(job.customTempName.c_str(), job.customData.data(), job.customData.size())) ||
        !commit_save_pair(job.finalName.c_str(), scopTemp.c_str(), job.customTempName.c_str(),
            job.customDataExpected))
    {
        native_remove_file(scopTemp.c_str());
        if (job.customDataExpected)
            native_remove_file(job.customTempName.c_str());
        completion.backgroundMilliseconds = timer.GetElapsed_sec() * 1000.f;
        return completion;
    }

    write_save_sidecar(job.finalName.c_str(), job.customDataExpected, job.saveId,
        completion.sourceSize, completion.compressedSize);
    completion.backgroundMilliseconds = timer.GetElapsed_sec() * 1000.f;
    completion.succeeded = true;
    return completion;
}

class AsyncSaveCoordinator final
{
public:
    AsyncSaveCoordinator()
    {
        worker = Threading::RunThread("Async save writer", [this] { worker_main(); });
    }

    ~AsyncSaveCoordinator()
    {
        wait();
        {
            std::lock_guard lock(mutex);
            stopping = true;
        }
        workAvailable.notify_one();
        worker.join();
    }

    void enqueue(AsyncSaveJob&& job)
    {
        {
            std::lock_guard lock(mutex);
            jobs.emplace_back(std::move(job));
        }
        workAvailable.notify_one();
    }

    void wait()
    {
        std::unique_lock lock(mutex);
        idle.wait(lock, [this] { return jobs.empty() && !active; });
    }

    std::deque<AsyncSaveCompletion> take_completions()
    {
        std::lock_guard lock(mutex);
        std::deque<AsyncSaveCompletion> result;
        result.swap(completions);
        return result;
    }

private:
    void worker_main()
    {
        for (;;)
        {
            AsyncSaveJob job;
            {
                std::unique_lock lock(mutex);
                workAvailable.wait(lock, [this] { return stopping || !jobs.empty(); });
                if (stopping && jobs.empty())
                    return;

                job = std::move(jobs.front());
                jobs.pop_front();
                active = true;
            }

            AsyncSaveCompletion completion = execute_save_job(std::move(job));

            {
                std::lock_guard lock(mutex);
                completions.emplace_back(std::move(completion));
                active = false;
            }
            idle.notify_all();
        }
    }

private:
    std::mutex mutex;
    std::condition_variable workAvailable;
    std::condition_variable idle;
    std::deque<AsyncSaveJob> jobs;
    std::deque<AsyncSaveCompletion> completions;
    bool active{};
    bool stopping{};
    std::thread worker;
};

AsyncSaveCoordinator& async_save_coordinator()
{
    static AsyncSaveCoordinator coordinator;
    return coordinator;
}

CALifeStorageManager* activeStorageManager{};

void show_save_status(pcstr text)
{
    CUIGameCustom* gameUi = CurrentGameUI();
    if (!gameUi)
        return;

    const bool compatibilityMode = ClearSkyMode || ShadowOfChernobylMode;
    gameUi->RemoveCustomStatic("game_saved");
    StaticDrawableWrapper* message =
        gameUi->AddCustomStatic("game_saved", true, compatibilityMode ? 3.f : -1.f);
    if (message)
        message->wnd()->TextItemControl()->SetText(text);
}
}

struct SaveCaptureQueue
{
    struct Pending
    {
        AsyncSaveJob job;
        std::unique_ptr<CMemoryWriter> stream;
        std::vector<u8> registryData;
        CALifeSpawnRegistry::SaveState spawnState;
        CALifeObjectRegistry::SaveState objectState;
        float mainThreadMilliseconds{};
        float scriptMilliseconds{};
        float headerMilliseconds{};
        float spawnMilliseconds{};
        float objectMilliseconds{};
        float registryMilliseconds{};
        float initialFrameMilliseconds{};
        float maxSliceMilliseconds{};
        u32 sliceCount{};
        u32 queuedAt{};
        bool scriptPreparePending{};
        bool scriptEncodePending{};
        u8 phase{};
    };

    std::deque<std::unique_ptr<Pending>> pending;
};

struct SaveRequestQueue
{
    struct Pending
    {
        std::string saveName;
        u32 clientSaveStart{};
        bool updateName{};
    };

    std::deque<Pending> pending;
};

CALifeStorageManager::CALifeStorageManager(IPureServer* server, LPCSTR section) : inherited(server, section)
{
    m_section = section;
    xr_strcpy(m_save_name, "");
}

CALifeStorageManager::~CALifeStorageManager()
{
    wait_for_pending_saves();
    if (activeStorageManager == this)
        activeStorageManager = nullptr;
}

void CALifeStorageManager::process_async_save_completions()
{
    if (activeStorageManager)
    {
        constexpr float saveBudgetMilliseconds = 3.f;
        CTimer budgetTimer;
        budgetTimer.Start();
        activeStorageManager->process_save_requests(saveBudgetMilliseconds);
        const float remainingMilliseconds =
            saveBudgetMilliseconds - budgetTimer.GetElapsed_sec() * 1000.f;
        if (remainingMilliseconds > 0.f)
            activeStorageManager->process_save_captures(remainingMilliseconds);
    }

    auto completions = async_save_coordinator().take_completions();
    if (completions.empty())
        return;

    refresh_save_index();
    for (const auto& completion : completions)
    {
        if (!completion.succeeded)
        {
            Msg("! Asynchronous save transaction failed for '%s'; the previous save pair was preserved",
                completion.saveName.c_str());
            show_save_status(StringTable().translate("st_game_save_failed").c_str());
            continue;
        }

        Msg("* Game %s is successfully saved to file '%s' (capture %.3f ms, background %.3f ms)",
            completion.saveName.c_str(), completion.finalName.c_str(),
            completion.captureMilliseconds, completion.backgroundMilliseconds);

        std::string displayName = completion.saveName;
        const size_t extension = displayName.rfind('.');
        if (extension != std::string::npos)
            displayName.erase(extension);
        const std::string message =
            StringTable().translate("st_game_saved").c_str() + std::string(": ") + displayName;
        show_save_status(message.c_str());

        if (GEnv.ScriptEngine)
        {
            luabind::functor<void> afterSave;
            if (GEnv.ScriptEngine->functor("alife_storage_manager.CALifeStorageManager_save", afterSave))
                afterSave(completion.saveName.c_str());
        }
    }
}

void CALifeStorageManager::wait_for_pending_saves()
{
    while (activeStorageManager && activeStorageManager->m_save_requests &&
        !activeStorageManager->m_save_requests->pending.empty())
    {
        activeStorageManager->process_save_requests(flt_max);
    }
    while (activeStorageManager && activeStorageManager->m_save_captures &&
        !activeStorageManager->m_save_captures->pending.empty())
    {
        activeStorageManager->process_save_captures(flt_max);
    }
    async_save_coordinator().wait();
    process_async_save_completions();
}

void CALifeStorageManager::process_save_requests(float budgetMilliseconds)
{
    if (!m_save_requests || m_save_requests->pending.empty() || !g_pGameLevel)
        return;

    CTimer budgetTimer;
    budgetTimer.Start();
    while (!m_save_requests->pending.empty())
    {
        SaveRequestQueue::Pending& request = m_save_requests->pending.front();
        if (Level().ClientSaveStep(request.clientSaveStart, 16))
        {
            save(request.saveName.c_str(), request.updateName);
            m_save_requests->pending.pop_front();
        }

        if (budgetMilliseconds != flt_max &&
            budgetTimer.GetElapsed_sec() * 1000.f >= budgetMilliseconds)
        {
            return;
        }
    }
}

bool CALifeStorageManager::save_capture_active()
{
    return activeStorageManager && activeStorageManager->m_save_captures &&
        !activeStorageManager->m_save_captures->pending.empty();
}

void CALifeStorageManager::process_save_captures(float budgetMilliseconds)
{
    if (!m_save_captures)
        return;

    CTimer budgetTimer;
    budgetTimer.Start();
    while (!m_save_captures->pending.empty())
    {
        SaveCaptureQueue::Pending& capture = *m_save_captures->pending.front();
        CTimer sliceTimer;
        sliceTimer.Start();
        ++capture.sliceCount;

        if (capture.phase == 0)
        {
            bool completed = false;
            while (!completed)
            {
                CTimer scriptTimer;
                scriptTimer.Start();
                completed = true;
                luabind::functor<bool> prepareStep;
                if (GEnv.ScriptEngine->functor(
                        "alife_storage_manager.CALifeStorageManager_capture_prepare_step", prepareStep))
                {
                    completed = prepareStep(64);
                }
                const float scriptMilliseconds = scriptTimer.GetElapsed_sec() * 1000.f;
                capture.scriptMilliseconds += scriptMilliseconds;
                capture.mainThreadMilliseconds += scriptMilliseconds;

                if (!completed && budgetMilliseconds != flt_max &&
                    sliceTimer.GetElapsed_sec() * 1000.f >= budgetMilliseconds)
                {
                    capture.maxSliceMilliseconds =
                        std::max(capture.maxSliceMilliseconds, sliceTimer.GetElapsed_sec() * 1000.f);
                    return;
                }
            }
            capture.scriptPreparePending = false;
            capture.phase = 1;
        }

        if (capture.phase == 1 &&
            (budgetMilliseconds == flt_max || sliceTimer.GetElapsed_sec() * 1000.f < budgetMilliseconds))
        {
            CTimer scriptTimer;
            scriptTimer.Start();
            luabind::functor<luabind::object> encodeSave;
            if (GEnv.ScriptEngine->functor(
                    "alife_storage_manager.CALifeStorageManager_capture_encode", encodeSave))
            {
                const luabind::object snapshot = encodeSave();
                lua_State* lua = snapshot.interpreter();
                snapshot.push(lua);
                size_t customSize = 0;
                const char* customBytes = lua_tolstring(lua, -1, &customSize);
                if (customBytes && customSize)
                {
                    capture.job.customData.assign(customBytes, customBytes + customSize);
                    capture.job.customDataExpected = true;
                }
                lua_pop(lua, 1);
            }
            const float scriptMilliseconds = scriptTimer.GetElapsed_sec() * 1000.f;
            capture.scriptMilliseconds += scriptMilliseconds;
            capture.mainThreadMilliseconds += scriptMilliseconds;
            capture.scriptEncodePending = false;
            capture.phase = 2;
        }

        if (capture.phase == 2 &&
            (budgetMilliseconds == flt_max || sliceTimer.GetElapsed_sec() * 1000.f < budgetMilliseconds))
        {
            CTimer spawnTimer;
            spawnTimer.Start();
            spawns().begin_save(*capture.stream, capture.spawnState);
            const float spawnMilliseconds = spawnTimer.GetElapsed_sec() * 1000.f;
            capture.spawnMilliseconds += spawnMilliseconds;
            capture.mainThreadMilliseconds += spawnMilliseconds;
            capture.phase = 3;
        }

        if (capture.phase == 3 &&
            (budgetMilliseconds == flt_max || sliceTimer.GetElapsed_sec() * 1000.f < budgetMilliseconds))
        {
            const float elapsed = sliceTimer.GetElapsed_sec() * 1000.f;
            const float remaining =
                budgetMilliseconds == flt_max ? flt_max : std::max(0.01f, budgetMilliseconds - elapsed);
            CTimer spawnTimer;
            spawnTimer.Start();
            const bool completed = spawns().continue_save(*capture.stream, capture.spawnState, remaining);
            const float spawnMilliseconds = spawnTimer.GetElapsed_sec() * 1000.f;
            capture.spawnMilliseconds += spawnMilliseconds;
            capture.mainThreadMilliseconds += spawnMilliseconds;
            if (!completed)
            {
                capture.maxSliceMilliseconds =
                    std::max(capture.maxSliceMilliseconds, sliceTimer.GetElapsed_sec() * 1000.f);
                return;
            }
            capture.phase = 4;
        }

        if (capture.phase == 4 &&
            (budgetMilliseconds == flt_max || sliceTimer.GetElapsed_sec() * 1000.f < budgetMilliseconds))
        {
            CTimer objectTimer;
            objectTimer.Start();
            objects().begin_save(*capture.stream, capture.objectState);
            const float objectMilliseconds = objectTimer.GetElapsed_sec() * 1000.f;
            capture.objectMilliseconds += objectMilliseconds;
            capture.mainThreadMilliseconds += objectMilliseconds;
            capture.phase = 5;
        }

        bool completed = false;
        if (capture.phase == 5 &&
            (budgetMilliseconds == flt_max || sliceTimer.GetElapsed_sec() * 1000.f < budgetMilliseconds))
        {
            const float elapsed = sliceTimer.GetElapsed_sec() * 1000.f;
            const float remaining =
                budgetMilliseconds == flt_max ? flt_max : std::max(0.01f, budgetMilliseconds - elapsed);
            CTimer objectTimer;
            objectTimer.Start();
            completed = objects().continue_save(*capture.stream, capture.objectState, remaining);
            const float objectMilliseconds = objectTimer.GetElapsed_sec() * 1000.f;
            capture.objectMilliseconds += objectMilliseconds;
            capture.mainThreadMilliseconds += objectMilliseconds;
        }

        const float sliceMilliseconds = sliceTimer.GetElapsed_sec() * 1000.f;
        capture.maxSliceMilliseconds = std::max(capture.maxSliceMilliseconds, sliceMilliseconds);
        if (!completed)
            return;

        capture.stream->w(capture.registryData.data(), capture.registryData.size());
        capture.job.sourceStream = std::move(capture.stream);
        capture.job.captureMilliseconds = capture.mainThreadMilliseconds;

        Msg("* Game %s snapshot queued for asynchronous compression and commit (main-thread work %.3f ms)",
            capture.job.saveName.c_str(), capture.mainThreadMilliseconds);
        Msg("* Save snapshot stages: script %.3f ms, header %.3f ms, spawns %.3f ms, objects %.3f ms, registry %.3f ms",
            capture.scriptMilliseconds, capture.headerMilliseconds, capture.spawnMilliseconds,
            capture.objectMilliseconds, capture.registryMilliseconds);
        Msg("* Save frame budget: initial %.3f ms, max follow-up slice %.3f ms, %u slices, %.3f s wall time",
            capture.initialFrameMilliseconds, capture.maxSliceMilliseconds, capture.sliceCount,
            float(Device.TimerAsync() - capture.queuedAt) / 1000.f);

        async_save_coordinator().enqueue(std::move(capture.job));
        m_save_captures->pending.pop_front();

        if (budgetMilliseconds != flt_max && budgetTimer.GetElapsed_sec() * 1000.f >= budgetMilliseconds)
            return;
    }
}

void CALifeStorageManager::save(LPCSTR save_name_no_check, bool update_name)
{
    pcstr gameSaveExtension = SAVE_EXTENSION;
    if (ShadowOfChernobylMode || ClearSkyMode)
        gameSaveExtension = SAVE_EXTENSION_LEGACY;

    LPCSTR game_saves_path = FS.get_path("$game_saves$")->m_Path;

    string_path save_name;
    strncpy_s(save_name, sizeof(save_name), save_name_no_check,
        sizeof(save_name) - 5 - xr_strlen(gameSaveExtension) - xr_strlen(game_saves_path));

    xr_strcpy(g_last_saved_game, save_name);

    string_path saveBackup;
    xr_strcpy(saveBackup, m_save_name);
    if (save_name[0])
    {
        strconcat(sizeof(m_save_name), m_save_name, save_name, gameSaveExtension);
    }
    else
    {
        if (!xr_strlen(m_save_name))
        {
            Log("There is no file name specified!");
            return;
        }
    }

    const u64 saveId = static_cast<u64>(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    string32 saveIdText;
    xr_sprintf(saveIdText, "%016llx", static_cast<unsigned long long>(saveId));

    CTimer captureTimer;
    captureTimer.Start();
    CTimer stageTimer;
    stageTimer.Start();

    string_path finalName;
    FS.update_path(finalName, "$game_saves$", m_save_name);
    string_path customName;
    make_custom_save_name(finalName, customName);
    const std::string customTempName = std::string(customName) + "." + saveIdText + ".tmp";

    bool customDataExpected = false;
    bool customTempPrepared = false;
    bool scriptPreparePending = false;
    bool scriptEncodePending = false;
    std::vector<u8> customData;
    luabind::functor<bool> prepareBegin;
    luabind::functor<bool> prepareStep;
    luabind::functor<luabind::object> encodeSave;
    if (GEnv.ScriptEngine->functor(
            "alife_storage_manager.CALifeStorageManager_capture_prepare_begin", prepareBegin) &&
        GEnv.ScriptEngine->functor(
            "alife_storage_manager.CALifeStorageManager_capture_prepare_step", prepareStep) &&
        GEnv.ScriptEngine->functor(
            "alife_storage_manager.CALifeStorageManager_capture_encode", encodeSave) &&
        prepareBegin((pcstr)m_save_name))
    {
        scriptPreparePending = true;
        scriptEncodePending = true;
    }
    else
    {
        luabind::functor<bool> prepareSave;
        if (GEnv.ScriptEngine->functor(
                "alife_storage_manager.CALifeStorageManager_capture_prepare", prepareSave) &&
            GEnv.ScriptEngine->functor(
                "alife_storage_manager.CALifeStorageManager_capture_encode", encodeSave) &&
            prepareSave((pcstr)m_save_name))
        {
            scriptEncodePending = true;
        }
        else
        {
            luabind::functor<luabind::object> captureSave;
            if (GEnv.ScriptEngine->functor(
                    "alife_storage_manager.CALifeStorageManager_capture_save", captureSave))
            {
                const luabind::object snapshot = captureSave((pcstr)m_save_name);
                lua_State* lua = snapshot.interpreter();
                snapshot.push(lua);
                size_t customSize = 0;
                const char* customBytes = lua_tolstring(lua, -1, &customSize);
                if (customBytes && customSize)
                {
                    customData.assign(customBytes, customBytes + customSize);
                    customDataExpected = true;
                }
                lua_pop(lua, 1);
            }
            else
            {
                string_path customPreserved;
                strconcat(sizeof(customPreserved), customPreserved, customName, ".", saveIdText, ".preserved");
                bool hadCustomData = native_file_exists(customName);
                if (hadCustomData)
                    hadCustomData = native_replace_file(customName, customPreserved);

                luabind::functor<void> beforeSave;
                const bool called = GEnv.ScriptEngine->functor(
                    "alife_storage_manager.CALifeStorageManager_before_save", beforeSave);
                if (called)
                    beforeSave((pcstr)m_save_name, (pcstr)saveIdText);

                string_path legacyTempName;
                strconcat(sizeof(legacyTempName), legacyTempName, customName, ".tmp");
                if (native_file_exists(customTempName.c_str()))
                    customDataExpected = true;
                else if (native_file_exists(legacyTempName))
                    customDataExpected = native_replace_file(legacyTempName, customTempName.c_str());
                else if (native_file_exists(customName))
                    customDataExpected = native_replace_file(customName, customTempName.c_str());

                if (hadCustomData)
                    native_replace_file(customPreserved, customName);

                if (customDataExpected)
                {
                    u64 customSize = 0;
                    u32 customChecksum = 0;
                    if (!read_file_signature(customTempName.c_str(), customSize, customChecksum) || !customSize)
                    {
                        native_remove_file(customTempName.c_str());
                        customDataExpected = false;
                    }
                    else
                        customTempPrepared = true;
                }
            }
        }
    }
    const float scriptMilliseconds = stageTimer.GetElapsed_sec() * 1000.f;

    auto capture = std::make_unique<SaveCaptureQueue::Pending>();
    capture->stream = std::make_unique<CMemoryWriter>();
    capture->stream->reserve(std::max<size_t>(
        2 * 1024 * 1024, objects().objects().size() * 96));
    capture->job.saveName = m_save_name;
    capture->job.finalName = finalName;
    capture->job.customTempName = customTempName;
    capture->job.customData = std::move(customData);
    capture->job.saveId = saveId;
    capture->job.customDataExpected = customDataExpected;
    capture->job.customTempPrepared = customTempPrepared;
    capture->scriptMilliseconds = scriptMilliseconds;
    capture->scriptPreparePending = scriptPreparePending;
    capture->scriptEncodePending = scriptEncodePending;
    capture->phase = scriptPreparePending ? 0 : scriptEncodePending ? 1 : 2;

    stageTimer.Start();
    header().save(*capture->stream);
    time_manager().save(*capture->stream);
    capture->headerMilliseconds = stageTimer.GetElapsed_sec() * 1000.f;

    stageTimer.Start();
    CMemoryWriter registryStream;
    registry().save(registryStream);
    capture->registryMilliseconds = stageTimer.GetElapsed_sec() * 1000.f;
    capture->registryData.resize(registryStream.tell());
    memcpy(capture->registryData.data(), registryStream.pointer(), registryStream.tell());

    capture->initialFrameMilliseconds = captureTimer.GetElapsed_sec() * 1000.f;
    capture->mainThreadMilliseconds = capture->initialFrameMilliseconds;
    capture->queuedAt = Device.TimerAsync();
    if (!m_save_captures)
        m_save_captures = std::make_unique<SaveCaptureQueue>();
    m_save_captures->pending.emplace_back(std::move(capture));
    activeStorageManager = this;

    if (!update_name)
        xr_strcpy(m_save_name, saveBackup);
}

void CALifeStorageManager::load(void* buffer, const u32& buffer_size, LPCSTR file_name, u64 save_id)
{
    string32 saveIdText;
    xr_sprintf(saveIdText, "%016llx", static_cast<unsigned long long>(save_id));

    luabind::functor<void> funct;
    if (GEnv.ScriptEngine->functor("alife_storage_manager.CALifeStorageManager_load", funct))
        funct(file_name, (pcstr)saveIdText);

    IReader source(buffer, buffer_size);
    header().load(source);
    time_manager().load(source);
    spawns().load(source, file_name);
    graph().on_load();
    objects().load(source);

    VERIFY(can_register_objects());
    can_register_objects(false);

    for (auto& object : objects().objects())
    {
        ALife::_OBJECT_ID id = object.second->ID;
        object.second->ID = server().PerformIDgen(id);
        VERIFY(id == object.second->ID);
        register_object(object.second, false);
    }

    registry().load(source);

    can_register_objects(true);

    for (auto& object : objects().objects())
        object.second->on_register();

    if (!g_pGameLevel)
        return;

    Level().autosave_manager().on_game_loaded();

	//Neloreck: For consistency with before/after save callbacks.
    luabind::functor<void> funct2;
    if (GEnv.ScriptEngine->functor("alife_storage_manager.CALifeStorageManager_after_load", funct2))
        funct2(file_name);
	//-Neloreck
}

bool CALifeStorageManager::load(LPCSTR save_name_no_check)
{
    wait_for_pending_saves();

    pcstr gameSaveExtension = SAVE_EXTENSION;
    if (ShadowOfChernobylMode || ClearSkyMode)
        gameSaveExtension = SAVE_EXTENSION_LEGACY;

    LPCSTR game_saves_path = FS.get_path("$game_saves$")->m_Path;

    string_path save_name;
    strncpy_s(save_name, sizeof(save_name), save_name_no_check,
        sizeof(save_name) - 5 - xr_strlen(gameSaveExtension) - xr_strlen(game_saves_path));

    CTimer timer;
    timer.Start();

    string_path saveBackup;
    xr_strcpy(saveBackup, m_save_name);
    if (!save_name[0])
    {
        if (!xr_strlen(m_save_name))
        {
            Log("There is no file name specified!");
            return false;
        }
    }
    else
    {
        strconcat(sizeof(m_save_name), m_save_name, save_name, gameSaveExtension);
    }
    string_path file_name;
    FS.update_path(file_name, "$game_saves$", m_save_name);

    xr_strcpy(g_last_saved_game, save_name);
    xrDebug::SetBugReportFile(file_name);

    IReader* stream = FS.r_open(file_name);
    if (!stream)
    {
        Msg("* Cannot open saved game %s", file_name);
        xr_strcpy(m_save_name, saveBackup);
        return false;
    }

    constexpr pcstr mismatch = "Saved game version mismatch or saved game is corrupted";
    if (!CSavedGameWrapper::valid_saved_game(*stream))
    {
        Msg("! %s [%s]", mismatch, file_name);
        FS.r_close(stream);
        stream = nullptr;

        if (CSavedGameWrapper::valid_saved_game(save_name))
            stream = FS.r_open(file_name);
        if (!stream || !CSavedGameWrapper::valid_saved_game(*stream))
        {
            if (stream)
                FS.r_close(stream);
            xr_strcpy(m_save_name, saveBackup);
            return false;
        }
    }

    CSavedGameWrapper::SaveMetadata metadata;
    if (!CSavedGameWrapper::read_metadata(*stream, metadata))
    {
        FS.r_close(stream);
        xr_strcpy(m_save_name, saveBackup);
        return false;
    }

    string512 temp;
    strconcat(temp, StringTable().translate("st_loading_saved_game").c_str(),
        " \"", save_name, gameSaveExtension, "\"");

    g_pGamePersistent->LoadTitle(temp);

    stream->advance(sizeof(u32));
    const u32 source_count = static_cast<u32>(metadata.sourceSize);
    void* source_data = xr_malloc(source_count);
    const size_t decodedSize = rtc_decompress(source_data, source_count, stream->pointer(), metadata.compressedSize);
    FS.r_close(stream);
    if (decodedSize != source_count)
    {
        xr_free(source_data);
        xr_strcpy(m_save_name, saveBackup);
        return false;
    }

    unload();
    reload(m_section);

    load(source_data, source_count, file_name, metadata.saveId);
    xr_free(source_data);

    groups().on_after_game_load();

    VERIFY(graph().actor());

    Msg("* Game %s is successfully loaded from file '%s' (%.3fs)", save_name, file_name, timer.GetElapsed_sec());

    return (true);
}

void CALifeStorageManager::save(NET_Packet& net_packet)
{
    shared_str game_name;
    net_packet.r_stringZ(game_name);
    if (!m_save_requests)
        m_save_requests = std::make_unique<SaveRequestQueue>();
    m_save_requests->pending.push_back({game_name.c_str(), 0, !!net_packet.r_u8()});
    activeStorageManager = this;
}
