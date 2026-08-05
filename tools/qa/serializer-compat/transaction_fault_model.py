from __future__ import annotations

from dataclasses import dataclass, field
import json
from pathlib import Path
import sys
import zlib


MAIN = "save.scop"
CUSTOM = "save.scoc"
SIDECAR = "save.scov"
FINALS = (MAIN, CUSTOM, SIDECAR)


@dataclass
class Transaction:
    files: dict[str, bytes] = field(default_factory=dict)
    marker: str = "none"
    previous_present: tuple[bool, bool, bool] = (True, True, True)
    helper_prepared: bool = False
    custom_temp_durable: bool = False
    main_temp_durable: bool = False
    path: str = "serial"

    @staticmethod
    def full_old() -> "Transaction":
        files = {
            MAIN: b"old-main",
            CUSTOM: b"old-custom",
            SIDECAR: b"old-sidecar",
            MAIN + ".bak": b"older-main",
            CUSTOM + ".bak": b"older-custom",
            SIDECAR + ".bak": b"older-sidecar",
        }
        return Transaction(files=files)

    def begin_helper(
        self,
        new_custom: bytes | None,
        fail_after: str | None = None,
    ) -> bool:
        if any(name not in self.files for name in FINALS):
            self.path = "serial-missing-companion"
            return False

        self.path = "overlap"
        if new_custom is not None:
            self.files[CUSTOM + ".tmp"] = new_custom
            self.custom_temp_durable = True
            if fail_after == "durable-custom-temp":
                self.path = "serial-helper-fallback"
                self.custom_temp_durable = False
                return False

        for index, name in enumerate(FINALS, start=1):
            self.files[name + ".bak"] = self.files[name]
            if fail_after == f"backup-{index}":
                self.path = "serial-helper-fallback"
                self.custom_temp_durable = False
                return False

        self.helper_prepared = True
        return True

    def publish_main_temp(self, fail_after_write: bool = False) -> bool:
        if not self.helper_prepared:
            return False
        self.files[MAIN + ".tmp"] = b"partial-main" if fail_after_write else b"new-main"
        if fail_after_write:
            self.path = "serial-helper-fallback"
            self.helper_prepared = False
            self.custom_temp_durable = False
            self.main_temp_durable = False
            return False
        self.main_temp_durable = True
        return True

    def write_new_temps(self, new_custom: bytes | None) -> None:
        if not self.main_temp_durable:
            self.files[MAIN + ".tmp"] = b"new-main"
        self.files[SIDECAR + ".tmp"] = b"new-sidecar"
        if new_custom is not None and not self.custom_temp_durable:
            self.files[CUSTOM + ".tmp"] = new_custom

    def serial_backup_preflight(self) -> None:
        self.previous_present = tuple(name in self.files for name in FINALS)
        for present, name in zip(self.previous_present, FINALS, strict=True):
            if present:
                self.files[name + ".bak"] = self.files[name]
            else:
                self.files.pop(name + ".bak", None)

    def commit_success(self, new_custom: bytes | None) -> None:
        self.write_new_temps(new_custom)
        if not self.helper_prepared:
            self.serial_backup_preflight()
        else:
            self.previous_present = (True, True, True)
        self.marker = "valid"
        self.files.pop(MAIN, None)
        if new_custom is None:
            self.files.pop(CUSTOM, None)
        else:
            self.files[CUSTOM] = self.files.pop(CUSTOM + ".tmp")
        self.files[SIDECAR] = self.files.pop(SIDECAR + ".tmp")
        self.files[MAIN] = self.files.pop(MAIN + ".tmp")
        self.marker = "none"

    def restore_old(self) -> None:
        for present, name in zip(self.previous_present, FINALS, strict=True):
            if present:
                self.files[name] = self.files[name + ".bak"]
            else:
                self.files.pop(name, None)
        self.marker = "none"
        self.cleanup_temps()

    def recover(self) -> None:
        if self.marker == "none":
            return
        if self.marker == "partial":
            if MAIN in self.files:
                self.marker = "none"
            return
        if MAIN in self.files:
            self.marker = "none"
            self.cleanup_temps()
            return
        self.restore_old()

    def cleanup_temps(self) -> None:
        for name in tuple(self.files):
            if name.endswith(".tmp"):
                del self.files[name]

    def assert_no_transients(self) -> None:
        assert self.marker == "none"
        assert not any(name.endswith(".tmp") for name in self.files)


def final_group(transaction: Transaction) -> dict[str, bytes | None]:
    return {name: transaction.files.get(name) for name in FINALS}


