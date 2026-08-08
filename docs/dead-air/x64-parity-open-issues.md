# Dead Air x64: open parity issues

Only currently unresolved x86-to-x64 discrepancies belong here. Workflow and
validation rules are defined in [`PROJECT_RULES.md`](../../PROJECT_RULES.md).

## Open issue

- Expanded combo boxes prefer the `ui_inGame2_combobox` frame whose center is
  effectively transparent, allowing labels below the list to overlap its
  option text.

## Deferred diagnostics candidate

- Early vectored capture of silent fatal failures (heap corruption, stack
  overflow) that bypass `SetUnhandledExceptionFilter` (`xrDebug.cpp:613`).
  Reference concept exists (`DeadAir-x64` engine, commit `abc28441`), but a
  crash handler may ship only after a conclusive safety review on an isolated
  crash harness: no heap allocation, no logger locks, one-shot guard, always
  `EXCEPTION_CONTINUE_SEARCH`. That review has not been performed, so the
  feature is deliberately not integrated yet.
