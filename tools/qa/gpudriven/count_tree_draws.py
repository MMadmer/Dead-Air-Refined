#!/usr/bin/env python3
"""Counts the CPU tree draw path of a running engine, per frame and per view (gpudriven.md 2.6).

    python tools/qa/gpudriven/count_tree_draws.py PID [--frames 12] [--module xrRender_R5]
        [--json out.json] [--timeout 120]
    python tools/qa/gpudriven/count_tree_draws.py --sites bin/x64/Release/xrRender_R5.dll

(tools/qa/gpudriven/Run-TreeDrawCount.ps1 runs a stand pass and attaches the counter.)

Four hardware execute breakpoints on every thread of the engine: R_dsgraph_structure::render_graph
(one pass of a view), FTreeVisual::FillInstanceData (one instance of a batched draw) and
FTreeVisual_ST::Render / FTreeVisual_PM::Render (one single draw). The view of a pass comes from the
caller's source file and line in the PDB, which sees through inlining (CRender::render_hud_shadow is
inlined into CRender::Render). A frame ends with the forward pass (render_graph(1) of
CRender::render_forward in phase_combine): the sun cascades record on workers from Calculate() on,
concurrently with the main pass, but they, the rain map, the lights and the HUD shadow are all done
before it. Every pass belongs to the frame in which it started.

The counter attaches as a debugger, skips one warm-up frame, counts --frames frames, clears the
breakpoints while every thread is stopped, drains the pending events and detaches; the engine keeps
running. The PDB of the module has to sit next to its DLL (Invoke-Dx12Stand copies it into the rig).

Views: main (G-buffer, priority 0), zfill, forward (priority 1), sun (the cascades), rain, lights
(one pass per shadowed face), smapvis, hud_shadow. "shadow" sums the shadow-map views (sun, rain,
lights, smapvis, zfill): with "main" the numbers the GPU-driven [gd] line is checked against
(gpudriven.md 9.3). Instances = batched + single. Exit code 0 when --frames frames were counted.
"""
import argparse
import bisect
import collections
import ctypes
import ctypes.wintypes as wt
import json
import os
import struct
import sys
import time

k32 = ctypes.WinDLL('kernel32', use_last_error=True)
dbg = ctypes.WinDLL('dbghelp', use_last_error=True)

PROCESS_ALL_ACCESS = 0x1F0FFF
THREAD_ALL_ACCESS = 0x1F03FF
DBG_CONTINUE = 0x00010002
DBG_EXCEPTION_NOT_HANDLED = 0x80010001
EXCEPTION_BREAKPOINT = 0x80000003
EXCEPTION_SINGLE_STEP = 0x80000004
EV_EXCEPTION, EV_CREATE_THREAD, EV_CREATE_PROCESS = 1, 2, 3
EV_EXIT_THREAD, EV_EXIT_PROCESS, EV_LOAD_DLL = 4, 5, 6

# CONTEXT (x64): CONTEXT_AMD64 | CONTROL | INTEGER | DEBUG_REGISTERS and the field offsets.
CONTEXT_FLAGS = 0x100000 | 0x1 | 0x2 | 0x10
OFF_FLAGS, OFF_EFLAGS, OFF_DR0, OFF_DR6, OFF_DR7 = 0x30, 0x44, 0x48, 0x68, 0x70
OFF_RDX, OFF_RSP = 0x88, 0x98
EFLAGS_RF = 0x10000
# DR0-DR3 locally enabled, execute, length 1.
DR7_EXECUTE_ALL = 0x55

SYMOPT_UNDNAME, SYMOPT_DEFERRED_LOADS, SYMOPT_LOAD_LINES, SYMOPT_FAIL_CRITICAL_ERRORS = 0x2, 0x4, 0x10, 0x200

KINDS = ('graph', 'fill', 'st', 'pm')
# The breakpoint targets, one per kind.
COUNTED = ('R_dsgraph_structure::render_graph', 'FTreeVisual::FillInstanceData',
           'FTreeVisual_ST::Render', 'FTreeVisual_PM::Render')
SHADOW_VIEWS = ('sun', 'rain', 'lights', 'smapvis', 'zfill')
# Caller source file -> view; r2_R_render.cpp is split by function and priority below.
FILE_VIEWS = (('phase_hud_shadow', 'hud_shadow'), ('render_phase_sun', 'sun'), ('r2_r_sun', 'sun'),
              ('r3_r_rain', 'rain'), ('r2_r_lights', 'lights'), ('light_smapvis', 'smapvis'))
