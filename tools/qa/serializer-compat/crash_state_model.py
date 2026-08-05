from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import sys


@dataclass(frozen=True)
class Checkpoint:
    name: str
    marker: str
    main: str
    companions: str
    backups: str
    temps: str
    expected: str
    reason: str


def modeled_recovery(checkpoint: Checkpoint) -> str:
    if checkpoint.marker == "none":
        return checkpoint.main
    if checkpoint.marker == "partial":
        return checkpoint.main if checkpoint.main in {"old", "new"} else "blocked"
    if checkpoint.main in {"old", "new"}:
        return checkpoint.main
    if checkpoint.backups == "complete-old":
        return "old"
    return "blocked"


CHECKPOINTS = (
    Checkpoint(
        "backup copy in progress",
        "none",
        "old",
        "old",
        "partial",
        "partial",
        "old",
        "No marker means partial backups and transaction temps are ignored.",
    ),
    Checkpoint(
        "compression or validation failed",
        "none",
        "old",
        "old",
        "partial-or-complete-old",
        "partial",
        "old",
        "The helper never mutates active finals and commit is not entered.",
    ),
    Checkpoint(
        "all temps and backups durable",
        "none",
        "old",
        "old",
        "complete-old",
        "complete",
        "old",
        "The old main remains the only commit marker until .txn is durable.",
    ),
    Checkpoint(
        "transaction marker torn",
        "partial",
        "old",
        "old",
        "complete-old",
        "complete",
        "old",
        "The valid old main authorizes removal of an incomplete marker.",
    ),
    Checkpoint(
        "transaction marker durable",
        "valid",
        "old",
        "old",
        "complete-old",
        "complete",
        "old",
        "Recovery sees a valid main and abandons the uncommitted new group.",
    ),
    Checkpoint(
        "old main removed",
        "valid",
        "missing",
        "old",
        "complete-old",
        "complete",
        "old",
        "Recovery restores the complete marked backup group.",
    ),
    Checkpoint(
        "custom companion installed",
        "valid",
        "missing",
        "mixed",
        "complete-old",
        "complete",
        "old",
        "A missing main forces rollback; companions are restored first.",
    ),
    Checkpoint(
        "all companions installed",
        "valid",
        "missing",
        "new",
        "complete-old",
        "complete",
        "old",
        "The new group is not committed until its main is installed.",
    ),
    Checkpoint(
        "new main installed",
        "valid",
        "new",
        "new",
        "complete-old",
        "consumed",
        "new",
        "The valid new main implies companions were installed first.",
    ),
    Checkpoint(
        "transaction marker removed",
        "none",
        "new",
        "new",
        "complete-old",
        "consumed",
        "new",
        "Normal completion leaves the newly committed group active.",
    ),
)


def render_table() -> str:
    lines = [
        "# Crash-state table for direct backup overlap",
        "",
        "| Checkpoint | `.txn` | Main | Companions | `.bak` | Temps | Restart result | Why |",
        "| --- | --- | --- | --- | --- | --- | --- | --- |",
    ]
    for checkpoint in CHECKPOINTS:
        actual = modeled_recovery(checkpoint)
        if actual != checkpoint.expected:
            raise AssertionError(
                f"{checkpoint.name}: modeled {actual}, expected {checkpoint.expected}"
            )
        lines.append(
            f"| {checkpoint.name} | {checkpoint.marker} | {checkpoint.main} | "
            f"{checkpoint.companions} | {checkpoint.backups} | {checkpoint.temps} | "
            f"{actual} | {checkpoint.reason} |"
        )
    lines.extend(
        (
            "",
            "The model intentionally treats every backup written without a durable marker as orphaned. "
            "That is the frozen recovery rule used by the current implementation.",
            "",
        )
    )
    return "\n".join(lines)


def main() -> int:
    output = Path(sys.argv[1]) if len(sys.argv) == 2 else None
    table = render_table()
    if output:
        output.write_text(table, encoding="utf-8")
    print(f"validated {len(CHECKPOINTS)} crash checkpoints")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
