////////////////////////////////////////////////////////////////////////////
//	Module 		: saved_game_wrapper.cpp
//	Created 	: 21.02.2006
//  Modified 	: 21.02.2006
//	Author		: Dmitriy Iassenev
//	Description : saved game wrapper class
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "saved_game_wrapper.h"
#include "alife_time_manager.h"
#include "alife_object_registry.h"
#include "xrServer_Objects_ALife_Monsters.h"
#include "ai_space.h"
#include "xrAICore/Navigation/game_graph.h"
#include "alife_simulator_header.h"
#include "alife_simulator.h"
#include "alife_spawn_registry.h"
#include "alife_storage_manager.h"
#include "save_extension_container.h"
#include "save_transaction_marker.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <thread>

extern LPCSTR alife_section;

namespace
{
constexpr u32 saveFooterMagic = 0x53343644;
constexpr u32 saveFooterVersion = 1;
constexpr u32 saveProtectedSourceFlag = 0x80000000;
constexpr size_t saveFooterSize = sizeof(u32) * 5 + sizeof(u64);
static_assert(CSavedGameWrapper::maxSourceSize <= std::numeric_limits<u32>::max());

struct SaveFileIdentity
{
    xr_string path;
    SaveExtensionContainer::FileSignature signature;
    u64 storageId{};
    std::array<u8, 16> fileId{};
    u64 fileSize{};
    u64 creationTime{};
    u64 lastWriteTime{};
    u64 changeTime{};
    bool strong{};
};

struct SaveSnapshot
{
    SaveFileIdentity identity;
    const u8* data{};

#if defined(XR_PLATFORM_WINDOWS)
    HANDLE file{INVALID_HANDLE_VALUE};
    HANDLE mapping{};
#else
    int file{-1};
    std::unique_ptr<u8[]> ownedData;
#endif

    SaveSnapshot() = default;
    SaveSnapshot(const SaveSnapshot&) = delete;
    SaveSnapshot& operator=(const SaveSnapshot&) = delete;

    ~SaveSnapshot()
    {
#if defined(XR_PLATFORM_WINDOWS)
        if (data)
            UnmapViewOfFile(data);
        if (mapping)
            CloseHandle(mapping);
        if (file != INVALID_HANDLE_VALUE)
            CloseHandle(file);
#else
        if (file >= 0)
            close(file);
#endif
    }

    bool map()
    {
        if (data)
            return true;
        if (!identity.fileSize)
            return false;

#if defined(XR_PLATFORM_WINDOWS)
        mapping = CreateFileMappingA(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!mapping)
            return false;
        data = static_cast<const u8*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
#else
        ownedData = std::make_unique_for_overwrite<u8[]>(static_cast<size_t>(identity.fileSize));
        size_t offset = 0;
        while (offset != identity.fileSize)
        {
            const ssize_t bytesRead = pread(file, ownedData.get() + offset,
                static_cast<size_t>(identity.fileSize) - offset, static_cast<off_t>(offset));
            if (bytesRead < 0 && errno == EINTR)
                continue;
            if (bytesRead <= 0)
            {
                ownedData.reset();
                return false;
            }
            offset += static_cast<size_t>(bytesRead);
        }
        data = ownedData.get();
#endif
        return data;
    }
};

using SaveSnapshotPtr = std::shared_ptr<SaveSnapshot>;

struct PreparedSaveData
{
    enum class Status
    {
        Unavailable,
        Invalid,
        Stale,
        Ready
    };