# Fallback when the PDB has no line for the caller.
FUNC_VIEWS = (('render_hud_shadow', 'hud_shadow'), ('render_sun', 'sun'), ('render_rain', 'rain'),
              ('render_lights', 'lights'), ('smapvis', 'smapvis'), ('render_forward', 'forward'),
              ('CRender::Render', 'main'))


class SYMBOL_INFO(ctypes.Structure):
    _fields_ = [('SizeOfStruct', wt.ULONG), ('TypeIndex', wt.ULONG), ('Reserved', ctypes.c_uint64 * 2),
                ('Index', wt.ULONG), ('Size', wt.ULONG), ('ModBase', ctypes.c_uint64), ('Flags', wt.ULONG),
                ('Value', ctypes.c_uint64), ('Address', ctypes.c_uint64), ('Register', wt.ULONG),
                ('Scope', wt.ULONG), ('Tag', wt.ULONG), ('NameLen', wt.ULONG), ('MaxNameLen', wt.ULONG),
                ('Name', ctypes.c_char * 1024)]


class IMAGEHLP_LINE64(ctypes.Structure):
    _fields_ = [('SizeOfStruct', wt.DWORD), ('Key', ctypes.c_void_p), ('LineNumber', wt.DWORD),
                ('FileName', ctypes.c_char_p), ('Address', ctypes.c_uint64)]


ENUMCB = ctypes.WINFUNCTYPE(wt.BOOL, ctypes.POINTER(SYMBOL_INFO), wt.ULONG, ctypes.c_void_p)

k32.OpenProcess.restype = wt.HANDLE
k32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
k32.OpenThread.restype = wt.HANDLE
k32.OpenThread.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
k32.CloseHandle.argtypes = [wt.HANDLE]
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_uint64, ctypes.c_void_p, ctypes.c_size_t,
                                  ctypes.POINTER(ctypes.c_size_t)]
k32.GetThreadContext.argtypes = [wt.HANDLE, ctypes.c_void_p]
k32.SetThreadContext.argtypes = [wt.HANDLE, ctypes.c_void_p]
k32.WaitForDebugEvent.argtypes = [ctypes.c_void_p, wt.DWORD]
k32.ContinueDebugEvent.argtypes = [wt.DWORD, wt.DWORD, wt.DWORD]
k32.DebugActiveProcess.argtypes = [wt.DWORD]
k32.DebugActiveProcessStop.argtypes = [wt.DWORD]
k32.DebugSetProcessKillOnExit.argtypes = [wt.BOOL]
k32.DebugBreakProcess.argtypes = [wt.HANDLE]
k32.QueryFullProcessImageNameW.argtypes = [wt.HANDLE, wt.DWORD, wt.LPWSTR, ctypes.POINTER(wt.DWORD)]
k32.GetModuleHandleW.restype = ctypes.c_void_p
k32.GetModuleHandleW.argtypes = [wt.LPCWSTR]
k32.GetProcAddress.restype = ctypes.c_void_p
k32.GetProcAddress.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
dbg.SymSetOptions.argtypes = [wt.DWORD]
dbg.SymInitializeW.argtypes = [wt.HANDLE, wt.LPCWSTR, wt.BOOL]
dbg.SymCleanup.argtypes = [wt.HANDLE]
dbg.SymEnumSymbols.argtypes = [wt.HANDLE, ctypes.c_uint64, ctypes.c_char_p, ENUMCB, ctypes.c_void_p]
dbg.SymFromAddr.argtypes = [wt.HANDLE, ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint64), ctypes.POINTER(SYMBOL_INFO)]
dbg.SymGetLineFromAddr64.argtypes = [wt.HANDLE, ctypes.c_uint64, ctypes.POINTER(wt.DWORD),
                                     ctypes.POINTER(IMAGEHLP_LINE64)]
dbg.SymAddrIncludeInlineTrace.restype = wt.DWORD
dbg.SymAddrIncludeInlineTrace.argtypes = [wt.HANDLE, ctypes.c_uint64]
dbg.SymQueryInlineTrace.argtypes = [wt.HANDLE, ctypes.c_uint64, wt.DWORD, ctypes.c_uint64, ctypes.c_uint64,
                                    ctypes.POINTER(wt.DWORD), ctypes.POINTER(wt.DWORD)]
