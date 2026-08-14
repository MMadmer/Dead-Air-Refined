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
#include "GameObject.h"
#include "saved_game_wrapper.h"
#include "save_extension_container.h"
#include "save_extension_gameplay.h"
#include "xms_game.h"
#include "save_transaction_marker.h"
#include "xrEngine/IGame_Persistent.h"
#include "xrCore/Threading/ThreadUtil.h"
#include "xrCore/Debug/CrashReport.h"
#include "autosave_manager.h"
#include "UIGameCustom.h"
#include "xrUICore/Static/UIStatic.h"

#include <array>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

using namespace ALife;

extern string_path g_last_saved_game;

namespace
{
u64 current_thread_cycles()
{
#if defined(XR_PLATFORM_WINDOWS)
    u64 cycles{};
    QueryThreadCycleTime(GetCurrentThread(), &cycles);
    return cycles;
#else
    return 0;
#endif
}

struct ThreadCpuTime
{
    u64 kernel100ns{};
    u64 user100ns{};
};

ThreadCpuTime current_thread_cpu_time()
{
#if defined(XR_PLATFORM_WINDOWS)
    FILETIME creationTime;
    FILETIME exitTime;
    FILETIME kernelTime;
    FILETIME userTime;
    if (!GetThreadTimes(GetCurrentThread(), &creationTime, &exitTime, &kernelTime, &userTime))
        return {};

    ULARGE_INTEGER kernelValue;
    kernelValue.LowPart = kernelTime.dwLowDateTime;
    kernelValue.HighPart = kernelTime.dwHighDateTime;
    ULARGE_INTEGER userValue;
    userValue.LowPart = userTime.dwLowDateTime;
    userValue.HighPart = userTime.dwHighDateTime;
    return {kernelValue.QuadPart, userValue.QuadPart};
#else
    return {};
#endif
}

class DiagnosticOperation
{
public:
    explicit DiagnosticOperation(pcstr name) { CrashReporter::BeginOperation(name); }
    ~DiagnosticOperation() { CrashReporter::EndOperation(); }
};

struct AsyncSaveJob
{
    std::string saveName;
    std::string finalName;
    std::string customTempName;
    std::unique_ptr<CMemoryWriter> sourceStream;
    std::vector<u8> customData;
    SaveExtensionContainer::ChunkSnapshot extensionBase;
    SaveExtensionGameplay::ChunkMutationList extensionMutations;
    u64 saveId{};
    bool customDataExpected{};
    bool customTempPrepared{};
    bool showStatus{true};
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
    std::string errorStage;
    u32 errorCode{};
    bool showStatus{true};
    bool succeeded{};
};

enum class ScriptCustomCaptureResult
{
    Absent,
    Present,
    Error,
};

ScriptCustomCaptureResult capture_script_custom_data(
    const luabind::object& snapshot, std::vector<u8>& customData, bool append = false)
{
    lua_State* lua = snapshot.interpreter();
    if (!lua)
        return ScriptCustomCaptureResult::Error;

    snapshot.push(lua);
    const int valueType = lua_type(lua, -1);
    if (valueType == LUA_TNIL)
    {
        lua_pop(lua, 1);
        return ScriptCustomCaptureResult::Absent;
    }
    if (valueType != LUA_TSTRING)
    {
        lua_pop(lua, 1);
        return ScriptCustomCaptureResult::Error;
    }

    size_t customSize = 0;
    const char* customBytes = lua_tolstring(lua, -1, &customSize);
    if (!customBytes)
    {
        lua_pop(lua, 1);
        return ScriptCustomCaptureResult::Error;
    }
    if (append)
        customData.insert(customData.end(), customBytes, customBytes + customSize);
    else
        customData.assign(customBytes, customBytes + customSize);
    lua_pop(lua, 1);
    return ScriptCustomCaptureResult::Present;
}

thread_local u32 lastNativeError{};

struct AsyncSaveScratch
{
    std::unique_ptr<u8[]> compressed;
    std::unique_ptr<u8[]> validation;
    size_t compressedCapacity{};
    size_t validationCapacity{};
};

AsyncSaveScratch& async_save_scratch()
{
    thread_local AsyncSaveScratch scratch;
    return scratch;
}

void capture_native_error()
{
#if defined(XR_PLATFORM_WINDOWS)
    lastNativeError = GetLastError();
#else
    lastNativeError = errno;
#endif
}

bool native_file_exists(pcstr path)
{
#if defined(XR_PLATFORM_WINDOWS)
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
#else
    return access(path, F_OK) == 0;
#endif
}

#if defined(XR_PLATFORM_WINDOWS)
bool native_clear_read_only(pcstr path)
{
    const DWORD attributes = GetFileAttributesA(path);
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_READONLY))
        return false;
    return SetFileAttributesA(path, attributes & ~FILE_ATTRIBUTE_READONLY) != FALSE;
}
#endif

bool native_remove_file(pcstr path)
{
    if (!native_file_exists(path))
        return true;
#if defined(XR_PLATFORM_WINDOWS)
    if (DeleteFileA(path))
        return true;
    const DWORD error = GetLastError();
    if (error == ERROR_ACCESS_DENIED && native_clear_read_only(path))
    {
        if (DeleteFileA(path))
            return true;
        lastNativeError = GetLastError();
        return false;
    }
    lastNativeError = error;
    return false;
#else
    return unlink(path) == 0;
#endif
}

bool native_replace_file(pcstr source, pcstr destination)
{
#if defined(XR_PLATFORM_WINDOWS)
    constexpr DWORD flags = MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH;
    if (MoveFileExA(source, destination, flags))
        return true;
    const DWORD error = GetLastError();
    if (error == ERROR_ACCESS_DENIED && native_clear_read_only(destination))
    {
        if (MoveFileExA(source, destination, flags))
            return true;
        lastNativeError = GetLastError();
        return false;
    }
    lastNativeError = error;
    return false;
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
    {
        capture_native_error();
        return false;
    }

    const bool flushed = FlushFileBuffers(file) != FALSE;
    if (!flushed)
        capture_native_error();
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

bool native_write_file_impl(pcstr path, std::span<const std::span<const u8>> parts, bool durable)
{
#if defined(XR_PLATFORM_WINDOWS)
    const HANDLE file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        capture_native_error();
        return false;
    }

    bool succeeded = true;
    for (const std::span<const u8> part : parts)
    {
        const u8* cursor = part.data();
        size_t remaining = part.size();
        while (remaining)
        {
            const DWORD blockSize =
                static_cast<DWORD>(std::min<size_t>(remaining, std::numeric_limits<DWORD>::max()));
            DWORD written = 0;
            if (!WriteFile(file, cursor, blockSize, &written, nullptr) || written != blockSize)
            {
                capture_native_error();
                if (!lastNativeError)
                    lastNativeError = ERROR_WRITE_FAULT;
                succeeded = false;
                break;
            }
            cursor += written;
            remaining -= written;
        }
        if (!succeeded)
            break;
    }

    if (succeeded && durable && !FlushFileBuffers(file))
    {
        capture_native_error();
        succeeded = false;
    }
    CloseHandle(file);
    return succeeded;
#else
    const int file = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (file < 0)
        return false;

    bool succeeded = true;
    for (const std::span<const u8> part : parts)
    {
        const u8* cursor = part.data();
        size_t remaining = part.size();
        while (remaining)
        {
            const ssize_t written = write(file, cursor, remaining);
            if (written <= 0)
            {
                succeeded = false;
                break;
            }
            cursor += written;
            remaining -= static_cast<size_t>(written);
        }
        if (!succeeded)
            break;
    }

    succeeded = succeeded && (!durable || fsync(file) == 0);
    close(file);
    return succeeded;
#endif
}

bool native_write_file(pcstr path, std::span<const std::span<const u8>> parts)
{
    return native_write_file_impl(path, parts, true);
}

bool native_write_transaction_temp(pcstr path, std::span<const std::span<const u8>> parts)
{
    // Commit preflight makes every transaction temp durable before backups or the marker are touched.
    return native_write_file_impl(path, parts, false);
}

bool native_write_file(pcstr path, const void* data, size_t size)
{
    const std::array<std::span<const u8>, 1> parts{{
        {static_cast<const u8*>(data), size},
    }};
    return native_write_file(path, parts);
}

bool native_write_transaction_temp(pcstr path, const void* data, size_t size)
{
    const std::array<std::span<const u8>, 1> parts{{
        {static_cast<const u8*>(data), size},
    }};
    return native_write_transaction_temp(path, parts);
}