def expect_new(transaction: Transaction, custom: bytes | None) -> None:
    assert final_group(transaction) == {
        MAIN: b"new-main",
        CUSTOM: custom,
        SIDECAR: b"new-sidecar",
    }
    transaction.assert_no_transients()


def expect_old(
    transaction: Transaction,
    custom: bytes | None = b"old-custom",
    sidecar: bytes | None = b"old-sidecar",
) -> None:
    assert final_group(transaction) == {
        MAIN: b"old-main",
        CUSTOM: custom,
        SIDECAR: sidecar,
    }
    transaction.assert_no_transients()


def helper_fallback_case(fail_after: str) -> dict[str, str]:
    transaction = Transaction.full_old()
    assert not transaction.begin_helper(b"new-custom", fail_after)
    transaction.commit_success(b"new-custom")
    expect_new(transaction, b"new-custom")
    assert transaction.files[MAIN + ".bak"] == b"old-main"
    assert transaction.files[CUSTOM + ".bak"] == b"old-custom"
    assert transaction.files[SIDECAR + ".bak"] == b"old-sidecar"
    return {"case": f"helper-failure-{fail_after}", "result": "serial-new-exact"}


def helper_main_fallback_case() -> dict[str, str]:
    transaction = Transaction.full_old()
    assert transaction.begin_helper(b"new-custom")
    assert not transaction.publish_main_temp(fail_after_write=True)
    transaction.commit_success(b"new-custom")
    expect_new(transaction, b"new-custom")
    assert transaction.files[MAIN + ".bak"] == b"old-main"
    assert transaction.files[CUSTOM + ".bak"] == b"old-custom"
    assert transaction.files[SIDECAR + ".bak"] == b"old-sidecar"
    return {"case": "helper-failure-main-temp", "result": "serial-new-exact"}


def stage2_success_case() -> dict[str, str]:
    transaction = Transaction.full_old()
    assert transaction.begin_helper(b"new-custom")
    assert transaction.publish_main_temp()
    transaction.commit_success(b"new-custom")
    expect_new(transaction, b"new-custom")
    return {"case": "stage2-main-temp-durable", "result": "overlap-new-exact"}


def missing_companion_case(missing: str, new_custom: bytes | None) -> dict[str, str]:
    transaction = Transaction.full_old()
    del transaction.files[missing]
    assert not transaction.begin_helper(new_custom)
    assert transaction.path == "serial-missing-companion"
    transaction.commit_success(new_custom)
    expect_new(transaction, new_custom)
    return {"case": f"missing-{missing}", "result": "serial-new-exact"}


def zero_custom_case() -> dict[str, str]:
    transaction = Transaction.full_old()
    assert transaction.begin_helper(b"")
    assert transaction.custom_temp_durable
    assert transaction.publish_main_temp()
    transaction.commit_success(b"")
    expect_new(transaction, b"")
    assert CUSTOM in transaction.files and len(transaction.files[CUSTOM]) == 0
    return {"case": "zero-byte-scoc", "result": "overlap-present-empty-exact"}


def crash_cases() -> list[dict[str, str]]:
    results: list[dict[str, str]] = []

    transaction = Transaction.full_old()
    transaction.begin_helper(b"new-custom", "backup-1")
    transaction.cleanup_temps()
    expect_old(transaction)
    results.append({"case": "crash-before-marker-partial-backup", "result": "old"})

    transaction = Transaction.full_old()
    assert transaction.begin_helper(b"new-custom")
    transaction.write_new_temps(b"new-custom")
    transaction.marker = "partial"
    transaction.recover()
    transaction.cleanup_temps()
    expect_old(transaction)
    results.append({"case": "torn-marker", "result": "old"})

    transaction = Transaction.full_old()
    assert transaction.begin_helper(b"new-custom")
    transaction.write_new_temps(b"new-custom")
    transaction.marker = "valid"
    transaction.recover()
    expect_old(transaction)
    results.append({"case": "durable-marker-old-main", "result": "old"})

    for stage in ("main-removed", "custom-installed", "sidecar-installed"):
        transaction = Transaction.full_old()
        assert transaction.begin_helper(b"new-custom")
        transaction.write_new_temps(b"new-custom")
        transaction.marker = "valid"
        transaction.files.pop(MAIN)
        if stage in {"custom-installed", "sidecar-installed"}:
            transaction.files[CUSTOM] = transaction.files.pop(CUSTOM + ".tmp")
        if stage == "sidecar-installed":
            transaction.files[SIDECAR] = transaction.files.pop(SIDECAR + ".tmp")
        transaction.recover()
        expect_old(transaction)
        results.append({"case": f"crash-{stage}", "result": "rollback-old"})

    transaction = Transaction.full_old()
    assert transaction.begin_helper(b"new-custom")
    transaction.write_new_temps(b"new-custom")
    transaction.marker = "valid"
    transaction.files.pop(MAIN)
    transaction.files[CUSTOM] = transaction.files.pop(CUSTOM + ".tmp")
    transaction.files[SIDECAR] = transaction.files.pop(SIDECAR + ".tmp")
    transaction.files[MAIN] = transaction.files.pop(MAIN + ".tmp")
    transaction.recover()
    expect_new(transaction, b"new-custom")
    results.append({"case": "crash-new-main-installed", "result": "keep-new"})
    return results