dbg.SymFromInlineContext.argtypes = [wt.HANDLE, ctypes.c_uint64, wt.ULONG, ctypes.POINTER(ctypes.c_uint64),
                                     ctypes.POINTER(SYMBOL_INFO)]
dbg.SymGetLineFromInlineContext.argtypes = [wt.HANDLE, ctypes.c_uint64, wt.ULONG, ctypes.c_uint64,
                                            ctypes.POINTER(wt.DWORD), ctypes.POINTER(IMAGEHLP_LINE64)]


class ThreadContext:
    """A 16-byte aligned x64 CONTEXT."""

    def __init__(self):
        self.raw = ctypes.create_string_buffer(1232 + 16)
        self.addr = (ctypes.addressof(self.raw) + 15) & ~15

    def u64(self, off, value=None):
        p = ctypes.c_uint64.from_address(self.addr + off)
        if value is not None:
            p.value = value
        return p.value

    def u32(self, off, value=None):
        p = ctypes.c_uint32.from_address(self.addr + off)
        if value is not None:
            p.value = value
        return p.value

    def load(self, thread):
        self.u32(OFF_FLAGS, CONTEXT_FLAGS)
        return bool(k32.GetThreadContext(thread, self.addr))

    def store(self, thread):
        return bool(k32.SetThreadContext(thread, self.addr))


class Symbols:
    def __init__(self, process, search_path, invade=True):
        self.process = process
        dbg.SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_FAIL_CRITICAL_ERRORS)
        if not dbg.SymInitializeW(process, search_path, invade):
            raise OSError(f'SymInitialize failed ({ctypes.get_last_error()})')

    def find(self, module, full_name):
        found = []

        def on_symbol(sym, size, context):
            s = sym.contents
            if s.Name[:s.NameLen].decode(errors='replace') == full_name:
                found.append(s.Address)
            return True
        mask = f'{module}!*{full_name.split("::")[-1]}'.encode()
        dbg.SymEnumSymbols(self.process, 0, mask, ENUMCB(on_symbol), None)
        return sorted(set(found))

    def frames(self, address):
        """(function, file, line) at an address: the inlined frames innermost first, then the function."""
        def symbol():
            si = SYMBOL_INFO()
            si.SizeOfStruct = 88
            si.MaxNameLen = 1024
            return si

        def line():
            ln = IMAGEHLP_LINE64()
            ln.SizeOfStruct = ctypes.sizeof(IMAGEHLP_LINE64)
            return ln

        def where(ok, ln):
            return (os.path.basename(ln.FileName.decode(errors='replace')), ln.LineNumber) if ok and ln.FileName else ('', 0)

        out = []
        disp, disp32 = ctypes.c_uint64(), wt.DWORD()
        depth = dbg.SymAddrIncludeInlineTrace(self.process, address)
        context, index = wt.DWORD(), wt.DWORD()
        if depth and dbg.SymQueryInlineTrace(self.process, address, 0, address, address,
                                             ctypes.byref(context), ctypes.byref(index)):
            for i in range(depth):
                si, ln = symbol(), line()
                name = (si.Name[:si.NameLen].decode(errors='replace') if dbg.SymFromInlineContext(
                    self.process, address, context.value + i, ctypes.byref(disp), ctypes.byref(si)) else '?')
                ok = dbg.SymGetLineFromInlineContext(self.process, address, context.value + i, 0,
                                                     ctypes.byref(disp32), ctypes.byref(ln))
                out.append((name, *where(ok, ln)))
        si, ln = symbol(), line()
        name = (si.Name[:si.NameLen].decode(errors='replace')
                if dbg.SymFromAddr(self.process, address, ctypes.byref(disp), ctypes.byref(si)) else hex(address))
        ok = dbg.SymGetLineFromAddr64(self.process, address, ctypes.byref(disp32), ctypes.byref(ln))
        out.append((name, *where(ok, ln)))
        return out


def classify_frame(function, file, priority):
    name = file.lower()
    for token, view in FILE_VIEWS:
        if token in name:
            return view
    if 'r2_r_render' in name:
        return 'forward' if priority == 1 or 'render_forward' in function else 'main'
    for token, view in FUNC_VIEWS:
        if token in function:
            return 'forward' if view == 'main' and priority == 1 else view
    return None