bool native_copy_file(pcstr source, pcstr destination)
{
#if defined(XR_PLATFORM_WINDOWS)
    bool copied = CopyFileExA(source, destination, nullptr, nullptr, nullptr, COPY_FILE_NO_BUFFERING) != FALSE;
    DWORD copyError = copied ? ERROR_SUCCESS : GetLastError();
    if (!copied && copyError == ERROR_ACCESS_DENIED && native_clear_read_only(destination))
    {
        copied = CopyFileExA(source, destination, nullptr, nullptr, nullptr, COPY_FILE_NO_BUFFERING) != FALSE;
        copyError = copied ? ERROR_SUCCESS : GetLastError();
    }
    const bool bufferingRequired = copyError == ERROR_INVALID_FUNCTION ||
        copyError == ERROR_INVALID_PARAMETER || copyError == ERROR_NOT_SUPPORTED ||
        copyError == ERROR_CALL_NOT_IMPLEMENTED;
    if (!copied && (!bufferingRequired || !CopyFileA(source, destination, FALSE)))
    {
        lastNativeError = bufferingRequired ? GetLastError() : copyError;
        return false;
    }
    native_clear_read_only(destination);
#else
    std::error_code error;
    std::filesystem::copy_file(
        source, destination, std::filesystem::copy_options::overwrite_existing, error);
    if (error)
        return false;
#endif
    return native_flush_file(destination);
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

class SaveFinalLeases final
{
public:
    SaveFinalLeases() = default;
    ~SaveFinalLeases() { release(); }

    SaveFinalLeases(const SaveFinalLeases&) = delete;
    SaveFinalLeases& operator=(const SaveFinalLeases&) = delete;

    SaveFinalLeases(SaveFinalLeases&& other) noexcept { move_from(other); }

    SaveFinalLeases& operator=(SaveFinalLeases&& other) noexcept
    {
        if (this != &other)
        {
            release();
            move_from(other);
        }
        return *this;
    }

    bool acquire(const std::array<std::string, 3>& finalNames)
    {
#if defined(XR_PLATFORM_WINDOWS)
        for (size_t index = 0; index != finalNames.size(); ++index)
        {
            handles[index] = CreateFileA(finalNames[index].c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
            if (handles[index] == INVALID_HANDLE_VALUE)
            {
                capture_native_error();
                release();
                return false;
            }
        }
        acquired = true;
        return true;
#else
        static_cast<void>(finalNames);
        return false;
#endif
    }

    void release()
    {
#if defined(XR_PLATFORM_WINDOWS)
        for (HANDLE& handle : handles)
        {
            if (handle != INVALID_HANDLE_VALUE)
            {
                CloseHandle(handle);
                handle = INVALID_HANDLE_VALUE;
            }
        }
#endif
        acquired = false;
    }

    [[nodiscard]] bool held() const { return acquired; }

private:
    void move_from(SaveFinalLeases& other) noexcept
    {
#if defined(XR_PLATFORM_WINDOWS)
        handles = other.handles;
        other.handles.fill(INVALID_HANDLE_VALUE);
#endif
        acquired = other.acquired;
        other.acquired = false;
    }

private:
#if defined(XR_PLATFORM_WINDOWS)
    std::array<HANDLE, 3> handles{INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE};
#endif
    bool acquired{};
};

struct AsyncSaveIoPreparation
{
    AsyncSaveIoPreparation() = default;
    AsyncSaveIoPreparation(const AsyncSaveIoPreparation&) = delete;
    AsyncSaveIoPreparation& operator=(const AsyncSaveIoPreparation&) = delete;
    AsyncSaveIoPreparation(AsyncSaveIoPreparation&&) noexcept = default;
    AsyncSaveIoPreparation& operator=(AsyncSaveIoPreparation&&) noexcept = default;

    [[nodiscard]] bool backups_prepared() const { return backupsPrepared && finalLeases.held(); }
    [[nodiscard]] bool custom_temp_durable() const { return customTempDurable; }
    [[nodiscard]] bool main_temp_durable() const { return mainTempDurable; }
    [[nodiscard]] const SaveExtensionContainer::FileSignature& custom_signature() const
    {
        return customSignature;
    }
    void release_final_leases() { finalLeases.release(); }

    SaveFinalLeases finalLeases;
    SaveExtensionContainer::FileSignature customSignature;
    bool backupsPrepared{};
    bool customTempDurable{};
    bool mainTempDurable{};
};

class AsyncSaveIoWorker final
{
public:
    struct Request
    {
        std::string finalName;
        std::string customTempName;
        std::span<const u8> customData;
        bool customDataExpected{};
        bool customTempPrepared{};
    };

    struct MainTempRequest
    {
        std::string tempName;
        std::array<std::span<const u8>, 2> parts;
    };

    AsyncSaveIoWorker()
    {
        worker = Threading::RunThread("Async save I/O", [this] { worker_main(); });
    }

    ~AsyncSaveIoWorker()
    {
        {
            std::unique_lock lock(mutex);
            idle.wait(lock, [this] { return !requestPending && !active; });
            stopping = true;
        }
        workAvailable.notify_one();
        worker.join();
    }

    void begin(Request&& newRequest)
    {
        {
            std::lock_guard lock(mutex);
            VERIFY(!requestPending && !mainTempPending && !active && !resultReady);
            request = std::move(newRequest);
            requestPending = true;
        }
        workAvailable.notify_one();
    }

    void publish_main_temp(MainTempRequest&& newRequest)
    {
        {
            std::lock_guard lock(mutex);
            VERIFY((requestPending || active) && !mainTempPending);
            mainTempRequest = std::move(newRequest);
            mainTempPending = true;
        }
        mainTempAvailable.notify_one();
    }

    AsyncSaveIoPreparation finish()
    {
        std::unique_lock lock(mutex);
        resultAvailable.wait(lock, [this] { return resultReady; });
        AsyncSaveIoPreparation finished = std::move(result);
        resultReady = false;
        return finished;
    }

private:
    static AsyncSaveIoPreparation prepare(const Request& request)
    {
        AsyncSaveIoPreparation prepared;
        string_path customName;
        make_custom_save_name(request.finalName.c_str(), customName);
        string_path sidecarName;
        make_companion_save_name(request.finalName.c_str(), SaveExtensionContainer::extension, sidecarName);
        const std::array<std::string, 3> finalNames{{request.finalName, customName, sidecarName}};
        if (!prepared.finalLeases.acquire(finalNames))
            return {};

        if (request.customDataExpected)
        {
            if (request.customTempPrepared)
            {
                if (!native_flush_file(request.customTempName.c_str()) ||
                    SaveExtensionContainer::read_file_signature(
                        request.customTempName.c_str(), prepared.customSignature) !=
                        SaveExtensionContainer::SignatureResult::Valid)
                {
                    return {};
                }
            }
            else if (!native_write_file(
                         request.customTempName.c_str(), request.customData.data(), request.customData.size()))
            {
                return {};
            }
            prepared.customTempDurable = true;
        }

        // Unmarked backups are recovery-inert, so early durable copies preserve crash semantics.
        for (const std::string& finalName : finalNames)
        {
            if (!native_copy_file(finalName.c_str(), (finalName + ".bak").c_str()))
                return {};
        }
        prepared.backupsPrepared = true;
        return prepared;
    }

    void worker_main()
    {
        for (;;)
        {
            Request current;
            {
                std::unique_lock lock(mutex);
                workAvailable.wait(lock, [this] { return stopping || requestPending; });
                if (stopping && !requestPending)
                    return;
                current = std::move(request);
                requestPending = false;
                active = true;
            }

            AsyncSaveIoPreparation completed = prepare(current);
            MainTempRequest currentMainTemp;
            {
                std::unique_lock lock(mutex);
                mainTempAvailable.wait(lock, [this] { return mainTempPending; });
                currentMainTemp = std::move(mainTempRequest);
                mainTempPending = false;
            }
            if (completed.backups_prepared())
            {
                if (native_write_file(currentMainTemp.tempName.c_str(), currentMainTemp.parts))
                    completed.mainTempDurable = true;
                else
                    completed = AsyncSaveIoPreparation{};
            }
            {
                std::lock_guard lock(mutex);
                result = std::move(completed);
                active = false;
                resultReady = true;
            }
            resultAvailable.notify_one();
            idle.notify_all();
        }
    }

private:
    std::mutex mutex;
    std::condition_variable workAvailable;
    std::condition_variable mainTempAvailable;
    std::condition_variable resultAvailable;
    std::condition_variable idle;
    Request request;
    MainTempRequest mainTempRequest;
    AsyncSaveIoPreparation result;
    bool requestPending{};
    bool mainTempPending{};
    bool resultReady{};
    bool active{};
    bool stopping{};
    std::thread worker;
};

bool commit_save_set(
    pcstr scopName, pcstr scopTemp, pcstr scocTemp, pcstr sidecarTemp, bool customDataExpected, u64 saveId,
    AsyncSaveIoPreparation& ioPreparation)
{
    string_path scocName;
    make_custom_save_name(scopName, scocName);
    string_path sidecarName;
    make_companion_save_name(scopName, SaveExtensionContainer::extension, sidecarName);
    const std::string transactionMarker = std::string(scopName) + SaveTransactionMarker::suffix;

    bool backupsPrepared = ioPreparation.backups_prepared();
    if (backupsPrepared)
    {
        SaveTransactionMarker::State markerState;
        if (SaveTransactionMarker::read(transactionMarker.c_str(), markerState) !=
            SaveTransactionMarker::ReadResult::Missing)
        {
            ioPreparation.release_final_leases();
            backupsPrepared = false;
        }
    }
    if (!backupsPrepared && !CSavedGameWrapper::recover_interrupted_save_file_for_commit(scopName))
        return false;

    struct TransactionFile
    {
        std::string finalName;
        std::string tempName;
        std::string backupName;
        bool install{};
        bool tempDurable{};
        bool hadFinal{};
    };

    std::array<TransactionFile, 3> files{{
        {scopName, scopTemp, std::string(scopName) + ".bak", true,
            ioPreparation.main_temp_durable()},
        {scocName, scocTemp, std::string(scocName) + ".bak", customDataExpected,
            ioPreparation.custom_temp_durable()},
        {sidecarName, sidecarTemp, std::string(sidecarName) + ".bak", true, false},
    }};

    for (TransactionFile& file : files)
    {
        if (file.install && (!native_file_exists(file.tempName.c_str()) ||
                (!file.tempDurable && !native_flush_file(file.tempName.c_str()))))
            return false;
        file.hadFinal = backupsPrepared || native_file_exists(file.finalName.c_str());
        if (!file.hadFinal && !native_remove_file(file.backupName.c_str()))
            return false;
    }

    const auto restore_previous = [&files]()
    {
        constexpr std::array<size_t, 2> companionOrder{1, 2};
        for (const size_t index : companionOrder)
        {
            TransactionFile& file = files[index];
            bool restored = false;
            if (file.hadFinal)
                restored = native_copy_file(file.backupName.c_str(), file.finalName.c_str());
            else
                restored = native_remove_file(file.finalName.c_str());
            if (!restored)
                return false;
        }

        TransactionFile& main = files[0];
        return main.hadFinal ? native_copy_file(main.backupName.c_str(), main.finalName.c_str()) :
                               native_remove_file(main.finalName.c_str());
    };

    if (!backupsPrepared)
    {
        // Backups are copied before the main commit marker is removed, so a crash never exposes a partial backup set.
        for (TransactionFile& file : files)
        {
            if (!file.hadFinal)
                continue;
            if (!native_copy_file(file.finalName.c_str(), file.backupName.c_str()))
                return false;
        }
    }

    xr_vector<u8> markerContents;
    SaveTransactionMarker::State markerState;
    markerState.saveId = saveId;
    markerState.flags = customDataExpected ? SaveTransactionMarker::customDataExpected : 0;
    if (files[0].hadFinal)
        markerState.flags |= SaveTransactionMarker::previousMainPresent;
    if (files[1].hadFinal)
        markerState.flags |= SaveTransactionMarker::previousCustomPresent;
    if (files[2].hadFinal)
        markerState.flags |= SaveTransactionMarker::previousSidecarPresent;
    SaveTransactionMarker::build(markerState, markerContents);
    if (!native_write_file(transactionMarker.c_str(), markerContents.data(), markerContents.size()))
    {
        const u32 markerWriteError = lastNativeError;
        if (!native_remove_file(transactionMarker.c_str()))
            Msg("! Incomplete save transaction marker could not be removed for '%s'", scopName);
        lastNativeError = markerWriteError;
        return false;
    }
    ioPreparation.release_final_leases();

    if (!native_remove_file(files[0].finalName.c_str()))
    {
        native_remove_file(transactionMarker.c_str());
        return false;
    }

    constexpr std::array<size_t, 2> companionOrder{1, 2};
    for (const size_t index : companionOrder)
    {
        TransactionFile& file = files[index];
        const bool updated = file.install ?
            native_replace_file(file.tempName.c_str(), file.finalName.c_str()) :
            native_remove_file(file.finalName.c_str());
        if (!updated)
        {
            if (restore_previous())
                native_remove_file(transactionMarker.c_str());
            return false;
        }
    }

    if (!native_replace_file(files[0].tempName.c_str(), files[0].finalName.c_str()))
    {
        if (restore_previous())
            native_remove_file(transactionMarker.c_str());
        return false;
    }
    if (!native_remove_file(transactionMarker.c_str()))
        Msg("! Saved game transaction marker could not be removed for '%s'", scopName);
    return true;
}

AsyncSaveCompletion execute_save_job(AsyncSaveJob&& job, AsyncSaveIoWorker& ioWorker)
{
    lastNativeError = 0;
    CTimer timer;
    timer.Start();

    AsyncSaveCompletion completion;
    completion.saveName = job.saveName;
    completion.finalName = job.finalName;
    completion.showStatus = job.showStatus;
    const u8* sourceData = static_cast<const u8*>(job.sourceStream->pointer());
    const size_t sourceSize = job.sourceStream->tell();
    completion.captureMilliseconds = job.captureMilliseconds;

    if (sourceSize > std::numeric_limits<u32>::max() || sourceSize > CSavedGameWrapper::maxSourceSize)
    {
#if defined(XR_PLATFORM_WINDOWS)
        completion.errorCode = ERROR_FILE_TOO_LARGE;
#else
        completion.errorCode = EFBIG;
#endif
        completion.errorStage = "validate save source size";
        if (job.customDataExpected)
            native_remove_file(job.customTempName.c_str());
        completion.backgroundMilliseconds = timer.GetElapsed_sec() * 1000.f;
        return completion;
    }
    completion.sourceSize = static_cast<u32>(sourceSize);

    const std::string transactionMarker = job.finalName + SaveTransactionMarker::suffix;
    SaveTransactionMarker::State markerState;
    const bool ioPreparationStarted =
#if defined(XR_PLATFORM_WINDOWS)
        SaveTransactionMarker::read(transactionMarker.c_str(), markerState) ==
        SaveTransactionMarker::ReadResult::Missing;
#else
        false;
#endif
    if (ioPreparationStarted)
    {
        ioWorker.begin({job.finalName, job.customTempName, job.customData,
            job.customDataExpected, job.customTempPrepared});
    }

    AsyncSaveScratch& scratch = async_save_scratch();
    const size_t compressionCapacity = rtc_csize(completion.sourceSize);
    if (scratch.compressedCapacity < compressionCapacity)
    {
        scratch.compressed = std::make_unique_for_overwrite<u8[]>(compressionCapacity);
        scratch.compressedCapacity = compressionCapacity;
    }
    completion.compressedSize = static_cast<u32>(rtc_compress(
        scratch.compressed.get(), scratch.compressedCapacity, sourceData, sourceSize));

    const u32 marker = u32(-1);
    const u32 version = ALIFE_VERSION;
    const std::array<u32, 3> saveHeader{marker, version, completion.sourceSize};
    const std::array<std::span<const u8>, 2> saveParts{{
        {reinterpret_cast<const u8*>(saveHeader.data()), sizeof(saveHeader)},
        {scratch.compressed.get(), completion.compressedSize},
    }};

    string_path saveIdText;
    xr_sprintf(saveIdText, "%016llx", static_cast<unsigned long long>(job.saveId));
    const std::string scopTemp = job.finalName + "." + saveIdText + ".tmp";
    string_path sidecarName;
    make_companion_save_name(job.finalName.c_str(), SaveExtensionContainer::extension, sidecarName);
    const std::string sidecarTemp = std::string(sidecarName) + "." + saveIdText + ".tmp";
    if (ioPreparationStarted)
        ioWorker.publish_main_temp({scopTemp, saveParts});

    if (scratch.validationCapacity < sourceSize)
    {
        scratch.validation = std::make_unique_for_overwrite<u8[]>(sourceSize);
        scratch.validationCapacity = sourceSize;
    }
    // The decompressor treats this length as the exact expected output size, not merely buffer capacity.
    const size_t decodedSize = rtc_decompress(
        scratch.validation.get(), sourceSize, scratch.compressed.get(), completion.compressedSize);
    bool written = decodedSize == sourceSize &&
        memcmp(scratch.validation.get(), sourceData, sourceSize) == 0;
    if (!written)
        completion.errorStage = "compression validation";

    SaveExtensionContainer::Binding binding;
    if (written)
    {
        binding.saveId = job.saveId;
        binding.sourceSize = completion.sourceSize;
        binding.compressedSize = completion.compressedSize;
        binding.scop.size = sizeof(saveHeader) + completion.compressedSize;
        binding.scop.checksum = crc32(saveHeader.data(), sizeof(saveHeader));
        binding.scop.checksum =
            crc32(scratch.compressed.get(), completion.compressedSize, binding.scop.checksum);
        binding.scop.present = true;
    }
    if (written && job.customDataExpected && !job.customTempPrepared)
    {
        binding.scoc = SaveExtensionContainer::make_signature({job.customData.data(), job.customData.size()});
    }

    SaveExtensionContainer::ChunkList extensionChunks;
    if (written && job.extensionBase)
        extensionChunks = *job.extensionBase;
    if (written && !SaveExtensionGameplay::apply_mutations(
            extensionChunks, std::move(job.extensionMutations)))
    {
        written = false;
        completion.errorStage = "merge save extensions";
    }

    AsyncSaveIoPreparation ioPreparation;
    if (ioPreparationStarted)
        ioPreparation = ioWorker.finish();
    if (written && job.customDataExpected && job.customTempPrepared)
    {
        const bool signedCustom = ioPreparation.custom_temp_durable() ?
            (binding.scoc = ioPreparation.custom_signature()).present :
            native_flush_file(job.customTempName.c_str()) &&
                SaveExtensionContainer::read_file_signature(
                    job.customTempName.c_str(), binding.scoc) == SaveExtensionContainer::SignatureResult::Valid;
        if (!signedCustom)
        {
            written = false;
            completion.errorStage = "sign script save data";
        }
    }

    xr_vector<u8> sidecar;
    if (written && !SaveExtensionContainer::build(binding, extensionChunks, sidecar))
    {
        written = false;
        completion.errorStage = "encode save extensions";
    }
    if (written && !ioPreparation.main_temp_durable() &&
        !native_write_transaction_temp(scopTemp.c_str(), saveParts))
    {
        written = false;
        completion.errorStage = "write save data";
    }
    if (written && job.customDataExpected && !job.customTempPrepared &&
        !ioPreparation.custom_temp_durable() && !native_write_transaction_temp(
            job.customTempName.c_str(), job.customData.data(), job.customData.size()))
    {
        written = false;
        completion.errorStage = "write script save data";
    }
    if (written && !native_write_transaction_temp(sidecarTemp.c_str(), sidecar.data(), sidecar.size()))
    {
        written = false;
        completion.errorStage = "write save extensions";
    }
    if (written && !commit_save_set(job.finalName.c_str(), scopTemp.c_str(), job.customTempName.c_str(),
            sidecarTemp.c_str(), job.customDataExpected, job.saveId, ioPreparation))
    {
        written = false;
        completion.errorStage = "commit save files";
    }
    if (!written)
    {
        completion.errorCode = lastNativeError;
        native_remove_file(scopTemp.c_str());
        if (job.customDataExpected)
            native_remove_file(job.customTempName.c_str());
        native_remove_file(sidecarTemp.c_str());
        completion.backgroundMilliseconds = timer.GetElapsed_sec() * 1000.f;
        return completion;
    }

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

            AsyncSaveCompletion completion = execute_save_job(std::move(job), ioWorker);

            {
                std::lock_guard lock(mutex);
                completions.emplace_back(std::move(completion));
                active = false;
            }
            idle.notify_all();
        }
    }

private:
    AsyncSaveIoWorker ioWorker;
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
thread_local bool saveCaptureScopeActive{};

class SaveCaptureScope final
{
public:
    SaveCaptureScope() { saveCaptureScopeActive = true; }
    ~SaveCaptureScope() { saveCaptureScopeActive = false; }
};

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

bool client_save_snapshot_step(const xr_vector<u16>& objectIds, u32& start, u32 objectBudget)
{
    NET_Packet packet;
    packet.w_begin(M_SAVE_PACKET);

    u32 processedObjects = 0;
    while (start < objectIds.size() && processedObjects < objectBudget)
    {
        IGameObject* object = Level().Objects.net_Find(objectIds[start++]);
        ++processedObjects;
        CGameObject* gameObject = smart_cast<CGameObject*>(object);
        if (!gameObject || gameObject->getDestroy() || !gameObject->net_SaveRelevant())
            continue;

        packet.w_u16(static_cast<u16>(gameObject->ID()));
        u32 position = 0;
        packet.w_chunk_open16(position);
        gameObject->net_Save(packet);
        packet.w_chunk_close16(position);
        constexpr u32 maximumObjectBytes = 8 * 1024;
        if (maximumObjectBytes >= NET_PacketSizeLimit - packet.w_tell())
            break;
    }

    if (packet.B.count > 2)
        Level().Send(packet, net_flags(FALSE));
    return start >= objectIds.size();
}
}

