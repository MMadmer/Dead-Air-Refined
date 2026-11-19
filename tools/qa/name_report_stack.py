"""Names the stack of a crash report from the build's own symbols.

    python tools/qa/name_report_stack.py <report.zip | report.json> [--bin <folder with the programs and their .pdb>]

A crash report (`appdata\\session_reports\\dar-report-crash-*.zip`) holds its stack as modules and offsets
(`report.json`, `stack`). Each frame of a module of the game is named - function, file, line - from the PDB
beside that module in the build's output (`bin\\x64\\Release` of this repository unless `--bin` says another).
The symbols must be the ones of the build that crashed: a later build names other lines.
"""
import argparse
import ctypes
import json
import os
import sys
import zipfile
from ctypes import wintypes

MAX_NAME = 1024


class SymbolInfo(ctypes.Structure):
    _fields_ = [
        ("SizeOfStruct", wintypes.ULONG), ("TypeIndex", wintypes.ULONG), ("Reserved", ctypes.c_ulonglong * 2),
        ("Index", wintypes.ULONG), ("Size", wintypes.ULONG), ("ModBase", ctypes.c_ulonglong),
        ("Flags", wintypes.ULONG), ("Value", ctypes.c_ulonglong), ("Address", ctypes.c_ulonglong),
        ("Register", wintypes.ULONG), ("Scope", wintypes.ULONG), ("Tag", wintypes.ULONG),
        ("NameLen", wintypes.ULONG), ("MaxNameLen", wintypes.ULONG), ("Name", ctypes.c_wchar * MAX_NAME),
    ]


class LineInfo(ctypes.Structure):
    _fields_ = [
        ("SizeOfStruct", wintypes.DWORD), ("Key", ctypes.c_void_p), ("LineNumber", wintypes.DWORD),
        ("FileName", ctypes.c_wchar_p), ("Address", ctypes.c_ulonglong),
    ]


def stack_of(path):
    if path.lower().endswith(".zip"):
        with zipfile.ZipFile(path) as z:
            report = json.loads(z.read("report.json").decode("utf-8"))
    else:
        with open(path, encoding="utf-8") as f:
            report = json.load(f)
    return [(f.get("module", ""), int(str(f.get("rva", "0")), 16)) for f in report.get("stack", [])]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("report")
    here = os.path.dirname(os.path.abspath(__file__))
    parser.add_argument("--bin", default=os.path.normpath(os.path.join(here, "..", "..", "bin", "x64", "Release")))
    args = parser.parse_args()

    dbghelp = ctypes.WinDLL("dbghelp", use_last_error=True)
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.GetCurrentProcess.restype = wintypes.HANDLE
    dbghelp.SymInitializeW.argtypes = [wintypes.HANDLE, wintypes.LPCWSTR, wintypes.BOOL]
    dbghelp.SymLoadModuleExW.argtypes = [wintypes.HANDLE, wintypes.HANDLE, wintypes.LPCWSTR, wintypes.LPCWSTR,
                                         ctypes.c_ulonglong, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD]
    dbghelp.SymLoadModuleExW.restype = ctypes.c_ulonglong
    dbghelp.SymFromAddrW.argtypes = [wintypes.HANDLE, ctypes.c_ulonglong, ctypes.POINTER(ctypes.c_ulonglong),
                                     ctypes.POINTER(SymbolInfo)]
    dbghelp.SymGetLineFromAddrW64.argtypes = [wintypes.HANDLE, ctypes.c_ulonglong, ctypes.POINTER(wintypes.DWORD),
                                              ctypes.POINTER(LineInfo)]
    process = kernel32.GetCurrentProcess()
    # lines, and names without their decoration
    dbghelp.SymSetOptions(0x10 | 0x2)
    if not dbghelp.SymInitializeW(process, args.bin, False):
        sys.exit("The symbols could not be opened: dbghelp refused " + args.bin)

    bases, next_base = {}, 0x10000000
    for module, rva in stack_of(args.report):
        path = os.path.join(args.bin, module)
        if module not in bases:
            bases[module] = dbghelp.SymLoadModuleExW(process, None, path, None, next_base, 0, None, 0) \
                if os.path.isfile(path) else 0
            next_base += 0x10000000
        base = bases[module]
        if not base:
            print(f"{module}+{rva:#x}")
            continue
        info = SymbolInfo()
        # the size of the structure as dbghelp declares it, with a name of one character
        info.SizeOfStruct = 88
        info.MaxNameLen = MAX_NAME - 1
        moved = ctypes.c_ulonglong(0)
        name = info.Name if dbghelp.SymFromAddrW(process, base + rva, ctypes.byref(moved), ctypes.byref(info)) else "?"
        line = LineInfo()
        line.SizeOfStruct = ctypes.sizeof(LineInfo)
        column = wintypes.DWORD(0)
        where = ""
        if dbghelp.SymGetLineFromAddrW64(process, base + rva, ctypes.byref(column), ctypes.byref(line)):
            where = f"  {line.FileName}:{line.LineNumber}"
        print(f"{module}+{rva:#x}  {name}{where}")


if __name__ == "__main__":
    main()