def classify(frames, priority):
    """The innermost frame that names a view decides (an inlined HUD shadow pass is not the main view)."""
    for function, file, line in frames:
        view = classify_frame(function, file, priority)
        if view:
            return view, function, file, line
    return ('other',) + frames[-1]


def image_dir(process):
    size = wt.DWORD(1024)
    buf = ctypes.create_unicode_buffer(size.value)
    if k32.QueryFullProcessImageNameW(process, 0, buf, ctypes.byref(size)):
        return os.path.dirname(buf.value)
    return ''


def count(pid, frames, module, timeout, search_path):
    """Attaches, counts and detaches; timeout (seconds) covers symbol loading and counting."""
    begin = time.time()
    process = k32.OpenProcess(PROCESS_ALL_ACCESS, False, pid)
    if not process:
        raise OSError(f'OpenProcess({pid}) failed ({ctypes.get_last_error()})')
    search_path = search_path or image_dir(process)
    symbols = Symbols(process, search_path)
    namespace = 'xray::render::render_' + module.split('_')[-1].lower() + '::'
    names = COUNTED
    found = [symbols.find(module, namespace + n) for n in names]
    symbol_seconds = time.time() - begin
    missing = [n for n, a in zip(names, found) if len(a) != 1]
    if missing or symbol_seconds > timeout:
        dbg.SymCleanup(process)
        k32.CloseHandle(process)
        if missing:
            raise LookupError(f'{module}: no unique symbol for {", ".join(missing)} (PDB next to the DLL?)')
        raise OSError(f'symbols took {symbol_seconds:.1f} s of the {timeout:.0f} s budget: not attaching')
    addresses = [a[0] for a in found]
    kind_of = {a: i for i, a in enumerate(addresses)}
    ntdll = k32.GetModuleHandleW('ntdll.dll')
    break_in = k32.GetProcAddress(ntdll, b'DbgBreakPoint') or 0

    if not k32.DebugActiveProcess(pid):
        raise OSError(f'DebugActiveProcess({pid}) failed ({ctypes.get_last_error()})')
    k32.DebugSetProcessKillOnExit(False)

    threads = {}
    ctx = ThreadContext()
    event = ctypes.create_string_buffer(176)
    sites = {}
    passes = []
    current = {}
    anchor_hits = {'forward': collections.Counter(), 'main': collections.Counter()}
    orphans = collections.Counter()
    armed_on = 0
    armed = draining = break_requested = exited = False
    reason = ''
    started = time.time()
    last_event = started

    def set_breakpoints(thread, on):
        if not thread or not ctx.load(thread):
            return False
        for i in range(4):
            ctx.u64(OFF_DR0 + 8 * i, addresses[i] if on else 0)
        ctx.u64(OFF_DR7, DR7_EXECUTE_ALL if on else 0)
        ctx.u64(OFF_DR6, 0)
        return ctx.store(thread)

    def read_u64(address):
        value = ctypes.c_uint64()
        got = ctypes.c_size_t()
        k32.ReadProcessMemory(process, address, ctypes.byref(value), 8, ctypes.byref(got))
        return value.value

    def site_of(ret, priority):
        key = (ret, priority)
        if key not in sites:
            # The call instruction ends right before the return address; its last byte has the call's line.
            view, function, file, line = classify(symbols.frames(ret - 1), priority)
            sites[key] = {'ret': hex(ret), 'priority': priority, 'function': function, 'file': file,
                          'line': line, 'view': view}
        return sites[key]

    drain_started = 0.0
    while True:
        # Draining ends after 0.3 s without events, or after 3 s of events that are not ours (debug strings).
        if draining and time.time() - (drain_started or time.time()) > 3:
            break
        if not k32.WaitForDebugEvent(event, 50 if draining else 500):
            now = time.time()
            if draining and now - last_event > 0.3:
                break
            if not draining and not break_requested and now - begin > timeout:
                # Every thread has to be stopped to clear the registers: force an event.
                reason = reason or 'timeout'
                break_requested = True
                k32.DebugBreakProcess(process)
            continue
        last_event = time.time()
        code, event_pid, tid = (ctypes.c_uint32.from_buffer(event, o).value for o in (0, 4, 8))
        status = DBG_CONTINUE
        if code == EV_CREATE_PROCESS or code == EV_LOAD_DLL:
            file_handle = ctypes.c_uint64.from_buffer(event, 16).value
            if file_handle:
                k32.CloseHandle(file_handle)
            if code == EV_CREATE_PROCESS:
                threads[tid] = k32.OpenThread(THREAD_ALL_ACCESS, False, tid)
        elif code == EV_CREATE_THREAD:
            threads[tid] = k32.OpenThread(THREAD_ALL_ACCESS, False, tid)
            if armed and not draining:
                set_breakpoints(threads[tid], True)
        elif code == EV_EXIT_THREAD:
            handle = threads.pop(tid, None)
            if handle:
                k32.CloseHandle(handle)
            current.pop(tid, None)
        elif code == EV_EXIT_PROCESS:
            reason, exited = 'process exited', True
            k32.ContinueDebugEvent(event_pid, tid, DBG_CONTINUE)
            break
        elif code == EV_EXCEPTION:
            exc = ctypes.c_uint32.from_buffer(event, 16).value
            at = ctypes.c_uint64.from_buffer(event, 32).value
            if exc == EXCEPTION_BREAKPOINT:
                if break_in and at != break_in:
                    # The engine's own break (a fatal error): its handlers run as without a debugger.
                    status = DBG_EXCEPTION_NOT_HANDLED
                elif not armed and not break_requested:
                    # The attach break-in: every thread is stopped, arm them all.
                    armed_on = sum(1 for h in threads.values() if set_breakpoints(h, True))
                    armed = True
                    print(f'armed on {armed_on}/{len(threads)} threads', flush=True)
                elif break_requested and not draining:
                    draining = True
                    for h in threads.values():
                        set_breakpoints(h, False)
            elif exc == EXCEPTION_SINGLE_STEP and at in kind_of:
                thread = threads.get(tid) or k32.OpenThread(THREAD_ALL_ACCESS, False, tid)
                threads[tid] = thread
                ctx.load(thread)
                kind = kind_of[at]
                if not draining:
                    if kind == 0:
                        ret = read_u64(ctx.u64(OFF_RSP))
                        priority = ctx.u64(OFF_RDX) & 0xFFFFFFFF
                        site = site_of(ret, priority)
                        passes.append([len(passes), (ret, priority), 0, 0, 0])
                        current[tid] = len(passes) - 1
                        if site['view'] in ('forward', 'main'):
                            anchor_hits[site['view']][ret] += 1
                            # Forward passes end frames: the partial one, the warm-up, the counted ones.
                            # Without them, main passes start frames and one more closes the last.
                            if (max(anchor_hits['forward'].values(), default=0) >= frames + 2 or
                                    not anchor_hits['forward'] and max(anchor_hits['main'].values()) >= frames + 3):
                                reason = 'counted'
                    else:
                        index = current.get(tid)
                        if index is None:
                            orphans[KINDS[kind]] += 1
                        else:
                            passes[index][1 + kind] += 1
                # Resume flag: run the instruction without hitting the breakpoint again.
                ctx.u32(OFF_EFLAGS, ctx.u32(OFF_EFLAGS) | EFLAGS_RF)
                ctx.u64(OFF_DR6, 0)
                ctx.store(thread)
                if reason == 'counted' and not draining:
                    draining = True
                    for h in threads.values():
                        set_breakpoints(h, False)
            else:
                status = DBG_EXCEPTION_NOT_HANDLED
        if draining and not drain_started:
            drain_started = time.time()
        k32.ContinueDebugEvent(event_pid, tid, status)

    if not exited:
        k32.DebugActiveProcessStop(pid)
    for h in threads.values():
        if h:
            k32.CloseHandle(h)
    seconds = time.time() - started
    dbg.SymCleanup(process)
    k32.CloseHandle(process)
    return summarize(frames, module, pid, seconds, symbol_seconds, armed_on, reason, addresses, names, sites,
                     passes, orphans)