struct SaveCaptureQueue
{
    struct Pending
    {
        AsyncSaveJob job;
        std::unique_ptr<CMemoryWriter> stream;
        CALifeSpawnRegistry::SaveState spawnState;
        CALifeObjectRegistry::SaveState objectState;
        std::unique_ptr<NET_Packet> objectPacket;
        CALifeRegistryContainer::SaveState registryState;
        SaveExtensionGameplay::CaptureState extensionState;
        float mainThreadMilliseconds{};
        float scriptMilliseconds{};
        float headerMilliseconds{};
        float spawnMilliseconds{};
        float objectMilliseconds{};
        float registryMilliseconds{};
        float extensionMilliseconds{};
        float maxScriptCallMilliseconds{};
        float maxSpawnCallMilliseconds{};
        float maxObjectCallMilliseconds{};
        u64 maxObjectCallCycles{};
        u64 maxObjectCallKernel100ns{};
        u64 maxObjectCallUser100ns{};
        float maxRegistryCallMilliseconds{};
        float maxExtensionCallMilliseconds{};
        float initialFrameMilliseconds{};
        float maxSliceMilliseconds{};
        u32 sliceCount{};
        u32 queuedAt{};
        bool scriptPreparePending{};
        bool scriptEncodePending{};
        bool scriptIncrementalEncode{};
        bool scriptEncodeStarted{};
        bool scriptEncodeSizeKnown{};
        u8 phase{};
    };