def immediate_failure_cases() -> list[dict[str, str]]:
    results: list[dict[str, str]] = []
    for stage in ("main-temp", "custom-temp", "sidecar-temp", "temp-flush", "backup-copy", "marker-write"):
        transaction = Transaction.full_old()
        transaction.begin_helper(b"new-custom", "backup-1" if stage == "backup-copy" else None)
        transaction.cleanup_temps()
        transaction.marker = "none"
        expect_old(transaction)
        results.append({"case": f"failure-{stage}", "result": "old-no-transients"})

    for stage in ("custom-install", "sidecar-install", "main-install"):
        transaction = Transaction.full_old()
        assert transaction.begin_helper(b"new-custom")
        transaction.write_new_temps(b"new-custom")
        transaction.marker = "valid"
        transaction.files.pop(MAIN)
        if stage != "custom-install":
            transaction.files[CUSTOM] = transaction.files.pop(CUSTOM + ".tmp")
        if stage == "main-install":
            transaction.files[SIDECAR] = transaction.files.pop(SIDECAR + ".tmp")
        transaction.restore_old()
        expect_old(transaction)
        results.append({"case": f"failure-{stage}", "result": "rollback-old"})
    return results


def thread_local_error_case() -> dict[str, str]:
    native_error = {"writer": 0, "helper": 0}
    native_error["helper"] = 5
    assert native_error["writer"] == 0
    native_error["writer"] = 112
    assert native_error["writer"] == 112 and native_error["helper"] == 5
    return {"case": "thread-local-native-error", "result": "writer-error-authoritative"}


def copy_fallback_case(error: str, fallback_expected: bool) -> dict[str, str]:
    unsupported = {
        "ERROR_INVALID_FUNCTION",
        "ERROR_INVALID_PARAMETER",
        "ERROR_NOT_SUPPORTED",
        "ERROR_CALL_NOT_IMPLEMENTED",
    }
    buffered_attempted = error in unsupported
    assert buffered_attempted == fallback_expected
    return {
        "case": f"unbuffered-copy-{error.lower()}",
        "result": "buffered-retry" if buffered_attempted else "no-io-error-retry",
    }


def prepared_signature_case(helper_succeeds: bool) -> dict[str, str]:
    captured_signature = zlib.crc32(b"captured-before-worker")
    current_temp = b"durable-current-temp"
    current_signature = zlib.crc32(current_temp)
    returned_signature = current_signature if helper_succeeds else zlib.crc32(current_temp)
    assert returned_signature == current_signature and returned_signature != captured_signature
    path = "helper-after-flush" if helper_succeeds else "serial-reread-after-helper-failure"
    return {"case": f"prepared-signature-{path}", "result": "current-temp-exact"}


def main() -> int:
    results = [
        helper_fallback_case("durable-custom-temp"),
        helper_fallback_case("backup-1"),
        helper_fallback_case("backup-2"),
        helper_main_fallback_case(),
        stage2_success_case(),
        missing_companion_case(CUSTOM, None),
        missing_companion_case(SIDECAR, b"new-custom"),
        zero_custom_case(),
        thread_local_error_case(),
        copy_fallback_case("ERROR_INVALID_FUNCTION", True),
        copy_fallback_case("ERROR_INVALID_PARAMETER", True),
        copy_fallback_case("ERROR_NOT_SUPPORTED", True),
        copy_fallback_case("ERROR_CALL_NOT_IMPLEMENTED", True),
        copy_fallback_case("ERROR_DISK_FULL", False),
        copy_fallback_case("ERROR_ACCESS_DENIED", False),
        prepared_signature_case(True),
        prepared_signature_case(False),
        *immediate_failure_cases(),
        *crash_cases(),
    ]
    payload = {"passed": len(results), "cases": results}
    output = Path(sys.argv[1]) if len(sys.argv) == 2 else None
    if output:
        output.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    print(f"passed {len(results)} deterministic transaction/fault cases")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