def summarize(frames, module, pid, seconds, symbol_seconds, armed_on, reason, addresses, names, sites, passes,
              orphans):
    # Several main-view sites in CRender::Render: the earliest in the source is the z-prefill.
    main_sites = sorted((s['line'], key) for key, s in sites.items() if s['view'] == 'main' and s['priority'] == 0)
    if len(main_sites) > 1:
        for _, key in main_sites[:-1]:
            sites[key]['view'] = 'zfill'
    forward = collections.Counter(p[1] for p in passes if sites[p[1]]['view'] == 'forward')
    if forward:
        # Frames end with the forward pass: up to the first one is partial, the next is the warm-up.
        anchor = forward.most_common(1)[0][0]
        boundaries = [p[0] for p in passes if p[1] == anchor]
        frame_of = lambda seq: bisect.bisect_left(boundaries, seq) - 2
    else:
        # Main passes start frames: the first period is the warm-up, the last one may be cut short.
        anchor = main_sites[-1][1] if main_sites else None
        boundaries = [p[0] for p in passes if p[1] == anchor]
        frame_of = lambda seq: bisect.bisect_right(boundaries, seq) - 2
    measured = max(0, min(frames, len(boundaries) - 2))
    per_frame = [collections.defaultdict(lambda: [0, 0, 0, 0]) for _ in range(measured)]
    for p in passes:
        frame = frame_of(p[0])
        if 0 <= frame < measured:
            site = sites[p[1]]
            row = per_frame[frame][f'{site["view"]}/p{site["priority"]}']
            row[0] += 1
            for k in range(1, 4):
                row[k] += p[1 + k]

    def stats(rows):
        n = max(1, measured)
        graph, fill, st, pm = (sum(r[k] for r in rows) / n for k in range(4))
        inst = [r[1] + r[2] + r[3] for r in rows] + [0] * (measured - len(rows))
        return {'passes': round(graph, 2), 'instances': round(fill + st + pm, 2), 'batched': round(fill, 2),
                'single': round(st + pm, 2), 'st': round(st, 2), 'pm': round(pm, 2),
                'instances_min': min(inst) if inst else 0, 'instances_max': max(inst) if inst else 0}

    view_keys = sorted({k for f in per_frame for k in f})
    views = {k: stats([f[k] for f in per_frame if k in f]) for k in view_keys}

    def group(pred):
        rows = []
        for f in per_frame:
            acc = [0, 0, 0, 0]
            for k, r in f.items():
                if pred(k.split('/')[0]):
                    acc = [a + b for a, b in zip(acc, r)]
            rows.append(acc)
        return stats(rows)

    groups = {
        'main': group(lambda v: v == 'main'),
        'shadow': group(lambda v: v in SHADOW_VIEWS),
        'cascades': group(lambda v: v == 'sun'),
        'rain': group(lambda v: v == 'rain'),
        'lights': group(lambda v: v == 'lights'),
        'hud_shadow': group(lambda v: v == 'hud_shadow'),
        'all': group(lambda v: True),
    }
    frame_rows = [{k: {'passes': r[0], 'instances': r[1] + r[2] + r[3]} for k, r in sorted(f.items())} for f in per_frame]
    return {
        'module': module, 'pid': pid, 'frames_requested': frames, 'frames_measured': measured,
        'seconds': round(seconds, 1), 'symbol_seconds': round(symbol_seconds, 1), 'armed_threads': armed_on,
        'stop': reason,
        'symbols': {n: hex(a) for n, a in zip(names, addresses)},
        'sites': sorted(sites.values(), key=lambda s: (s['view'], s['file'], s['line'], s['priority'])),
        'hits_before_first_pass': dict(orphans), 'views': views, 'groups': groups, 'per_frame': frame_rows,
    }