    std::deque<std::unique_ptr<Pending>> pending;
    bool failed{};
};

struct SaveRequestQueue
{
    struct Pending
    {
        std::string saveName;
        xr_vector<u16> clientObjectIds;
        u32 clientSaveStart{};
        bool updateName{};
        bool showStatus{true};
        bool clientSaveComplete{};
    };

    std::deque<Pending> pending;
    bool failed{};
};

CALifeStorageManager::CALifeStorageManager(IPureServer* server, LPCSTR section) : inherited(server, section)
{
    m_section = section;
    xr_strcpy(m_save_name, "");
    SaveExtensionContainer::clear_loaded_chunks();
}

CALifeStorageManager::~CALifeStorageManager()
{
    wait_for_pending_saves();
    if (activeStorageManager == this)
        activeStorageManager = nullptr;
}

bool CALifeStorageManager::process_async_save_completions()
{
    bool allSucceeded = true;
    if (activeStorageManager)
    {
        constexpr float saveWorkBudgetMilliseconds = 3.f;
        CTimer budgetTimer;
        budgetTimer.Start();
        activeStorageManager->process_save_requests(saveWorkBudgetMilliseconds);
        const float remainingMilliseconds =
            saveWorkBudgetMilliseconds - budgetTimer.GetElapsed_sec() * 1000.f;
        if (remainingMilliseconds > 0.f)
            activeStorageManager->process_save_captures(remainingMilliseconds);

        if (activeStorageManager->m_save_requests && activeStorageManager->m_save_requests->failed)
        {
            activeStorageManager->m_save_requests->failed = false;
            allSucceeded = false;
        }
        if (activeStorageManager->m_save_captures && activeStorageManager->m_save_captures->failed)
        {
            activeStorageManager->m_save_captures->failed = false;
            allSucceeded = false;
        }
    }

    auto completions = async_save_coordinator().take_completions();
    if (completions.empty())
        return allSucceeded;

    refresh_save_index();
    for (const auto& completion : completions)
    {
        if (!completion.succeeded)
        {
            allSucceeded = false;
            Msg("! Asynchronous save transaction failed for '%s' during %s (system error %u); "
                "the previous save set was preserved", completion.saveName.c_str(),
                completion.errorStage.empty() ? "unknown stage" : completion.errorStage.c_str(), completion.errorCode);
            if (completion.showStatus)
                show_save_status(StringTable().translate("st_game_save_failed").c_str());
            continue;
        }

        Msg("* Game %s is successfully saved to file '%s' (capture %.3f ms, background %.3f ms)",
            completion.saveName.c_str(), completion.finalName.c_str(),
            completion.captureMilliseconds, completion.backgroundMilliseconds);
        CrashReporter::SetSavePath(completion.finalName.c_str());

        std::string displayName = completion.saveName;
        const size_t extension = displayName.rfind('.');
        if (extension != std::string::npos)
            displayName.erase(extension);
        if (completion.showStatus)
        {
            const std::string message =
                StringTable().translate("st_game_saved").c_str() + std::string(": ") + displayName;
            show_save_status(message.c_str());
        }

        if (GEnv.ScriptEngine)
        {
            luabind::functor<void> afterSave;
            if (GEnv.ScriptEngine->functor("alife_storage_manager.CALifeStorageManager_save", afterSave))
                afterSave(completion.saveName.c_str());
        }
    }
    return allSucceeded;
}

bool CALifeStorageManager::wait_for_pending_saves()
{
    if (saveCaptureScopeActive)
        return false;

    bool requestsSucceeded = true;
    while (activeStorageManager)
    {
        CALifeStorageManager* manager = activeStorageManager;
        const bool requestsPending = manager->m_save_requests && !manager->m_save_requests->pending.empty();
        const bool capturesPending = manager->m_save_captures && !manager->m_save_captures->pending.empty();
        if (!requestsPending && !capturesPending)
            break;

        // Lua capture state is process-global, so finish it before beginning another request.
        if (capturesPending)
        {
            manager->process_save_captures(flt_max);
            continue;
        }

        if (!g_pGameLevel)
        {
            Msg("! Discarded %zu pending save request(s) because the game level is unavailable",
                manager->m_save_requests->pending.size());
            manager->m_save_requests->pending.clear();
            requestsSucceeded = false;
            break;
        }

        manager->process_save_requests(flt_max);
    }
    async_save_coordinator().wait();
    return process_async_save_completions() && requestsSucceeded;
}

void CALifeStorageManager::process_save_requests(float budgetMilliseconds)
{
    if (!m_save_requests || m_save_requests->pending.empty() || !g_pGameLevel)
        return;

    CTimer budgetTimer;
    budgetTimer.Start();
    while (!m_save_requests->pending.empty())
    {
        if ((m_save_captures && !m_save_captures->pending.empty()) || !g_pGameLevel)
            return;

        SaveRequestQueue::Pending& request = m_save_requests->pending.front();
        if (!request.clientSaveComplete)
        {
            if (!client_save_snapshot_step(request.clientObjectIds, request.clientSaveStart, 16))
            {
                if (budgetMilliseconds != flt_max &&
                    budgetTimer.GetElapsed_sec() * 1000.f >= budgetMilliseconds)
                {
                    return;
                }
                continue;
            }

            request.clientSaveComplete = true;
            // Finite work resumes next frame so native capture starts with a fresh global budget.
            if (budgetMilliseconds != flt_max)
                return;
        }

        if (!save(request.saveName.c_str(), request.updateName, request.showStatus))
            m_save_requests->failed = true;
        m_save_requests->pending.pop_front();

        if (m_save_captures && !m_save_captures->pending.empty())
            return;

        if (budgetMilliseconds != flt_max &&
            budgetTimer.GetElapsed_sec() * 1000.f >= budgetMilliseconds)
        {
            return;
        }
    }
}

bool CALifeStorageManager::save_capture_active()
{
    return saveCaptureScopeActive || (activeStorageManager &&
        ((activeStorageManager->m_save_requests && !activeStorageManager->m_save_requests->pending.empty()) ||
            (activeStorageManager->m_save_captures && !activeStorageManager->m_save_captures->pending.empty())));
}

bool CALifeStorageManager::save_capture_reentrant()
{
    return saveCaptureScopeActive;
}

