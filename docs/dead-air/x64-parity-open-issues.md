# Dead Air x64: open parity issues

This file contains only the currently unresolved x86-to-x64 discrepancies.
Clear the issue list only after the whole batch is built and verified.

## Current batch

- Expanded combo boxes prefer the `ui_inGame2_combobox` frame whose center is
  effectively transparent, allowing labels below the list to overlap its
  option text.