    xr_string name;
    SaveFileIdentity identity;
    std::unique_ptr<u8[]> source;
    SaveSnapshotPtr snapshot;
    CSavedGameWrapper::SaveMetadata metadata;
    Status status{};
};

using PreparedSavePtr = std::shared_ptr<PreparedSaveData>;

std::mutex preparedSaveMutex;
std::condition_variable preparedSaveCondition;
std::mutex saveRecoveryMutex;
std::shared_future<PreparedSavePtr> preparedSaveFuture;
SaveFileIdentity preparedSaveIdentity;
bool preparedSaveIdentityValid{};
bool preparedSaveReplacing{};

struct PreparedLeaseState
{
    PreparedSavePtr prepared;
    SaveSnapshotPtr pathGuard;
    std::array<SaveSnapshotPtr, 3> companionGuards;
};

bool valid_saved_game_file(pcstr fileName)
{
    IReader* stream = FS.r_open(fileName);
    if (!stream)
        return false;

    const bool valid = CSavedGameWrapper::valid_saved_game(*stream);
    FS.r_close(stream);
    return valid;
}

bool recovery_file_exists(pcstr path)
{
#if defined(XR_PLATFORM_WINDOWS)
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
#else
    return access(path, F_OK) == 0;
#endif
}

bool recovery_remove_file(pcstr path)
{
#if defined(XR_PLATFORM_WINDOWS)
    if (DeleteFileA(path))
        return true;
    const DWORD error = GetLastError();
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
#else
    return unlink(path) == 0 || errno == ENOENT;
#endif
}

bool recovery_replace_file(pcstr source, pcstr destination)
{
#if defined(XR_PLATFORM_WINDOWS)
    return MoveFileExA(source, destination, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
#else
    return rename(source, destination) == 0;
#endif
}

bool recovery_flush_file(pcstr path)
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

bool recovery_copy_file(pcstr source, pcstr destination)
{
#if defined(XR_PLATFORM_WINDOWS)
    return CopyFileA(source, destination, FALSE) != FALSE && recovery_flush_file(destination);
#else
    std::error_code error;
    std::filesystem::copy_file(
        source, destination, std::filesystem::copy_options::overwrite_existing, error);
    return !error && recovery_flush_file(destination);
#endif
}

void refresh_recovered_save_index()
{
    FS.get_path("$game_saves$")->m_Flags.set(FS_Path::flNeedRescan, true);
    FS.m_Flags.set(CLocatorAPI::flNeedCheck, true);
    FS.rescan_pathes();
    FS.m_Flags.set(CLocatorAPI::flNeedCheck, false);
}

void make_companion_name(pcstr mainName, pcstr extensionName, string_path& result)
{
    xr_strcpy(result, mainName);
    if (char* extension = strrchr(result, '.'))
        xr_strcpy(extension, sizeof(result) - (extension - result), extensionName);
}

bool validate_recovery_set(pcstr mainName, pcstr customName, pcstr sidecarName,
    bool& customExpected, bool& sidecarExpected)
{
    if (!valid_saved_game_file(mainName))
        return false;

    SaveExtensionContainer::FileSignature customSignature;
    if (SaveExtensionContainer::read_file_signature(customName, customSignature) ==
        SaveExtensionContainer::SignatureResult::Error)
    {
        return false;
    }
    customExpected = customSignature.present;
    sidecarExpected = recovery_file_exists(sidecarName);
    return true;
}

bool restore_backup_set(pcstr mainName)
{
    const std::string mainBackup = std::string(mainName) + ".bak";
    if (!recovery_file_exists(mainBackup.c_str()))
        return false;

    string_path customName;
    make_companion_name(mainName, ".scoc", customName);
    const std::string customBackup = std::string(customName) + ".bak";
    string_path sidecarName;
    make_companion_name(mainName, SaveExtensionContainer::extension, sidecarName);
    const std::string sidecarBackup = std::string(sidecarName) + ".bak";

    bool customExpected = false;
    bool sidecarExpected = false;
    if (!validate_recovery_set(mainBackup.c_str(), customBackup.c_str(), sidecarBackup.c_str(),
            customExpected, sidecarExpected))
    {
        return false;
    }

    const std::string mainRecovery = std::string(mainName) + ".recover";
    const std::string customRecovery = std::string(customName) + ".recover";
    const std::string sidecarRecovery = std::string(sidecarName) + ".recover";
    if (!recovery_remove_file(mainRecovery.c_str()) || !recovery_remove_file(customRecovery.c_str()) ||
        !recovery_remove_file(sidecarRecovery.c_str()) ||
        !recovery_copy_file(mainBackup.c_str(), mainRecovery.c_str()) ||
        (customExpected && !recovery_copy_file(customBackup.c_str(), customRecovery.c_str())) ||
        (sidecarExpected && !recovery_copy_file(sidecarBackup.c_str(), sidecarRecovery.c_str())))
    {
        return false;
    }

    bool recoveredCustomExpected = false;
    bool recoveredSidecarExpected = false;
    if (!validate_recovery_set(mainRecovery.c_str(), customRecovery.c_str(), sidecarRecovery.c_str(),
            recoveredCustomExpected, recoveredSidecarExpected) ||
        recoveredCustomExpected != customExpected || recoveredSidecarExpected != sidecarExpected)
    {
        return false;
    }

    // Companion files are installed first; the main save remains the commit marker.
    if ((customExpected && !recovery_replace_file(customRecovery.c_str(), customName)) ||
        (!customExpected && !recovery_remove_file(customName)) ||
        (sidecarExpected && !recovery_replace_file(sidecarRecovery.c_str(), sidecarName)) ||
        (!sidecarExpected && !recovery_remove_file(sidecarName)) ||
        !recovery_replace_file(mainRecovery.c_str(), mainName))
    {
        return false;
    }
    return valid_saved_game_file(mainName);
}

void make_transaction_file_names(pcstr mainName, const SaveTransactionMarker::State& marker,
    std::string& mainTemp, std::string& customTemp, std::string& sidecarTemp,
    string_path& customName, string_path& sidecarName)
{
    string32 saveIdText;
    xr_sprintf(saveIdText, "%016llx", static_cast<unsigned long long>(marker.saveId));
    mainTemp = std::string(mainName) + "." + saveIdText + ".tmp";
    make_companion_name(mainName, ".scoc", customName);
    customTemp = std::string(customName) + "." + saveIdText + ".tmp";
    make_companion_name(mainName, SaveExtensionContainer::extension, sidecarName);
    sidecarTemp = std::string(sidecarName) + "." + saveIdText + ".tmp";
}

void cleanup_transaction_temps(pcstr mainName, const SaveTransactionMarker::State& marker)
{
    std::string mainTemp;
    std::string customTemp;
    std::string sidecarTemp;
    string_path customName;
    string_path sidecarName;
    make_transaction_file_names(
        mainName, marker, mainTemp, customTemp, sidecarTemp, customName, sidecarName);
    recovery_remove_file(mainTemp.c_str());
    recovery_remove_file(customTemp.c_str());
    recovery_remove_file(sidecarTemp.c_str());
}

bool recover_legacy_custom_capture(pcstr mainName, const SaveTransactionMarker::State& marker)
{
    string_path customName;
    make_companion_name(mainName, ".scoc", customName);
    string32 saveIdText;
    xr_sprintf(saveIdText, "%016llx", static_cast<unsigned long long>(marker.saveId));
    const std::string preservedName = std::string(customName) + "." + saveIdText + ".preserved";
    bool customRestored = false;
    if (marker.flags & SaveTransactionMarker::previousCustomPresent)
    {
        customRestored = recovery_file_exists(preservedName.c_str()) ?
            recovery_replace_file(preservedName.c_str(), customName) : recovery_file_exists(customName);
    }
    else
    {
        customRestored = recovery_remove_file(customName);
    }
    if (!customRestored)
        return false;

    cleanup_transaction_temps(mainName, marker);
    recovery_remove_file((std::string(customName) + ".tmp").c_str());
    recovery_remove_file(preservedName.c_str());
    return true;
}

bool restore_marked_backup_set(pcstr mainName, const SaveTransactionMarker::State& marker)
{
    if (!(marker.flags & SaveTransactionMarker::previousMainPresent))
        return false;

    string_path customName;
    make_companion_name(mainName, ".scoc", customName);
    string_path sidecarName;
    make_companion_name(mainName, SaveExtensionContainer::extension, sidecarName);
    const bool mainBackupPresent = recovery_file_exists((std::string(mainName) + ".bak").c_str());
    const bool customBackupPresent = recovery_file_exists((std::string(customName) + ".bak").c_str());
    const bool sidecarBackupPresent = recovery_file_exists((std::string(sidecarName) + ".bak").c_str());
    if (!mainBackupPresent || customBackupPresent !=
            !!(marker.flags & SaveTransactionMarker::previousCustomPresent) ||
        sidecarBackupPresent != !!(marker.flags & SaveTransactionMarker::previousSidecarPresent))
    {
        return false;
    }
    return restore_backup_set(mainName);
}

bool choose_transaction_source(const std::string& tempName, pcstr finalName, std::string& source)
{
    if (recovery_file_exists(tempName.c_str()))
    {
        source = tempName;
        return true;
    }
    if (!recovery_file_exists(finalName))
        return false;
    source = finalName;
    return true;
}

bool finish_new_save_set(pcstr mainName, const SaveTransactionMarker::State& marker)
{
    if (marker.flags & (SaveTransactionMarker::previousMainPresent |
            SaveTransactionMarker::legacyCustomCapture))
        return false;

    std::string mainTemp;
    std::string customTemp;
    std::string sidecarTemp;
    string_path customName;
    string_path sidecarName;
    make_transaction_file_names(
        mainName, marker, mainTemp, customTemp, sidecarTemp, customName, sidecarName);
    if (!valid_saved_game_file(mainTemp.c_str()))
        return false;

    const bool customExpected = !!(marker.flags & SaveTransactionMarker::customDataExpected);
    std::string customSource;
    if (customExpected && !choose_transaction_source(customTemp, customName, customSource))
        return false;
    std::string sidecarSource;
    if (!choose_transaction_source(sidecarTemp, sidecarName, sidecarSource))
        return false;

    SaveExtensionContainer::FileSignature mainSignature;
    SaveExtensionContainer::FileSignature customSignature;
    if (SaveExtensionContainer::read_file_signature(mainTemp.c_str(), mainSignature) !=
            SaveExtensionContainer::SignatureResult::Valid ||
        (customExpected && SaveExtensionContainer::read_file_signature(
            customSource.c_str(), customSignature) != SaveExtensionContainer::SignatureResult::Valid))
    {
        return false;
    }

    SaveExtensionContainer::Container extensions;
    if (SaveExtensionContainer::load(
            sidecarSource.c_str(), mainSignature, customSignature, extensions) !=
            SaveExtensionContainer::LoadResult::Valid || extensions.binding.saveId != marker.saveId ||
        extensions.binding.scoc.present != customExpected)
    {
        return false;
    }

    const std::string mainRecovery = std::string(mainName) + ".recover";
    const std::string customRecovery = std::string(customName) + ".recover";
    const std::string sidecarRecovery = std::string(sidecarName) + ".recover";
    if (!recovery_remove_file(mainRecovery.c_str()) || !recovery_remove_file(customRecovery.c_str()) ||
        !recovery_remove_file(sidecarRecovery.c_str()) ||
        !recovery_copy_file(mainTemp.c_str(), mainRecovery.c_str()) ||
        (customExpected && !recovery_copy_file(customSource.c_str(), customRecovery.c_str())) ||
        !recovery_copy_file(sidecarSource.c_str(), sidecarRecovery.c_str()))
    {
        return false;
    }

    SaveExtensionContainer::FileSignature recoveredMainSignature;
    SaveExtensionContainer::FileSignature recoveredCustomSignature;
    SaveExtensionContainer::Container recoveredExtensions;
    if (!valid_saved_game_file(mainRecovery.c_str()) ||
        SaveExtensionContainer::read_file_signature(mainRecovery.c_str(), recoveredMainSignature) !=
            SaveExtensionContainer::SignatureResult::Valid || recoveredMainSignature != mainSignature ||
        (customExpected &&
            (SaveExtensionContainer::read_file_signature(customRecovery.c_str(), recoveredCustomSignature) !=
                    SaveExtensionContainer::SignatureResult::Valid ||
                recoveredCustomSignature != customSignature)) ||
        SaveExtensionContainer::load(sidecarRecovery.c_str(), recoveredMainSignature,
            recoveredCustomSignature, recoveredExtensions) != SaveExtensionContainer::LoadResult::Valid ||
        recoveredExtensions.binding.saveId != marker.saveId)
    {
        return false;
    }

    if ((customExpected && !recovery_replace_file(customRecovery.c_str(), customName)) ||
        (!customExpected && !recovery_remove_file(customName)) ||
        !recovery_replace_file(sidecarRecovery.c_str(), sidecarName) ||
        !recovery_replace_file(mainRecovery.c_str(), mainName))
    {
        return false;
    }
    return valid_saved_game_file(mainName);
}

bool recover_interrupted_transaction(pcstr mainName)
{
    const std::string markerName = std::string(mainName) + SaveTransactionMarker::suffix;
    SaveTransactionMarker::State marker;
    const SaveTransactionMarker::ReadResult markerResult =
        SaveTransactionMarker::read(markerName.c_str(), marker);
    if (markerResult == SaveTransactionMarker::ReadResult::Missing)
        return false;
    if (markerResult == SaveTransactionMarker::ReadResult::Invalid && valid_saved_game_file(mainName))
    {
        if (!recovery_remove_file(markerName.c_str()))
            Msg("! Invalid save transaction marker could not be removed '%s'", markerName.c_str());
        else
            Msg("! Removed an incomplete save transaction marker '%s'", markerName.c_str());
        return true;
    }
    if (markerResult != SaveTransactionMarker::ReadResult::Valid)
    {
        Msg("! Invalid or unreadable save transaction marker blocked recovery '%s'", markerName.c_str());
        return false;
    }

    bool recovered = false;
    if (marker.flags & SaveTransactionMarker::legacyCustomCapture)
        recovered = recover_legacy_custom_capture(mainName, marker);
    else
        recovered = valid_saved_game_file(mainName);
    if (!recovered && (marker.flags & SaveTransactionMarker::previousMainPresent))
        recovered = restore_marked_backup_set(mainName, marker);
    if (!recovered)
        recovered = finish_new_save_set(mainName, marker);
    if (!recovered)
    {
        Msg("! Could not recover interrupted save transaction '%s'", markerName.c_str());
        return false;
    }

    cleanup_transaction_temps(mainName, marker);
    if (!recovery_remove_file(markerName.c_str()))
    {
        Msg("! Save transaction marker could not be removed after recovery '%s'", markerName.c_str());
        return true;
    }
    Msg("! Recovered interrupted save transaction for '%s'", mainName);
    return true;
}

bool same_identity_metadata(const SaveFileIdentity& left, const SaveFileIdentity& right)
{
    return left.path == right.path && left.storageId == right.storageId && left.fileId == right.fileId &&
        left.fileSize == right.fileSize && left.creationTime == right.creationTime &&
        left.lastWriteTime == right.lastWriteTime && left.changeTime == right.changeTime &&
        left.strong == right.strong;
}

bool same_identity(const SaveFileIdentity& left, const SaveFileIdentity& right)
{
    if (!same_identity_metadata(left, right))
        return false;
    return left.strong ||
        (left.signature.present && right.signature.present && left.signature == right.signature);
}

bool query_save_identity(
#if defined(XR_PLATFORM_WINDOWS)
    HANDLE file,
#else
    int file,
#endif
    pcstr fileName, SaveFileIdentity& identity)
{
    identity = {};
    identity.path = std::filesystem::path(fileName).lexically_normal().string();
#if defined(XR_PLATFORM_WINDOWS)
    FILE_BASIC_INFO basic{};
    FILE_STANDARD_INFO standard{};
    if (!GetFileInformationByHandleEx(file, FileBasicInfo, &basic, sizeof(basic)) ||
        !GetFileInformationByHandleEx(file, FileStandardInfo, &standard, sizeof(standard)) ||
        standard.Directory || standard.DeletePending || standard.EndOfFile.QuadPart < 0 ||
        static_cast<u64>(standard.EndOfFile.QuadPart) > std::numeric_limits<u32>::max())
    {
        return false;
    }

    struct StrongFileIdInfo
    {
        u64 volumeSerialNumber;
        std::array<u8, 16> fileId;
    };

    // FileIdInfo is hidden by older target macros but retains this ABI on supported Windows versions.
    constexpr FILE_INFO_BY_HANDLE_CLASS fileIdInfoClass = static_cast<FILE_INFO_BY_HANDLE_CLASS>(18);
    StrongFileIdInfo fileId{};
    const bool fileIdAvailable =
        GetFileInformationByHandleEx(file, fileIdInfoClass, &fileId, sizeof(fileId)) != FALSE;
    identity.strong = fileIdAvailable &&
        std::ranges::any_of(fileId.fileId, [](const u8 value) { return value != 0; });
    if (identity.strong)
    {
        identity.storageId = fileId.volumeSerialNumber;
        identity.fileId = fileId.fileId;
    }
    else
    {
        BY_HANDLE_FILE_INFORMATION fallback{};
        if (!GetFileInformationByHandle(file, &fallback))
            return false;
        identity.storageId = fallback.dwVolumeSerialNumber;
        const u64 fallbackId =
            (static_cast<u64>(fallback.nFileIndexHigh) << 32) | fallback.nFileIndexLow;
        std::memcpy(identity.fileId.data(), &fallbackId, sizeof(fallbackId));
    }

    identity.fileSize = static_cast<u64>(standard.EndOfFile.QuadPart);
    identity.creationTime = static_cast<u64>(basic.CreationTime.QuadPart);
    identity.lastWriteTime = static_cast<u64>(basic.LastWriteTime.QuadPart);
    identity.changeTime = static_cast<u64>(basic.ChangeTime.QuadPart);
#else
    struct stat fileInfo{};
    if (fstat(file, &fileInfo) != 0 || fileInfo.st_size < 0 ||
        static_cast<u64>(fileInfo.st_size) > std::numeric_limits<u32>::max())
        return false;

#if defined(XR_PLATFORM_APPLE)
    identity.lastWriteTime = static_cast<u64>(fileInfo.st_mtimespec.tv_sec) * 1000000000ull +
        static_cast<u64>(fileInfo.st_mtimespec.tv_nsec);
    identity.changeTime = static_cast<u64>(fileInfo.st_ctimespec.tv_sec) * 1000000000ull +
        static_cast<u64>(fileInfo.st_ctimespec.tv_nsec);
#else
    identity.lastWriteTime = static_cast<u64>(fileInfo.st_mtim.tv_sec) * 1000000000ull +
        static_cast<u64>(fileInfo.st_mtim.tv_nsec);
    identity.changeTime = static_cast<u64>(fileInfo.st_ctim.tv_sec) * 1000000000ull +
        static_cast<u64>(fileInfo.st_ctim.tv_nsec);
#endif
    identity.storageId = static_cast<u64>(fileInfo.st_dev);
    const u64 fileId = static_cast<u64>(fileInfo.st_ino);
    std::memcpy(identity.fileId.data(), &fileId, sizeof(fileId));
    identity.fileSize = static_cast<u64>(fileInfo.st_size);
#endif
    return true;
}

SaveSnapshotPtr open_save_snapshot(pcstr fileName, bool blockReplacement, bool* missing = nullptr)
{
    if (missing)
        *missing = false;
    auto result = std::make_shared<SaveSnapshot>();
#if defined(XR_PLATFORM_WINDOWS)
    const DWORD sharing = FILE_SHARE_READ | (blockReplacement ? 0 : FILE_SHARE_DELETE);
    result->file = CreateFileA(fileName, GENERIC_READ, sharing, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (result->file == INVALID_HANDLE_VALUE)
    {
        if (missing)
        {
            const DWORD error = GetLastError();
            *missing = error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
        }
        return {};
    }
#else
    static_cast<void>(blockReplacement);
    result->file = open(fileName, O_RDONLY);
    if (result->file < 0)
    {
        if (missing)
            *missing = errno == ENOENT;
        return {};
    }
#endif
    if (!query_save_identity(result->file, fileName, result->identity))
        return {};
    return result;
}

bool ensure_signature(SaveSnapshot& snapshot)
{
    if (snapshot.identity.signature.present)
        return true;
    if (!snapshot.map())
        return false;
    snapshot.identity.signature = SaveExtensionContainer::make_signature(
        {snapshot.data, static_cast<size_t>(snapshot.identity.fileSize)});
    return true;
}

bool verify_snapshot(const SaveSnapshot& snapshot)
{
    SaveFileIdentity current;
    return query_save_identity(snapshot.file, snapshot.identity.path.c_str(), current) &&
        same_identity_metadata(snapshot.identity, current);
}

SaveSnapshotPtr resolve_save_snapshot(pcstr savedGameName)
{
    string_path fileName;
    CSavedGameWrapper::saved_game_full_name(savedGameName, fileName, SAVE_EXTENSION);
    bool missing = false;
    SaveSnapshotPtr snapshot = open_save_snapshot(fileName, false, &missing);
    if (snapshot || !missing)
        return snapshot;

    CSavedGameWrapper::saved_game_full_name(savedGameName, fileName, SAVE_EXTENSION_LEGACY);
    snapshot = open_save_snapshot(fileName, false, &missing);
    if (!snapshot && !missing)
        return {};
    return snapshot;
}

PreparedSavePtr decode_prepared_save(xr_string savedGameName, SaveSnapshotPtr snapshot)
{
    auto result = std::make_shared<PreparedSaveData>();
    result->name = std::move(savedGameName);
    result->snapshot = snapshot;
    result->identity = snapshot->identity;

    if (!ensure_signature(*snapshot))
        return result;
    result->identity = snapshot->identity;
    if (snapshot->identity.fileSize < 3 * sizeof(u32))
    {
        result->status = PreparedSaveData::Status::Invalid;
        return result;
    }

    IReader stream(const_cast<u8*>(snapshot->data), static_cast<size_t>(snapshot->identity.fileSize));
    const u32 marker = stream.r_u32();
    const u32 version = stream.r_u32();
    if (marker != u32(-1) || version < ALIFE_VERSION ||
        !CSavedGameWrapper::read_metadata(stream, result->metadata))
    {
        result->status = PreparedSaveData::Status::Invalid;
        return result;
    }

    stream.seek(3 * sizeof(u32));
    result->source = std::make_unique_for_overwrite<u8[]>(result->metadata.sourceSize);
    const size_t decodedSize = rtc_decompress(
        result->source.get(), result->metadata.sourceSize, stream.pointer(), result->metadata.compressedSize);
    if (decodedSize != result->metadata.sourceSize)
    {
        result->source.reset();
        result->status = PreparedSaveData::Status::Invalid;
        return result;
    }

    if (!verify_snapshot(*snapshot))
    {
        result->source.reset();
        result->status = PreparedSaveData::Status::Stale;
        return result;
    }

    SaveSnapshotPtr current = open_save_snapshot(snapshot->identity.path.c_str(), false);
    if (!current || (!current->identity.strong && !ensure_signature(*current)) ||
        !same_identity(snapshot->identity, current->identity))
    {
        result->source.reset();
        result->status = PreparedSaveData::Status::Stale;
        return result;
    }

    result->status = PreparedSaveData::Status::Ready;
    return result;
}

std::shared_future<PreparedSavePtr> request_prepared_save(pcstr savedGameName)
{
    if (!savedGameName || !savedGameName[0])
        return {};
    SaveSnapshotPtr snapshot = resolve_save_snapshot(savedGameName);
    if (!snapshot || (!snapshot->identity.strong && !ensure_signature(*snapshot)))
        return {};
    const SaveFileIdentity identity = snapshot->identity;

    std::shared_future<PreparedSavePtr> stale;
    for (;;)
    {
        std::unique_lock lock(preparedSaveMutex);
        preparedSaveCondition.wait(lock, [] { return !preparedSaveReplacing; });
        if (preparedSaveFuture.valid() && preparedSaveIdentityValid &&
            same_identity(preparedSaveIdentity, identity))
        {
            return preparedSaveFuture;
        }

        preparedSaveReplacing = true;
        stale = std::move(preparedSaveFuture);
        preparedSaveIdentity = {};
        preparedSaveIdentityValid = false;
        break;
    }

    if (stale.valid())
        stale.wait();
    stale = {};

    std::shared_future<PreparedSavePtr> next;
    try
    {
        const xr_string requestedName = savedGameName;
        next = std::async(std::launch::async,
            [requestedName, snapshot] { return decode_prepared_save(requestedName, snapshot); }).share();
    }
    catch (...)
    {
        {
            std::lock_guard lock(preparedSaveMutex);
            preparedSaveReplacing = false;
        }
        preparedSaveCondition.notify_all();
        throw;
    }

    {
        std::lock_guard lock(preparedSaveMutex);
        preparedSaveFuture = next;
        preparedSaveIdentity = identity;
        preparedSaveIdentityValid = true;
        preparedSaveReplacing = false;
    }
    preparedSaveCondition.notify_all();
    return next;
}

void evict_prepared_save(const SaveFileIdentity& identity)
{
    std::lock_guard lock(preparedSaveMutex);
    if (!preparedSaveReplacing && preparedSaveIdentityValid && same_identity(preparedSaveIdentity, identity))
    {
        preparedSaveFuture = {};
        preparedSaveIdentity = {};
        preparedSaveIdentityValid = false;
    }
}

PreparedSavePtr get_prepared_save_with_retry(pcstr savedGameName)
{
    for (u32 attempt = 0; attempt != 2; ++attempt)
    {
        std::shared_future<PreparedSavePtr> pending = request_prepared_save(savedGameName);
        if (!pending.valid())
        {
            std::this_thread::yield();
            continue;
        }

        PreparedSavePtr prepared = pending.get();
        if (!prepared || prepared->status == PreparedSaveData::Status::Invalid ||
            prepared->status == PreparedSaveData::Status::Ready)
        {
            return prepared;
        }

        evict_prepared_save(prepared->identity);
        pending = {};
        prepared.reset();
        std::this_thread::yield();
    }
    return {};
}
}

bool CSavedGameWrapper::recover_interrupted_transactions()
{
    if (CALifeStorageManager::save_capture_reentrant())
        return true;
    CALifeStorageManager::wait_for_pending_saves();
    std::lock_guard lock(saveRecoveryMutex);
    const std::filesystem::path saveDirectory(FS.get_path("$game_saves$")->m_Path);
    std::error_code error;
    if (!std::filesystem::exists(saveDirectory, error))
        return !error;

    const std::string scopMarkerSuffix = std::string(SAVE_EXTENSION) + SaveTransactionMarker::suffix;
    const std::string legacyMarkerSuffix =
        std::string(SAVE_EXTENSION_LEGACY) + SaveTransactionMarker::suffix;
    bool recoveredAny = false;
    bool allRecovered = true;
    for (std::filesystem::directory_iterator item(saveDirectory, error), end;
        !error && item != end; item.increment(error))
    {
        const std::string fileName = item->path().filename().string();
        const size_t suffixSize = fileName.ends_with(scopMarkerSuffix) ? scopMarkerSuffix.size() :
            fileName.ends_with(legacyMarkerSuffix) ? legacyMarkerSuffix.size() : 0;
        if (!suffixSize)
            continue;

        std::string mainName = item->path().string();
        mainName.resize(mainName.size() - xr_strlen(SaveTransactionMarker::suffix));
        if (recover_interrupted_transaction(mainName.c_str()))
            recoveredAny = true;
        else
            allRecovered = false;
    }
    if (error)
        allRecovered = false;
    if (recoveredAny)
        refresh_recovered_save_index();
    return allRecovered;
}

bool CSavedGameWrapper::recover_interrupted_save_file_for_commit(LPCSTR main_name)
{
    std::lock_guard lock(saveRecoveryMutex);
    const std::string markerName = std::string(main_name) + SaveTransactionMarker::suffix;
    SaveTransactionMarker::State marker;
    if (SaveTransactionMarker::read(markerName.c_str(), marker) == SaveTransactionMarker::ReadResult::Missing)
        return true;
    return recover_interrupted_transaction(main_name);
}

bool CSavedGameWrapper::recover_interrupted_save(LPCSTR saved_game_name)
{
    if (CALifeStorageManager::save_capture_reentrant())
        return true;
    CALifeStorageManager::wait_for_pending_saves();

    string_path fileName;
    string_path legacyName;
    saved_game_full_name(saved_game_name, fileName, SAVE_EXTENSION);
    saved_game_full_name(saved_game_name, legacyName, SAVE_EXTENSION_LEGACY);

    bool recoveredAny = false;
    std::lock_guard lock(saveRecoveryMutex);
    for (pcstr mainName : {fileName, legacyName})
    {
        const std::string markerName = std::string(mainName) + SaveTransactionMarker::suffix;
        SaveTransactionMarker::State marker;
        const SaveTransactionMarker::ReadResult markerResult =
            SaveTransactionMarker::read(markerName.c_str(), marker);
        if (markerResult == SaveTransactionMarker::ReadResult::Missing)
            continue;
        if (!recover_interrupted_transaction(mainName))
            return false;
        recoveredAny = true;
    }
    if (recoveredAny)
        refresh_recovered_save_index();
    return true;
}

void CSavedGameWrapper::begin_async_load(LPCSTR saved_game_name)
{
    if (!saved_game_name || !saved_game_name[0])
        return;
    if (CALifeStorageManager::save_capture_reentrant())
        return;
    if (!recover_interrupted_save(saved_game_name))
        return;
    request_prepared_save(saved_game_name);
}

bool CSavedGameWrapper::consume_async_load(
    LPCSTR saved_game_name, xr_vector<u8>& source_data, SaveMetadata& metadata)
{
    u64 sourceFileSize = 0;
    u32 sourceFileChecksum = 0;
    return consume_async_load(
        saved_game_name, source_data, metadata, sourceFileSize, sourceFileChecksum);
}

bool CSavedGameWrapper::consume_async_load(LPCSTR saved_game_name, xr_vector<u8>& source_data,
    SaveMetadata& metadata, u64& source_file_size, u32& source_file_checksum)
{
    PreparedSource preparedSource;
    if (!consume_async_load(
            saved_game_name, preparedSource, metadata, source_file_size, source_file_checksum))
    {
        return false;
    }
    source_data.assign(preparedSource.data, preparedSource.data + preparedSource.size);
    return true;
}

bool CSavedGameWrapper::consume_async_load(LPCSTR saved_game_name, PreparedSource& source_data,
    SaveMetadata& metadata, u64& source_file_size, u32& source_file_checksum)
{
    return consume_prepared_load(
               saved_game_name, source_data, metadata, source_file_size, source_file_checksum) ==
        PreparedLoadResult::Ready;
}

CSavedGameWrapper::PreparedLoadResult CSavedGameWrapper::consume_prepared_load(LPCSTR saved_game_name,
    PreparedSource& source_data, SaveMetadata& metadata, u64& source_file_size, u32& source_file_checksum)
{
    source_data = {};
    for (u32 attempt = 0; attempt != 2; ++attempt)
    {
        std::shared_future<PreparedSavePtr> pending = request_prepared_save(saved_game_name);
        if (!pending.valid())
            continue;

        PreparedSavePtr prepared = pending.get();
        if (!prepared || prepared->name != saved_game_name)
            continue;
        if (prepared->status == PreparedSaveData::Status::Invalid)
            return PreparedLoadResult::Invalid;
        if (prepared->status != PreparedSaveData::Status::Ready || !prepared->source ||
            !prepared->snapshot || !verify_snapshot(*prepared->snapshot))
        {
            evict_prepared_save(prepared->identity);
            pending = {};
            prepared.reset();
            continue;
        }

        SaveSnapshotPtr pathGuard = open_save_snapshot(prepared->identity.path.c_str(), true);
        if (!pathGuard || (!pathGuard->identity.strong && !ensure_signature(*pathGuard)) ||
            !same_identity(prepared->identity, pathGuard->identity))
        {
            evict_prepared_save(prepared->identity);
            pending = {};
            prepared.reset();
            continue;
        }

        // Read-share-only guards keep later Windows Lua reads bound to the same save group.
        std::array<SaveSnapshotPtr, 3> companionGuards;
        const std::array<pcstr, 3> companionExtensions{
            {".scoc", ".scoc.bak", SaveExtensionContainer::extension}};
        bool companionsAvailable = true;
        for (size_t index = 0; index != companionExtensions.size(); ++index)
        {
            std::filesystem::path companionPath(prepared->identity.path);
            companionPath.replace_extension(companionExtensions[index]);
            bool missing = false;
            companionGuards[index] = open_save_snapshot(companionPath.string().c_str(), true, &missing);
            if (!companionGuards[index] && !missing)
            {
                companionsAvailable = false;
                break;
            }
        }
        if (!companionsAvailable)
        {
            evict_prepared_save(prepared->identity);
            pending = {};
            prepared.reset();
            continue;
        }

        auto lifetime = std::make_shared<PreparedLeaseState>();
        lifetime->prepared = prepared;
        lifetime->pathGuard = std::move(pathGuard);
        lifetime->companionGuards = std::move(companionGuards);

        evict_prepared_save(prepared->identity);
        pending = {};
        source_data.data = prepared->source.get();
        source_data.size = prepared->metadata.sourceSize;
        source_data.path = prepared->identity.path.c_str();
        source_data.lifetime = std::move(lifetime);
        metadata = prepared->metadata;
        source_file_size = prepared->identity.signature.size;
        source_file_checksum = prepared->identity.signature.checksum;
        return PreparedLoadResult::Ready;
    }
    return PreparedLoadResult::Unavailable;
}

pcstr CSavedGameWrapper::saved_game_full_name(pcstr saved_game_name, string_path& result, pcstr extension)
{
    string_path temp;
    strconcat(sizeof(temp), temp, saved_game_name, extension);
    FS.update_path(result, "$game_saves$", temp);
    return (result);
}

bool CSavedGameWrapper::saved_game_exist(LPCSTR saved_game_name)
{
    const bool captureReentrant = CALifeStorageManager::save_capture_reentrant();
    if (!captureReentrant)
        CALifeStorageManager::wait_for_pending_saves();
    if (captureReentrant)
    {
        string_path fileName;
        return FS.exist(saved_game_full_name(saved_game_name, fileName, SAVE_EXTENSION)) ||
            FS.exist(saved_game_full_name(saved_game_name, fileName, SAVE_EXTENSION_LEGACY));
    }
    if (!recover_interrupted_save(saved_game_name))
        return false;

    string_path file_name;
    if (FS.exist(saved_game_full_name(saved_game_name, file_name, SAVE_EXTENSION)))
        return true;
    if (FS.exist(saved_game_full_name(saved_game_name, file_name, SAVE_EXTENSION_LEGACY)))
        return true;
    return valid_saved_game(saved_game_name);
}

bool CSavedGameWrapper::valid_saved_game(IReader& stream)
{
    if (stream.length() < 3 * sizeof(u32))
        return (false);

    if (stream.r_u32() != u32(-1))
        return (false);

    if (stream.r_u32() < ALIFE_VERSION)
        return (false);

    SaveMetadata metadata;
    if (!read_metadata(stream, metadata))
        return false;

    if (metadata.protectedFormat)
        return true;

    const size_t savedPosition = stream.tell();
    stream.seek(3 * sizeof(u32));
    void* sourceData = xr_malloc(metadata.sourceSize);
    const size_t decodedSize =
        rtc_decompress(sourceData, metadata.sourceSize, stream.pointer(), metadata.compressedSize);
    xr_free(sourceData);
    stream.seek(savedPosition);
    return decodedSize == metadata.sourceSize;
}

bool CSavedGameWrapper::valid_saved_game(LPCSTR saved_game_name)
{
    if (!CALifeStorageManager::save_capture_reentrant())
        CALifeStorageManager::wait_for_pending_saves();
    const bool captureActive = CALifeStorageManager::save_capture_active();
    if (!captureActive && !recover_interrupted_save(saved_game_name))
        return false;

    string_path fileName;
    string_path legacyName;
    saved_game_full_name(saved_game_name, fileName, SAVE_EXTENSION);
    saved_game_full_name(saved_game_name, legacyName, SAVE_EXTENSION_LEGACY);
    if (!recovery_file_exists(fileName) && !recovery_file_exists(legacyName))
    {
        if (captureActive)
            return false;
        bool recovered = false;
        {
            std::lock_guard lock(saveRecoveryMutex);
            recovered = recover_interrupted_transaction(fileName);
            if (!recovered)
                recovered = recover_interrupted_transaction(legacyName);
        }
        if (recovered)
            refresh_recovered_save_index();
        if (!recovery_file_exists(fileName) && !recovery_file_exists(legacyName))
            return false;
    }

    const PreparedSavePtr prepared = get_prepared_save_with_retry(saved_game_name);
    return prepared && prepared->status == PreparedSaveData::Status::Ready;
}

bool CSavedGameWrapper::read_metadata(IReader& stream, SaveMetadata& metadata)
{
    const size_t savedPosition = stream.tell();
    const size_t streamLength = stream.length();
    metadata = {};

    if (streamLength < 3 * sizeof(u32))
        return false;

    stream.seek(2 * sizeof(u32));
    const u32 sourceMarker = stream.r_u32();
    const bool protectedHeader = (sourceMarker & saveProtectedSourceFlag) != 0;
    const u32 sourceSize = sourceMarker & ~saveProtectedSourceFlag;
    metadata.sourceSize = sourceSize;
    metadata.compressedSize = streamLength - 3 * sizeof(u32);

    if (streamLength >= 3 * sizeof(u32) + saveFooterSize)
    {
        stream.seek(streamLength - saveFooterSize);
        const u32 magic = stream.r_u32();
        if (magic == saveFooterMagic)
        {
            const u32 formatVersion = stream.r_u32();
            metadata.saveId = stream.r_u64();
            const u32 footerSourceSize = stream.r_u32();
            const u32 footerCompressedSize = stream.r_u32();
            const u32 footerChecksum = stream.r_u32();

            metadata.protectedFormat = true;
            metadata.compressedSize = streamLength - 3 * sizeof(u32) - saveFooterSize;

            if (formatVersion != saveFooterVersion || footerSourceSize != sourceSize ||
                footerCompressedSize != metadata.compressedSize)
            {
                stream.seek(savedPosition);
                return false;
            }

            stream.seek(3 * sizeof(u32));
            if (crc32(stream.pointer(), static_cast<u32>(metadata.compressedSize)) != footerChecksum)
            {
                stream.seek(savedPosition);
                return false;
            }
        }
    }

    if (protectedHeader && !metadata.protectedFormat)
    {
        stream.seek(savedPosition);
        return false;
    }

    stream.seek(savedPosition);
    return metadata.compressedSize > 0 && metadata.sourceSize > 0 &&
        metadata.sourceSize <= CSavedGameWrapper::maxSourceSize;
}

CSavedGameWrapper::CSavedGameWrapper(LPCSTR saved_game_name)
{
    R_ASSERT3(recover_interrupted_save(saved_game_name),
        "Cannot recover interrupted saved game ", saved_game_name);

    string_path file_name;
    saved_game_full_name(saved_game_name, file_name, SAVE_EXTENSION);
    if (!FS.exist(file_name))
        saved_game_full_name(saved_game_name, file_name, SAVE_EXTENSION_LEGACY);

    R_ASSERT3(FS.exist(file_name), "There is no saved game ", saved_game_name);

    PreparedSavePtr prepared = get_prepared_save_with_retry(saved_game_name);
    if (!prepared || prepared->status != PreparedSaveData::Status::Ready)
    {
        CALifeTimeManager time_manager(alife_section);
        m_game_time = time_manager.game_time();
        m_actor_health = 1.f;
        m_level_id = _LEVEL_ID(-1);
        m_level_name = "";
        return;
    }

    const u32 source_count = static_cast<u32>(prepared->metadata.sourceSize);
    IReader reader(prepared->source.get(), source_count);

    {
        CALifeTimeManager time_manager(alife_section);
        time_manager.load(reader);
        m_game_time = time_manager.game_time();
    }

    {
        R_ASSERT2(reader.find_chunk(OBJECT_CHUNK_DATA), "Can't find chunk OBJECT_CHUNK_DATA!");
        [[maybe_unused]] u32 count = reader.r_u32();
        VERIFY(count > 0);
        CSE_ALifeDynamicObject* object = CALifeObjectRegistry::get_object(reader);
        VERIFY(object->ID == 0);
        CSE_ALifeCreatureActor* actor = smart_cast<CSE_ALifeCreatureActor*>(object);
        VERIFY(actor);

        m_actor_health = actor->get_health();

        IReader* chunk = reader.open_chunk(SPAWN_CHUNK_DATA);
        R_ASSERT2(chunk, "Spawn version mismatch - REBUILD SPAWN!");

        string_path spawn_file_name;
        {
            IReader* sub_chunk = chunk->open_chunk(0);
            if (!sub_chunk)
            {
                chunk->close();
                F_entity_Destroy(object);
                m_level_id = _LEVEL_ID(-1);
                m_level_name = "";
                return;
            }
            sub_chunk->r_stringZ(spawn_file_name, sizeof(spawn_file_name));
            sub_chunk->close();
        }

        chunk->close();

        if (!FS.exist(file_name, "$game_spawn$", spawn_file_name, ".spawn"))
        {
            F_entity_Destroy(object);
            m_level_id = _LEVEL_ID(-1);
            m_level_name = "";
            return;
        }

        IReader* spawn = NULL;
        bool b_destroy_spawn = true;
        if (ai().get_alife() && ai().alife().spawns().get_spawn_name() == spawn_file_name)
        {
            spawn = ai().alife().spawns().get_spawn_file();
            b_destroy_spawn = false;
        }
        else
            spawn = FS.r_open(file_name);

        if (!spawn)
        {
            F_entity_Destroy(object);
            m_level_id = _LEVEL_ID(-1);
            m_level_name = "";
            return;
        }

        chunk = spawn->open_chunk(4);
        if (!chunk) // Shadow of Chernobyl
        {
            string_path graph_path;
            FS.update_path(graph_path, "$game_data$", GRAPH_NAME);
            chunk = FS.r_open(graph_path);
        }

        if (!chunk)
        {
            F_entity_Destroy(object);
            if (b_destroy_spawn)
                FS.r_close(spawn);
            m_level_id = _LEVEL_ID(-1);
            m_level_name = "";
            return;
        }

        {
            CGameGraph graph(*chunk);
            m_level_id = graph.vertex(object->m_tGraphID)->level_id();
            if (graph.header().level_exist(m_level_id))
                m_level_name = graph.header().level(m_level_id).name();
            else
                m_level_name = StringTable().translate("ui_st_error");
        }

        chunk->close();
        if (b_destroy_spawn)
            FS.r_close(spawn);
        F_entity_Destroy(object);
    }

}