void CALifeStorageManager::process_save_captures(float budgetMilliseconds)
{
    if (!m_save_captures || saveCaptureScopeActive)
        return;

    const SaveCaptureScope captureScope;

    CTimer budgetTimer;
    budgetTimer.Start();
    const bool unlimitedBudget = budgetMilliseconds == flt_max;
    const auto budgetExhausted = [&]()
    {
        return !unlimitedBudget && budgetTimer.GetElapsed_sec() * 1000.f >= budgetMilliseconds;
    };
    const auto remainingBudget = [&]()
    {
        return unlimitedBudget ? flt_max :
            std::max(0.01f, budgetMilliseconds - budgetTimer.GetElapsed_sec() * 1000.f);
    };

    while (!m_save_captures->pending.empty())
    {
        if (budgetExhausted())
            return;

        SaveCaptureQueue::Pending& capture = *m_save_captures->pending.front();
        CTimer sliceTimer;
        sliceTimer.Start();
        ++capture.sliceCount;
        pcstr scriptFailureStage{};
        const auto failScriptCapture = [&](pcstr stage)
        {
            Msg("! Script save capture failed for '%s' during %s; the previous save set was preserved",
                capture.job.saveName.c_str(), stage);
            native_remove_file(capture.job.customTempName.c_str());
            if (capture.job.showStatus)
                show_save_status(StringTable().translate("st_game_save_failed").c_str());
            m_save_captures->failed = true;
            m_save_captures->pending.pop_front();
        };

        if (capture.phase == 0)
        {
            bool completed = false;
            while (!completed)
            {
                CTimer scriptTimer;
                scriptTimer.Start();
                luabind::functor<bool> prepareStep;
                if (!GEnv.ScriptEngine->functor(
                        "alife_storage_manager.CALifeStorageManager_capture_prepare_step", prepareStep))
                {
                    scriptFailureStage = "prepare step lookup";
                    break;
                }
                completed = prepareStep(64);
                const float scriptMilliseconds = scriptTimer.GetElapsed_sec() * 1000.f;
                capture.scriptMilliseconds += scriptMilliseconds;
                capture.mainThreadMilliseconds += scriptMilliseconds;
                capture.maxScriptCallMilliseconds =
                    std::max(capture.maxScriptCallMilliseconds, scriptMilliseconds);

                if (!completed && budgetExhausted())
                {
                    capture.maxSliceMilliseconds =
                        std::max(capture.maxSliceMilliseconds, sliceTimer.GetElapsed_sec() * 1000.f);
                    return;
                }
            }
            if (scriptFailureStage)
            {
                failScriptCapture(scriptFailureStage);
                if (budgetExhausted())
                    return;
                continue;
            }
            capture.scriptPreparePending = false;
            capture.phase = 1;
        }

        if (capture.phase == 1 && !budgetExhausted())
        {
            if (capture.scriptIncrementalEncode)
            {
                if (!capture.scriptEncodeStarted)
                {
                    CTimer scriptTimer;
                    scriptTimer.Start();
                    luabind::functor<bool> encodeBegin;
                    if (!GEnv.ScriptEngine->functor(
                            "alife_storage_manager.CALifeStorageManager_capture_encode_begin", encodeBegin) ||
                        !encodeBegin())
                    {
                        scriptFailureStage = "incremental encode begin";
                    }
                    const float scriptMilliseconds = scriptTimer.GetElapsed_sec() * 1000.f;
                    capture.scriptMilliseconds += scriptMilliseconds;
                    capture.mainThreadMilliseconds += scriptMilliseconds;
                    capture.maxScriptCallMilliseconds =
                        std::max(capture.maxScriptCallMilliseconds, scriptMilliseconds);
                    capture.scriptEncodeStarted = !scriptFailureStage;
                }

                bool completed = false;
                while (!scriptFailureStage && !completed && !budgetExhausted())
                {
                    CTimer scriptTimer;
                    scriptTimer.Start();
                    int status = -1;
                    luabind::functor<int> encodeStep;
                    if (!GEnv.ScriptEngine->functor(
                            "alife_storage_manager.CALifeStorageManager_capture_encode_step", encodeStep))
                    {
                        scriptFailureStage = "incremental encode step lookup";
                    }
                    else
                        status = encodeStep(256);
                    float scriptMilliseconds = scriptTimer.GetElapsed_sec() * 1000.f;
                    capture.scriptMilliseconds += scriptMilliseconds;
                    capture.mainThreadMilliseconds += scriptMilliseconds;
                    capture.maxScriptCallMilliseconds =
                        std::max(capture.maxScriptCallMilliseconds, scriptMilliseconds);

                    if (status < 0 || status > 2)
                    {
                        scriptFailureStage = "incremental encode step";
                        break;
                    }
                    if (!status)
                        continue;

                    if (!capture.scriptEncodeSizeKnown)
                    {
                        scriptTimer.Start();
                        luabind::functor<u32> encodeSize;
                        if (!GEnv.ScriptEngine->functor(
                                "alife_storage_manager.CALifeStorageManager_capture_encode_size", encodeSize))
                        {
                            scriptFailureStage = "incremental encode size lookup";
                        }
                        else
                        {
                            capture.job.customData.reserve(encodeSize());
                            capture.scriptEncodeSizeKnown = true;
                        }
                        scriptMilliseconds = scriptTimer.GetElapsed_sec() * 1000.f;
                        capture.scriptMilliseconds += scriptMilliseconds;
                        capture.mainThreadMilliseconds += scriptMilliseconds;
                        capture.maxScriptCallMilliseconds =
                            std::max(capture.maxScriptCallMilliseconds, scriptMilliseconds);
                        if (scriptFailureStage)
                            break;
                    }

                    scriptTimer.Start();
                    luabind::functor<luabind::object> encodeResult;
                    if (!GEnv.ScriptEngine->functor(
                            "alife_storage_manager.CALifeStorageManager_capture_encode_result", encodeResult))
                    {
                        scriptFailureStage = "incremental encode result lookup";
                    }
                    else
                    {
                        const luabind::object chunk = encodeResult();
                        switch (capture_script_custom_data(chunk, capture.job.customData, true))
                        {
                        case ScriptCustomCaptureResult::Present:
                            capture.job.customDataExpected = true;
                            break;
                        case ScriptCustomCaptureResult::Absent:
                            break;
                        case ScriptCustomCaptureResult::Error:
                            scriptFailureStage = "incremental encode result";
                            break;
                        }
                    }
                    scriptMilliseconds = scriptTimer.GetElapsed_sec() * 1000.f;
                    capture.scriptMilliseconds += scriptMilliseconds;
                    capture.mainThreadMilliseconds += scriptMilliseconds;
                    capture.maxScriptCallMilliseconds =
                        std::max(capture.maxScriptCallMilliseconds, scriptMilliseconds);
                    completed = status == 2;
                }

                if (!scriptFailureStage && !completed)
                {
                    capture.maxSliceMilliseconds =
                        std::max(capture.maxSliceMilliseconds, sliceTimer.GetElapsed_sec() * 1000.f);
                    return;
                }
            }
            else
            {
                CTimer scriptTimer;
                scriptTimer.Start();
                luabind::functor<luabind::object> encodeSave;
                if (!GEnv.ScriptEngine->functor(
                        "alife_storage_manager.CALifeStorageManager_capture_encode", encodeSave))
                {
                    scriptFailureStage = "encode lookup";
                }
                else
                {
                    const luabind::object snapshot = encodeSave();
                    switch (capture_script_custom_data(snapshot, capture.job.customData))
                    {
                    case ScriptCustomCaptureResult::Present:
                        capture.job.customDataExpected = true;
                        break;
                    case ScriptCustomCaptureResult::Absent:
                        break;
                    case ScriptCustomCaptureResult::Error:
                        scriptFailureStage = "encode result";
                        break;
                    }
                }
                const float scriptMilliseconds = scriptTimer.GetElapsed_sec() * 1000.f;
                capture.scriptMilliseconds += scriptMilliseconds;
                capture.mainThreadMilliseconds += scriptMilliseconds;
                capture.maxScriptCallMilliseconds =
                    std::max(capture.maxScriptCallMilliseconds, scriptMilliseconds);
            }

            if (scriptFailureStage)
            {
                failScriptCapture(scriptFailureStage);
                if (budgetExhausted())
                    return;
                continue;
            }
            capture.scriptEncodePending = false;
            capture.phase = 2;
        }

        if (capture.phase == 2 && !budgetExhausted())
        {
            CTimer spawnTimer;
            spawnTimer.Start();
            spawns().begin_save(*capture.stream, capture.spawnState);
            const float spawnMilliseconds = spawnTimer.GetElapsed_sec() * 1000.f;
            capture.spawnMilliseconds += spawnMilliseconds;
            capture.mainThreadMilliseconds += spawnMilliseconds;
            capture.maxSpawnCallMilliseconds =
                std::max(capture.maxSpawnCallMilliseconds, spawnMilliseconds);
            capture.phase = 3;
        }

        if (capture.phase == 3 && !budgetExhausted())
        {
            CTimer spawnTimer;
            spawnTimer.Start();
            const bool completed =
                spawns().continue_save(*capture.stream, capture.spawnState, remainingBudget());
            const float spawnMilliseconds = spawnTimer.GetElapsed_sec() * 1000.f;
            capture.spawnMilliseconds += spawnMilliseconds;
            capture.mainThreadMilliseconds += spawnMilliseconds;
            capture.maxSpawnCallMilliseconds =
                std::max(capture.maxSpawnCallMilliseconds, spawnMilliseconds);
            if (!completed)
            {
                capture.maxSliceMilliseconds =
                    std::max(capture.maxSliceMilliseconds, sliceTimer.GetElapsed_sec() * 1000.f);
                return;
            }
            capture.phase = 4;
        }

        if (capture.phase == 4 && !budgetExhausted())
        {
            CTimer objectTimer;
            const u64 objectCyclesStarted = current_thread_cycles();
            const ThreadCpuTime objectCpuStarted = current_thread_cpu_time();
            objectTimer.Start();
            objects().begin_save(*capture.stream, capture.objectState);
            const float objectMilliseconds = objectTimer.GetElapsed_sec() * 1000.f;
            const ThreadCpuTime objectCpuEnded = current_thread_cpu_time();
            const u64 objectCycles = current_thread_cycles() - objectCyclesStarted;
            capture.objectMilliseconds += objectMilliseconds;
            capture.mainThreadMilliseconds += objectMilliseconds;
            if (objectMilliseconds > capture.maxObjectCallMilliseconds)
            {
                capture.maxObjectCallMilliseconds = objectMilliseconds;
                capture.maxObjectCallCycles = objectCycles;
                capture.maxObjectCallKernel100ns =
                    objectCpuEnded.kernel100ns - objectCpuStarted.kernel100ns;
                capture.maxObjectCallUser100ns =
                    objectCpuEnded.user100ns - objectCpuStarted.user100ns;
            }
            capture.phase = 5;
        }

        bool objectsCompleted = capture.phase >= 6;
        if (capture.phase == 5 && !budgetExhausted())
        {
            CTimer objectTimer;
            const u64 objectCyclesStarted = current_thread_cycles();
            const ThreadCpuTime objectCpuStarted = current_thread_cpu_time();
            objectTimer.Start();
            objectsCompleted = objects().continue_save(
                *capture.stream, capture.objectState, remainingBudget(), *capture.objectPacket);
            const float objectMilliseconds = objectTimer.GetElapsed_sec() * 1000.f;
            const ThreadCpuTime objectCpuEnded = current_thread_cpu_time();
            const u64 objectCycles = current_thread_cycles() - objectCyclesStarted;
            capture.objectMilliseconds += objectMilliseconds;
            capture.mainThreadMilliseconds += objectMilliseconds;
            if (objectMilliseconds > capture.maxObjectCallMilliseconds)
            {
                capture.maxObjectCallMilliseconds = objectMilliseconds;
                capture.maxObjectCallCycles = objectCycles;
                capture.maxObjectCallKernel100ns =
                    objectCpuEnded.kernel100ns - objectCpuStarted.kernel100ns;
                capture.maxObjectCallUser100ns =
                    objectCpuEnded.user100ns - objectCpuStarted.user100ns;
            }
            if (objectsCompleted)
                capture.phase = 6;
        }

        float sliceMilliseconds = sliceTimer.GetElapsed_sec() * 1000.f;
        capture.maxSliceMilliseconds = std::max(capture.maxSliceMilliseconds, sliceMilliseconds);
        if (!objectsCompleted)
            return;
        if (budgetExhausted())
            return;

        if (capture.phase == 6)
        {
            CTimer registryTimer;
            registryTimer.Start();
            registry().begin_save(*capture.stream, capture.registryState);
            const float registryMilliseconds = registryTimer.GetElapsed_sec() * 1000.f;
            capture.registryMilliseconds += registryMilliseconds;
            capture.mainThreadMilliseconds += registryMilliseconds;
            capture.maxRegistryCallMilliseconds =
                std::max(capture.maxRegistryCallMilliseconds, registryMilliseconds);
            capture.phase = 7;
        }

        if (capture.phase == 7)
        {
            if (budgetExhausted())
                return;

            CTimer registryTimer;
            registryTimer.Start();
            const bool registriesCompleted =
                registry().continue_save(*capture.stream, capture.registryState, remainingBudget());
            const float registryMilliseconds = registryTimer.GetElapsed_sec() * 1000.f;
            capture.registryMilliseconds += registryMilliseconds;
            capture.mainThreadMilliseconds += registryMilliseconds;
            capture.maxRegistryCallMilliseconds =
                std::max(capture.maxRegistryCallMilliseconds, registryMilliseconds);
            sliceMilliseconds = sliceTimer.GetElapsed_sec() * 1000.f;
            capture.maxSliceMilliseconds = std::max(capture.maxSliceMilliseconds, sliceMilliseconds);
            if (!registriesCompleted)
                return;
            capture.phase = 8;
        }

        if (budgetExhausted())
            return;

        if (capture.phase == 8)
        {
            CTimer extensionTimer;
            extensionTimer.Start();
            capture.job.extensionBase = SaveExtensionContainer::snapshot_loaded_chunks();
            SaveExtensionGameplay::begin_capture(capture.extensionState);
            const float extensionMilliseconds = extensionTimer.GetElapsed_sec() * 1000.f;
            capture.extensionMilliseconds += extensionMilliseconds;
            capture.mainThreadMilliseconds += extensionMilliseconds;
            capture.maxExtensionCallMilliseconds =
                std::max(capture.maxExtensionCallMilliseconds, extensionMilliseconds);
            capture.phase = 9;
        }

        if (budgetExhausted())
            return;

        CTimer extensionTimer;
        extensionTimer.Start();
        const bool extensionsCompleted =
            SaveExtensionGameplay::continue_capture(capture.extensionState, remainingBudget());
        const float extensionMilliseconds = extensionTimer.GetElapsed_sec() * 1000.f;
        capture.extensionMilliseconds += extensionMilliseconds;
        capture.mainThreadMilliseconds += extensionMilliseconds;
        capture.maxExtensionCallMilliseconds =
            std::max(capture.maxExtensionCallMilliseconds, extensionMilliseconds);
        capture.maxSliceMilliseconds =
            std::max(capture.maxSliceMilliseconds, sliceTimer.GetElapsed_sec() * 1000.f);
        if (!extensionsCompleted)
            return;
        if (capture.extensionState.failed)
        {
            Msg("! Save-extension capture failed for '%s'; the previous save set was preserved",
                capture.job.saveName.c_str());
            native_remove_file(capture.job.customTempName.c_str());
            if (capture.job.showStatus)
                show_save_status(StringTable().translate("st_game_save_failed").c_str());
            m_save_captures->failed = true;
            m_save_captures->pending.pop_front();
            if (budgetExhausted())
                return;
            continue;
        }
        capture.job.extensionMutations = std::move(capture.extensionState.mutations);
        capture.job.sourceStream = std::move(capture.stream);
        capture.job.captureMilliseconds = capture.mainThreadMilliseconds;

        Msg("* Game %s snapshot queued for asynchronous compression and commit (main-thread work %.3f ms)",
            capture.job.saveName.c_str(), capture.mainThreadMilliseconds);
        Msg("* Save snapshot stages: script %.3f ms, header %.3f ms, spawns %.3f ms, objects %.3f ms, "
            "registry %.3f ms, extensions %.3f ms",
            capture.scriptMilliseconds, capture.headerMilliseconds, capture.spawnMilliseconds,
            capture.objectMilliseconds, capture.registryMilliseconds, capture.extensionMilliseconds);
        Msg("* Save phase call maxima: script %.3f ms, spawns %.3f ms, objects %.3f ms, "
            "registry %.3f ms, extensions %.3f ms",
            capture.maxScriptCallMilliseconds, capture.maxSpawnCallMilliseconds,
            capture.maxObjectCallMilliseconds, capture.maxRegistryCallMilliseconds,
            capture.maxExtensionCallMilliseconds);
        Msg("* Save max-wall object call: %.3f ms, %llu thread cycles, %.3f ms kernel, %.3f ms user",
            capture.maxObjectCallMilliseconds, capture.maxObjectCallCycles,
            double(capture.maxObjectCallKernel100ns) / 10000.0,
            double(capture.maxObjectCallUser100ns) / 10000.0);
        const auto& objectProfile = capture.objectState.heaviestObjectProfile;
        Msg("* Save heaviest object: id %u, %llu cycles (spawn %llu, spawn copy %llu, update %llu, update copy %llu)",
            static_cast<u32>(capture.objectState.heaviestObjectId), capture.objectState.heaviestObjectCycles,
            objectProfile.spawnCycles, objectProfile.spawnCopyCycles,
            objectProfile.updateCycles, objectProfile.updateCopyCycles);
        Msg("* Save frame budget: initial %.3f ms, max follow-up slice %.3f ms, %u slices, %.3f s wall time",
            capture.initialFrameMilliseconds, capture.maxSliceMilliseconds, capture.sliceCount,
            float(Device.TimerAsync() - capture.queuedAt) / 1000.f);

        async_save_coordinator().enqueue(std::move(capture.job));
        m_save_captures->pending.pop_front();

        if (budgetExhausted())
            return;
    }
}