def print_table(result):
    print(f'count_tree_draws: {result["module"]} pid {result["pid"]}: {result["frames_measured"]} frame(s) of '
          f'{result["frames_requested"]} in {result["seconds"]} s (symbols {result["symbol_seconds"]} s), '
          f'armed {result["armed_threads"]} threads, '
          f'stop: {result["stop"]}')
    print('  call sites:')
    for s in result['sites']:
        print(f'    {s["view"]:<10} p{s["priority"]}  {s["file"] or "?"}:{s["line"]}  {s["function"]}')
    header = f'  {"view":<14}{"passes":>8}{"instances":>11}{"batched":>9}{"single":>8}{"st":>7}{"pm":>7}  min..max'
    print(header)

    def row(name, v):
        print(f'  {name:<14}{v["passes"]:>8}{v["instances"]:>11}{v["batched"]:>9}{v["single"]:>8}'
              f'{v["st"]:>7}{v["pm"]:>7}  {v["instances_min"]}..{v["instances_max"]}')
    for k, v in result['views'].items():
        row(k, v)
    print('  ' + '-' * (len(header) - 2))
    for k in ('main', 'shadow', 'cascades', 'lights', 'rain', 'hud_shadow', 'all'):
        row(k, result['groups'][k])
    if result['hits_before_first_pass']:
        print(f'  not counted, passes already running at the attach: {result["hits_before_first_pass"]}')


