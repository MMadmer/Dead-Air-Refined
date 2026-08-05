from pathlib import Path
import sys


def require(source: str, fragment: str, start: int = 0) -> int:
    position = source.find(fragment, start)
    if position < 0:
        raise AssertionError(f"missing production fragment: {fragment}")
    return position


def main() -> int:
    source_path = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("src/xrGame/alife_storage_manager.cpp")
    source = source_path.read_text(encoding="utf-8")
    assert "customPreparedSignature" not in source

    require(source, 'Threading::RunThread("Async save I/O"')
    require(source, "GENERIC_READ, FILE_SHARE_READ")
    assert "GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE" not in source
    assert "GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE" not in source

    worker_start = require(source, "class AsyncSaveIoWorker final")
    custom_flush = require(source, "native_flush_file(request.customTempName.c_str())", worker_start)
    custom_signature = require(source, "SaveExtensionContainer::read_file_signature(", custom_flush)
    helper_copy = require(source, 'native_copy_file(finalName.c_str(), (finalName + ".bak").c_str())')
    main_wait = require(source, "mainTempAvailable.wait(", helper_copy)
    main_write = require(source, "native_write_file(currentMainTemp.tempName.c_str()", main_wait)
    assert custom_flush < custom_signature < helper_copy < main_wait < main_write

    execute = require(source, "AsyncSaveCompletion execute_save_job(")
    helper_begin = require(source, "ioWorker.begin(", execute)
    compression = require(source, "rtc_compress(", helper_begin)
    publish = require(source, "ioWorker.publish_main_temp(", compression)
    decompression = require(source, "rtc_decompress(", publish)
    comparison = require(source, "memcmp(", decompression)
    checksum = require(source, "crc32(saveHeader.data()", comparison)
    helper_finish = require(source, "ioWorker.finish()", checksum)
    sidecar_build = require(source, "SaveExtensionContainer::build(", helper_finish)
    serial_main_write = require(source, "native_write_transaction_temp(scopTemp.c_str()", helper_finish)
    assert helper_begin < compression < publish < decompression < comparison < checksum < helper_finish
    assert helper_finish < sidecar_build < serial_main_write

    require(source, "ioPreparation.main_temp_durable()")
    require(source, "completed = AsyncSaveIoPreparation{};")
    require(source, "CopyFileExA(source, destination, nullptr, nullptr, nullptr, COPY_FILE_NO_BUFFERING)")
    require(source, "if (!bufferingRequired || !CopyFileA(source, destination, FALSE))")

    marker_write = require(
        source,
        "native_write_file(transactionMarker.c_str(), markerContents.data(), markerContents.size())",
    )
    lease_release = require(source, "ioPreparation.release_final_leases();", marker_write)
    old_main_remove = require(source, "native_remove_file(files[0].finalName.c_str())")
    assert marker_write < lease_release < old_main_remove

    require(source, "if (!backupsPrepared && !CSavedGameWrapper::recover_interrupted_save_file_for_commit(scopName))")
    require(source, "file.hadFinal = backupsPrepared || native_file_exists(file.finalName.c_str());")
    require(source, "thread_local u32 lastNativeError{};")
    print("production overlap structure and ordering verified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