bool CALifeStorageManager::save(LPCSTR save_name_no_check, bool update_name, bool show_status)
{
    if (saveCaptureScopeActive)
    {
        Msg("! Reentrant save capture request was rejected");
        return false;
    }
    if (m_save_captures && !m_save_captures->pending.empty())
    {
        Msg("! Concurrent save capture request was rejected");
        return false;
    }
    const SaveCaptureScope captureScope;
    const DiagnosticOperation operation("save_game");
    pcstr gameSaveExtension = SAVE_EXTENSION;
    if (ShadowOfChernobylMode || ClearSkyMode)
        gameSaveExtension = SAVE_EXTENSION_LEGACY;

    LPCSTR game_saves_path = FS.get_path("$game_saves$")->m_Path;

    std::error_code directoryError;
    const std::filesystem::path saveDirectory(game_saves_path);
    if ((!std::filesystem::exists(saveDirectory, directoryError) &&
            !std::filesystem::create_directories(saveDirectory, directoryError)) ||
        directoryError || !std::filesystem::is_directory(saveDirectory, directoryError))
    {
        const u32 errorCode = directoryError ? directoryError.value() : ERROR_DIRECTORY;
        Msg("! Cannot prepare the save directory (system error %u)", errorCode);
        if (show_status)
            show_save_status(StringTable().translate("st_game_save_failed").c_str());
        return false;
    }

    string_path save_name;
    strncpy_s(save_name, sizeof(save_name), save_name_no_check ? save_name_no_check : "",
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
            return false;
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
    bool scriptIncrementalEncode = false;
    bool scriptCaptureFailed = false;
    float maxScriptCallMilliseconds = 0.f;
    std::vector<u8> customData;
    luabind::functor<bool> prepareBegin;
    luabind::functor<bool> prepareStep;
    luabind::functor<bool> encodeBegin;
    luabind::functor<int> encodeStep;
    luabind::functor<luabind::object> encodeResult;
    luabind::functor<u32> encodeSize;
    luabind::functor<luabind::object> encodeSave;
    const bool incrementalPrepareAvailable = GEnv.ScriptEngine->functor(
            "alife_storage_manager.CALifeStorageManager_capture_prepare_begin", prepareBegin) &&
        GEnv.ScriptEngine->functor(
            "alife_storage_manager.CALifeStorageManager_capture_prepare_step", prepareStep);
    const bool incrementalEncodeAvailable = GEnv.ScriptEngine->functor(
            "alife_storage_manager.CALifeStorageManager_capture_encode_begin", encodeBegin) &&
        GEnv.ScriptEngine->functor(
            "alife_storage_manager.CALifeStorageManager_capture_encode_step", encodeStep) &&
        GEnv.ScriptEngine->functor(
            "alife_storage_manager.CALifeStorageManager_capture_encode_result", encodeResult) &&
        GEnv.ScriptEngine->functor(
            "alife_storage_manager.CALifeStorageManager_capture_encode_size", encodeSize);
    const bool synchronousEncodeAvailable = GEnv.ScriptEngine->functor(
        "alife_storage_manager.CALifeStorageManager_capture_encode", encodeSave);
    const bool incrementalCaptureAvailable =
        incrementalPrepareAvailable && (incrementalEncodeAvailable || synchronousEncodeAvailable);
    if (incrementalCaptureAvailable)
    {
        CTimer scriptCallTimer;
        scriptCallTimer.Start();
        const bool prepared = prepareBegin((pcstr)m_save_name);
        maxScriptCallMilliseconds = scriptCallTimer.GetElapsed_sec() * 1000.f;
        if (prepared)
        {
            scriptPreparePending = true;
            scriptEncodePending = true;
            scriptIncrementalEncode = incrementalEncodeAvailable;
        }
        else
            scriptCaptureFailed = true;
    }
    else
    {
        luabind::functor<bool> prepareSave;
        const bool synchronousCaptureAvailable = GEnv.ScriptEngine->functor(
                "alife_storage_manager.CALifeStorageManager_capture_prepare", prepareSave) &&
            GEnv.ScriptEngine->functor(
                "alife_storage_manager.CALifeStorageManager_capture_encode", encodeSave);
        if (synchronousCaptureAvailable)
        {
            CTimer scriptCallTimer;
            scriptCallTimer.Start();
            const bool prepared = prepareSave((pcstr)m_save_name);
            maxScriptCallMilliseconds = scriptCallTimer.GetElapsed_sec() * 1000.f;
            if (prepared)
                scriptEncodePending = true;
            else
                scriptCaptureFailed = true;
        }
        else
        {
            luabind::functor<luabind::object> captureSave;
            if (GEnv.ScriptEngine->functor(
                    "alife_storage_manager.CALifeStorageManager_capture_save", captureSave))
            {
                CTimer scriptCallTimer;
                scriptCallTimer.Start();
                const luabind::object snapshot = captureSave((pcstr)m_save_name);
                maxScriptCallMilliseconds = scriptCallTimer.GetElapsed_sec() * 1000.f;
                switch (capture_script_custom_data(snapshot, customData))
                {
                case ScriptCustomCaptureResult::Present:
                    customDataExpected = true;
                    break;
                case ScriptCustomCaptureResult::Absent:
                    break;
                case ScriptCustomCaptureResult::Error:
                    scriptCaptureFailed = true;
                    break;
                }
            }
            else
            {
                // Legacy callbacks mutate the final .scoc, so finish any worker commit first.
                async_save_coordinator().wait();
                string_path customPreserved;
                strconcat(sizeof(customPreserved), customPreserved, customName, ".", saveIdText, ".preserved");
                const bool hadCustomData = native_file_exists(customName);
                const std::string captureMarkerName =
                    std::string(finalName) + SaveTransactionMarker::suffix;
                SaveTransactionMarker::State captureMarker;
                captureMarker.saveId = saveId;
                captureMarker.flags = SaveTransactionMarker::legacyCustomCapture;
                if (native_file_exists(finalName))
                    captureMarker.flags |= SaveTransactionMarker::previousMainPresent;
                if (hadCustomData)
                    captureMarker.flags |= SaveTransactionMarker::previousCustomPresent;
                xr_vector<u8> captureMarkerContents;
                SaveTransactionMarker::build(captureMarker, captureMarkerContents);
                bool legacyCaptureReady = native_write_file(captureMarkerName.c_str(),
                    captureMarkerContents.data(), captureMarkerContents.size());
                if (legacyCaptureReady && hadCustomData)
                    legacyCaptureReady = native_copy_file(customName, customPreserved);
                if (!legacyCaptureReady)
                {
                    if (hadCustomData && native_file_exists(customPreserved))
                        native_replace_file(customPreserved, customName);
                    native_remove_file(captureMarkerName.c_str());
                    xr_strcpy(m_save_name, saveBackup);
                    if (show_status)
                        show_save_status(StringTable().translate("st_game_save_failed").c_str());
                    return false;
                }

                luabind::functor<void> beforeSave;
                if (GEnv.ScriptEngine->functor(
                        "alife_storage_manager.CALifeStorageManager_before_save", beforeSave) &&
                    legacyCaptureReady)
                {
                    beforeSave((pcstr)m_save_name, (pcstr)saveIdText);
                }

                string_path legacyTempName;
                strconcat(sizeof(legacyTempName), legacyTempName, customName, ".tmp");
                if (native_file_exists(customTempName.c_str()))
                    customDataExpected = true;
                else if (native_file_exists(legacyTempName))
                {
                    customDataExpected = true;
                    legacyCaptureReady = native_replace_file(legacyTempName, customTempName.c_str());
                }
                else if (native_file_exists(customName))
                {
                    customDataExpected = true;
                    legacyCaptureReady = native_copy_file(customName, customTempName.c_str());
                }

                const bool customRestored = hadCustomData ?
                    native_replace_file(customPreserved, customName) : native_remove_file(customName);
                const bool captureMarkerRemoved =
                    customRestored && native_remove_file(captureMarkerName.c_str());
                legacyCaptureReady =
                    legacyCaptureReady && customRestored && captureMarkerRemoved;
                if (!legacyCaptureReady)
                {
                    native_remove_file(customTempName.c_str());
                    xr_strcpy(m_save_name, saveBackup);
                    if (show_status)
                        show_save_status(StringTable().translate("st_game_save_failed").c_str());
                    return false;
                }

                if (customDataExpected)
                {
                    SaveExtensionContainer::FileSignature customSignature;
                    if (SaveExtensionContainer::read_file_signature(
                            customTempName.c_str(), customSignature) !=
                        SaveExtensionContainer::SignatureResult::Valid)
                    {
                        native_remove_file(customTempName.c_str());
                        xr_strcpy(m_save_name, saveBackup);
                        if (show_status)
                            show_save_status(StringTable().translate("st_game_save_failed").c_str());
                        return false;
                    }
                    else
                        customTempPrepared = true;
                }
            }
        }
    }
    if (scriptCaptureFailed)
    {
        Msg("! Script save capture failed for '%s'; the previous save set was preserved", m_save_name);
        native_remove_file(customTempName.c_str());
        xr_strcpy(m_save_name, saveBackup);
        if (show_status)
            show_save_status(StringTable().translate("st_game_save_failed").c_str());
        return false;
    }
    const float scriptMilliseconds = stageTimer.GetElapsed_sec() * 1000.f;

    constexpr size_t minimumSaveReserve = 2 * 1024 * 1024;
    constexpr size_t estimatedSaveBytesPerObject = 256;
    auto capture = std::make_unique<SaveCaptureQueue::Pending>();
    capture->stream = std::make_unique<CMemoryWriter>();
    capture->objectPacket = std::make_unique<NET_Packet>();
    capture->stream->reserve(std::max<size_t>(
        minimumSaveReserve, objects().objects().size() * estimatedSaveBytesPerObject));
    capture->job.saveName = m_save_name;
    capture->job.finalName = finalName;
    capture->job.customTempName = customTempName;
    capture->job.customData = std::move(customData);
    capture->job.saveId = saveId;
    capture->job.customDataExpected = customDataExpected;
    capture->job.customTempPrepared = customTempPrepared;
    capture->job.showStatus = show_status;
    capture->scriptMilliseconds = scriptMilliseconds;
    capture->maxScriptCallMilliseconds = maxScriptCallMilliseconds;
    capture->scriptPreparePending = scriptPreparePending;
    capture->scriptEncodePending = scriptEncodePending;
    capture->scriptIncrementalEncode = scriptIncrementalEncode;
    capture->phase = scriptPreparePending ? 0 : scriptEncodePending ? 1 : 2;

    stageTimer.Start();
    header().save(*capture->stream);
    time_manager().save(*capture->stream);
    capture->headerMilliseconds = stageTimer.GetElapsed_sec() * 1000.f;

    capture->initialFrameMilliseconds = captureTimer.GetElapsed_sec() * 1000.f;
    capture->mainThreadMilliseconds = capture->initialFrameMilliseconds;
    capture->queuedAt = Device.TimerAsync();
    if (!m_save_captures)
        m_save_captures = std::make_unique<SaveCaptureQueue>();
    m_save_captures->pending.emplace_back(std::move(capture));
    activeStorageManager = this;

    if (!update_name)
        xr_strcpy(m_save_name, saveBackup);
    return true;
}

void CALifeStorageManager::load(void* buffer, const u32& buffer_size, LPCSTR file_name, u64 save_id,
    bool custom_present, std::span<const u8> custom_data,
    bool custom_backup_present, std::span<const u8> custom_backup_data)
{
    string32 saveIdText;
    xr_sprintf(saveIdText, "%016llx", static_cast<unsigned long long>(save_id));

    luabind::functor<void> funct;
    if (GEnv.ScriptEngine->functor("alife_storage_manager.CALifeStorageManager_load", funct))
    {
        const auto makeLuaString = [](std::span<const u8> contents)
        {
            if (contents.empty())
                return luabind::string{};
            return luabind::string(reinterpret_cast<const char*>(contents.data()), contents.size());
        };
        const luabind::string customData = makeLuaString(custom_data);
        const luabind::string customBackupData = makeLuaString(custom_backup_data);
        funct(file_name, static_cast<pcstr>(saveIdText), custom_present, customData,
            custom_backup_present, customBackupData);
    }

    IReader source(buffer, buffer_size);
    header().load(source);
    time_manager().load(source);
    spawns().load(source, file_name);
    graph().on_load();
    objects().load(source);

    // XMS: when the loader skipped objects of removed modules, their children
    // reference missing parent ids - drop those subtrees before registration
    if (XmsGame::SkippedObjectCount())
    {
        auto& object_map = objects().objects();
        u32 pruned = 0;
        for (bool removed_any = true; removed_any;)
        {
            removed_any = false;
            for (auto it = object_map.begin(); it != object_map.end(); ++it)
            {
                CSE_ALifeDynamicObject* object = it->second;
                if (object->ID_Parent == 0xffff || object_map.find(object->ID_Parent) != object_map.end())
                    continue;
                CSE_Abstract* abstract = object;
                F_entity_Destroy(abstract);
                object_map.erase(it);
                removed_any = true;
                ++pruned;
                break; // iterator invalidated
            }
        }
        if (pruned)
            Msg("! XMS: pruned %u child object(s) of skipped parents", pruned);
    }

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

    // XMS: a module updated since this save was written has its new spawn
    // vertices composed (spawns().load above runs the composer on this path
    // too) but no objects. Instantiate what this playthrough never applied -
    // still inside the deferred-on_register window, so the new objects join
    // the uniform on_register pass below exactly like on a new game.
    XmsGame::LateSpawnCompose(*this);

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

bool CALifeStorageManager::validate_load_companions(LPCSTR save_name)
{
    if (!save_name || !save_name[0])
        return false;

    string_path mainName;
    CSavedGameWrapper::saved_game_full_name(save_name, mainName, SAVE_EXTENSION);
    if (!FS.exist(mainName))
    {
        CSavedGameWrapper::saved_game_full_name(save_name, mainName, SAVE_EXTENSION_LEGACY);
        if (!FS.exist(mainName))
            return false;
    }

    SaveExtensionContainer::FileSignature scopSignature;
    if (SaveExtensionContainer::read_file_signature(mainName, scopSignature) !=
        SaveExtensionContainer::SignatureResult::Valid)
    {
        Msg("! Cannot read saved game '%s'", mainName);
        return false;
    }

    string_path customName;
    make_custom_save_name(mainName, customName);
    SaveExtensionContainer::FileSignature scocSignature;
    if (SaveExtensionContainer::read_file_signature(customName, scocSignature) ==
        SaveExtensionContainer::SignatureResult::Error)
    {
        Msg("! Cannot read custom save companion '%s'", customName);
        return false;
    }

    string_path sidecarName;
    make_companion_save_name(mainName, SaveExtensionContainer::extension, sidecarName);
    SaveExtensionContainer::Container extensions;
    const auto extensionResult =
        SaveExtensionContainer::load(sidecarName, scopSignature, scocSignature, extensions);
    if (extensionResult == SaveExtensionContainer::LoadResult::IoError ||
        extensionResult == SaveExtensionContainer::LoadResult::Invalid)
    {
        Msg("! Cannot load save extensions '%s'", sidecarName);
        return false;
    }
    return true;
}

bool CALifeStorageManager::load(LPCSTR save_name_no_check)
{
    const DiagnosticOperation operation("load_game");
    if (save_capture_reentrant())
    {
        Msg("! Reentrant game load request was rejected during save capture");
        return false;
    }
    wait_for_pending_saves();
    if (!CSavedGameWrapper::recover_interrupted_save(save_name_no_check))
    {
        Msg("! Cannot recover the interrupted save transaction for '%s'", save_name_no_check);
        return false;
    }

    pcstr gameSaveExtension = SAVE_EXTENSION;
    if (ShadowOfChernobylMode || ClearSkyMode)
        gameSaveExtension = SAVE_EXTENSION_LEGACY;

    LPCSTR game_saves_path = FS.get_path("$game_saves$")->m_Path;

    string_path save_name;
    strncpy_s(save_name, sizeof(save_name), save_name_no_check ? save_name_no_check : "",
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

    string_path requestedSaveName;
    if (save_name[0])
        xr_strcpy(requestedSaveName, save_name);
    else
        xr_strcpy(requestedSaveName, std::filesystem::path(m_save_name).stem().string().c_str());

    string_path file_name;
    FS.update_path(file_name, "$game_saves$", m_save_name);
    if (!FS.exist(file_name) && CSavedGameWrapper::valid_saved_game(requestedSaveName))
    {
        CSavedGameWrapper::saved_game_full_name(requestedSaveName, file_name, SAVE_EXTENSION);
        if (!FS.exist(file_name))
            CSavedGameWrapper::saved_game_full_name(requestedSaveName, file_name, SAVE_EXTENSION_LEGACY);
    }

    string_path previous_report_save{};
    if (*saveBackup)
        FS.update_path(previous_report_save, "$game_saves$", saveBackup);

    // Keep the attempted save available to the crash handler, but restore the active save after a normal load failure.
    struct ReportSaveGuard
    {
        string_path previous{};
        bool loaded{};

        ReportSaveGuard(pcstr oldPath, pcstr newPath)
        {
            xr_strcpy(previous, oldPath ? oldPath : "");
            CrashReporter::SetSavePath(newPath);
        }

        ~ReportSaveGuard()
        {
            if (!loaded)
                CrashReporter::SetSavePath(previous);
        }
    } reportSave(previous_report_save, file_name);

    xr_strcpy(g_last_saved_game, requestedSaveName);

    constexpr pcstr mismatch = "Saved game version mismatch or saved game is corrupted";
    CSavedGameWrapper::PreparedSource preparedSource;
    CSavedGameWrapper::SaveMetadata metadata;
    SaveExtensionContainer::FileSignature scopSignature;
    u64 preparedScopSize = 0;
    u32 preparedScopChecksum = 0;
    const CSavedGameWrapper::PreparedLoadResult preparedResult = CSavedGameWrapper::consume_prepared_load(
        requestedSaveName, preparedSource, metadata, preparedScopSize, preparedScopChecksum);
    if (preparedResult != CSavedGameWrapper::PreparedLoadResult::Ready)
    {
        if (preparedResult == CSavedGameWrapper::PreparedLoadResult::Invalid)
            Msg("! %s [%s]", mismatch, file_name);
        else
            Msg("! Cannot acquire an immutable saved game snapshot [%s]", file_name);
        xr_strcpy(m_save_name, saveBackup);
        return false;
    }

    xr_strcpy(file_name, preparedSource.path);
    const std::string canonicalFileName = std::filesystem::path(file_name).filename().string();
    const std::string activeSaveExtension = std::filesystem::path(file_name).extension().string();
    xr_strcpy(m_save_name, canonicalFileName.c_str());
    CrashReporter::SetSavePath(file_name);

    string512 temp;
    strconcat(temp, StringTable().translate("st_loading_saved_game").c_str(),
        " \"", requestedSaveName, activeSaveExtension.c_str(), "\"");

    g_pGamePersistent->LoadTitle(temp);

    const u32 source_count = static_cast<u32>(metadata.sourceSize);

    void* source_data = const_cast<u8*>(preparedSource.data);
    scopSignature = {preparedScopSize, preparedScopChecksum, true};

    const auto companionContents = [](const CSavedGameWrapper::PreparedCompanion& companion)
    {
        return std::span<const u8>(companion.data, companion.size);
    };

    SaveExtensionContainer::ChunkList loadedExtensionChunks;
    SaveExtensionContainer::FileSignature scocSignature;
    string_path customName;
    make_custom_save_name(file_name, customName);
    if (preparedSource.custom.present)
        scocSignature = SaveExtensionContainer::make_signature(companionContents(preparedSource.custom));

    string_path sidecarName;
    make_companion_save_name(file_name, SaveExtensionContainer::extension, sidecarName);
    SaveExtensionContainer::Container extensions;
    const auto extensionResult = preparedSource.extensions.present ?
        SaveExtensionContainer::load(
            companionContents(preparedSource.extensions), scopSignature, scocSignature, extensions) :
        SaveExtensionContainer::LoadResult::Missing;
    if (extensionResult == SaveExtensionContainer::LoadResult::Invalid)
    {
        Msg("! Cannot load save extensions '%s'", sidecarName);
        xr_strcpy(m_save_name, saveBackup);
        return false;
    }
    if (extensionResult == SaveExtensionContainer::LoadResult::Valid)
    {
        if (extensions.skippedChunks)
            Msg("! Ignored %u corrupted save-extension chunks in '%s'", extensions.skippedChunks, sidecarName);
        loadedExtensionChunks = std::move(extensions.chunks);
    }
    else if (extensionResult == SaveExtensionContainer::LoadResult::SignatureMismatch)
    {
        Msg("! Ignored stale save extensions in '%s'", sidecarName);
    }

    unload();
    reload(m_section);

    SaveExtensionGameplay::stage_chunks(loadedExtensionChunks);
    // XMS: module manifest diff + per-module blob snapshot
    XmsGame::OnSnapshotLoaded(loadedExtensionChunks);
    SaveExtensionContainer::set_loaded_chunks(std::move(loadedExtensionChunks));

    const u64 effectiveSaveId = metadata.protectedFormat ||
            extensionResult != SaveExtensionContainer::LoadResult::Valid ?
        metadata.saveId : extensions.binding.saveId;
    load(source_data, source_count, file_name, effectiveSaveId,
        preparedSource.custom.present, companionContents(preparedSource.custom),
        preparedSource.customBackup.present, companionContents(preparedSource.customBackup));

    groups().on_after_game_load();

    VERIFY(graph().actor());

    Msg("* Game %s is successfully loaded from file '%s' (%.3fs)",
        requestedSaveName, file_name, timer.GetElapsed_sec());
    reportSave.loaded = true;

    return (true);
}

void CALifeStorageManager::save(NET_Packet& net_packet)
{
    shared_str game_name;
    net_packet.r_stringZ(game_name);
    if (!m_save_requests)
        m_save_requests = std::make_unique<SaveRequestQueue>();

    SaveRequestQueue::Pending request;
    request.saveName = game_name.c_str();
    request.updateName = !!net_packet.r_u8();
    request.showStatus = net_packet.r_eof() || !!net_packet.r_u8();
    const u32 objectCount = Level().Objects.o_count();
    request.clientObjectIds.reserve(objectCount);
    // Stable IDs keep a multi-frame capture independent of active/sleeping list reshuffles.
    for (u32 objectIndex = 0; objectIndex < objectCount; ++objectIndex)
    {
        if (IGameObject* object = Level().Objects.o_get_by_iterator(objectIndex))
            request.clientObjectIds.push_back(u16(object->ID()));
    }
    m_save_requests->pending.emplace_back(std::move(request));
    activeStorageManager = this;
}