def static_sites(dll):
    """Every direct call of render_graph in a renderer DLL with its view (no engine needed)."""
    data = open(dll, 'rb').read()
    pe = struct.unpack_from('<I', data, 0x3C)[0]
    sections = struct.unpack_from('<H', data, pe + 6)[0]
    optional = struct.unpack_from('<H', data, pe + 20)[0]
    image_base = struct.unpack_from('<Q', data, pe + 48)[0]
    module = os.path.splitext(os.path.basename(dll))[0]
    process = wt.HANDLE(0x7D5)
    symbols = Symbols(process, os.path.dirname(os.path.abspath(dll)), invade=False)
    dbg.SymLoadModuleExW.restype = ctypes.c_uint64
    dbg.SymLoadModuleExW.argtypes = [wt.HANDLE, wt.HANDLE, wt.LPCWSTR, wt.LPCWSTR, ctypes.c_uint64, wt.DWORD,
                                     ctypes.c_void_p, wt.DWORD]
    if not dbg.SymLoadModuleExW(process, None, dll, None, image_base, 0, None, 0):
        raise OSError(f'SymLoadModuleEx({dll}) failed ({ctypes.get_last_error()})')
    namespace = 'xray::render::render_' + module.split('_')[-1].lower() + '::'
    # Every breakpoint target of a count must resolve to one address, as count() requires.
    found = [symbols.find(module, namespace + n) for n in COUNTED]
    missing = [n for n, a in zip(COUNTED, found) if len(a) != 1]
    if missing:
        raise LookupError(f'{module}: no unique symbol for {", ".join(missing)}')
    target = found[0]
    sites = []
    table = pe + 24 + optional
    for i in range(sections):
        _, _, va, raw_size, raw_ptr = struct.unpack_from('<8sIIII', data, table + 40 * i)
        # Executable sections only.
        if not struct.unpack_from('<I', data, table + 40 * i + 36)[0] & 0x20000000:
            continue
        code = data[raw_ptr:raw_ptr + raw_size]
        for j in range(len(code) - 5):
            if code[j] == 0xE8:
                ret = image_base + va + j + 5
                if ret + struct.unpack_from('<i', code, j + 1)[0] == target[0]:
                    view, function, file, line = classify(symbols.frames(ret - 1), -1)
                    sites.append({'ret': hex(ret), 'view': view, 'function': function, 'file': file, 'line': line})
    dbg.SymCleanup(process)
    # The z-prefill is CRender::Render's earliest call (the split and unsplit main passes follow).
    main = sorted((s for s in sites if s['view'] == 'main'), key=lambda s: s['line'])
    if len(main) > 1:
        main[0]['view'] = 'zfill'
    return sites


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('pid', type=int, nargs='?')
    ap.add_argument('--frames', type=int, default=12)
    ap.add_argument('--module', default='xrRender_R5', help='xrRender_R5 (DirectX 12) or xrRender_R4 (DirectX 11)')
    ap.add_argument('--timeout', type=float, default=120, help='seconds before the counter gives up')
    ap.add_argument('--symbols', default='', help='symbol search path (default: the engine\'s folder)')
    ap.add_argument('--json', default='', help='write the result here')
    ap.add_argument('--sites', metavar='DLL', default='',
                    help='instead of counting: check the counted symbols and list and classify every render_graph '
                         'call of this DLL (PDB beside it)')
    args = ap.parse_args()
    try:
        if args.sites:
            sites = static_sites(args.sites)
            for s in sites:
                print(f'{s["view"]:<10} {s["file"] or "?"}:{s["line"]}  {s["function"]}')
            unknown = [s for s in sites if s['view'] == 'other']
            if unknown:
                print(f'{len(unknown)} call site(s) without a view', file=sys.stderr)
            return 1 if unknown or not sites else 0
        if args.pid is None:
            ap.error('a pid (or --sites DLL) is required')
        result = count(args.pid, args.frames, args.module, args.timeout, args.symbols)
    except (OSError, LookupError) as e:
        print(f'count_tree_draws: {e}', file=sys.stderr)
        return 1
    print_table(result)
    if args.json:
        with open(args.json, 'w', encoding='utf-8') as f:
            json.dump(result, f, indent=1)
    return 0 if result['frames_measured'] >= args.frames else 1


if __name__ == '__main__':
    sys.exit(main())
