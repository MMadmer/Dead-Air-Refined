"""Compile every R5 shader permutation with DXC (DXIL, SM 6.x) the way the engine does, and audit it
(docs/dead-air/dx12-features/dxc.md 6.1).

Built on tools/qa/Check-ShaderLinkage.py: the same shader tree (archive layers + loose overlay), the
same pipelines (C++ r_Pass, .s scripts, uber_deffer, the lanes' EXTRA_PIPELINES), the same
permutations (preset ladder x bare/+game x skinning) with the DirectX 12 define set (SMAP_ARRAY), plus
every other shader file of the tree (compute shaders, unreferenced files, mod overrides). For each
unique preprocessed shader:

  * DXC compile through Externals/dxc in process with the engine's setup (dxc.md 3.3): FXC's
    preprocessor with the engine's includer and the XFR_* macros, then DXC on the self-contained
    text with -HV 2018 -Zpr -O3 -flegacy-resource-reservation and the silenced legacy warnings
    (.ms/.as with -HV 2021);
  * FXC compile of the same permutation (d3dcompiler_47, the engine's flags, the X3523 retry);
  * reflection of both (ID3D12ShaderReflection) run through what R_constant_table::parse and the
    layer's Shader::Reflect accept, and compared field by field;
  * stage linkage of every pipeline with the DXIL signatures (Check-ShaderLinkage.link);
  * compile time of both compilers.

    python tools\\qa\\Check-DxcShaders.py --sm 6_6                  # the preset permutations
    python tools\\qa\\Check-DxcShaders.py --sm 6_6 --console-extras # + console-only switches, one at a time
    python tools\\qa\\Check-DxcShaders.py --filter taa              # pipelines/files with this in a name
    python tools\\qa\\Check-DxcShaders.py --preprocess dxc          # DXC's own preprocessor + include handler
    python tools\\qa\\Check-DxcShaders.py --out DIR                 # DXIL blobs + pipelines.txt for pso_probe

tools\\qa\\dxc\\Run-DxcAudit.ps1 runs it with --out and creates every pipeline on the device.

Exit code 1 when DXC refuses a shader FXC compiles, when a reflection would be mishandled in a way FXC's
is not, or when a pipeline outside the legacy-switch set (Check-ShaderLinkage.GATES) does not link.
"""
import argparse
import concurrent.futures
import ctypes
import hashlib
import importlib.util
import json
import os
import pathlib
import re
import struct
import sys
import threading
import time
from ctypes import POINTER, byref, c_long, c_size_t, c_uint, c_ulong, c_void_p, c_wchar_p

REPO = pathlib.Path(__file__).resolve().parents[2]
_spec = importlib.util.spec_from_file_location("linkage", REPO / "tools" / "qa" / "Check-ShaderLinkage.py")
L = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(L)

EXTS = ("vs", "ps", "gs", "hs", "ds", "cs", "ms", "as")
E_FAIL = -2147467259
IID_ID3D12ShaderReflection = L.GUID.of("5A58797D-A72C-478D-8BA2-EFC6B0EFE88E")
IID_IDxcIncludeHandler = L.GUID.of("7f61fc7d-950d-467f-b3e3-3c02fb49187c")
vcall = L.vcall
release = L.com_release


# ---- DXC's own preprocessor (--preprocess dxc): an include handler in Python ----------------

QI_FN = ctypes.WINFUNCTYPE(c_long, c_void_p, POINTER(L.GUID), POINTER(c_void_p))
REF_FN = ctypes.WINFUNCTYPE(c_ulong, c_void_p)
LOAD_FN = ctypes.WINFUNCTYPE(c_long, c_void_p, c_wchar_p, POINTER(c_void_p))


class IncludeVtbl(ctypes.Structure):
    _fields_ = [("QueryInterface", QI_FN), ("AddRef", REF_FN), ("Release", REF_FN), ("LoadSource", LOAD_FN)]


class IncludeObj(ctypes.Structure):
    _fields_ = [("lpVtbl", POINTER(IncludeVtbl))]


class Handler:
    """Engine-equivalent include resolution: the r3 root first, then with the includer's directory the
    compiler put in front of the name stripped one level at a time (FXC gets the bare name and
    r4_shaders.cpp always resolves it from the r3 root)."""

    def __init__(self, tree, utils_factory):
        self.tree = tree
        self.utils_factory = utils_factory
        self.missing = set()
        self.nonutf8 = set()
        self._qi = QI_FN(self._query)
        self._ref = REF_FN(lambda _this: 1)
        self._load = LOAD_FN(self._on_load)
        self._vtbl = IncludeVtbl(self._qi, self._ref, self._ref, self._load)
        self.obj = IncludeObj(ctypes.pointer(self._vtbl))

    def _query(self, this, riid, ppv):
        ppv[0] = this
        return 0

    def resolve(self, name):
        path = name.replace("\\", "/")
        while path.startswith("./"):
            path = path[2:]
        parts = [p for p in path.split("/") if p not in ("", ".")]
        if parts and parts[0].lower() == "r3":
            parts = parts[1:]
        while parts:
            hit = self.tree.lookup("/".join(parts))
            if hit:
                return hit
            parts = parts[1:]
        return None

    def _on_load(self, _this, filename, pp_blob):
        try:
            hit = self.resolve(filename)
            if not hit:
                self.missing.add(filename)
                pp_blob[0] = None
                return E_FAIL
            raw = hit[0].read_bytes()
            data = L.sanitize(raw)
            if data is not raw:
                self.nonutf8.add(hit[0].name)
            blob = c_void_p()
            hr = vcall(self.utils_factory(), 6, c_long, [c_void_p, c_uint, c_uint, POINTER(c_void_p)], data, len(data),
                       L.DXC_CP_UTF8, byref(blob))
            pp_blob[0] = blob.value
            return hr
        except Exception:  # noqa: BLE001 - never raise into the compiler
            return E_FAIL


class Dxc(L.Dxc):
    def __init__(self, tree, dxc_dir):
        super().__init__(dxc_dir)
        self.handler = Handler(tree, self.utils)

    def compile_raw(self, source, file_name, entry, target, hv, args, defines):
        """DXC's own preprocessor with the include handler (the --preprocess dxc experiment)."""
        argv_list = [file_name, "-E", entry, "-T", target, "-HV", hv] + list(args)
        for k, v in defines:
            argv_list += ["-D", f"{k}={v}"]
        argv = (c_wchar_p * len(argv_list))(*argv_list)
        src = L.sanitize(source)
        buf = L.DxcBuffer(ctypes.cast(ctypes.c_char_p(src), c_void_p), len(src), L.DXC_CP_UTF8)
        result = c_void_p()
        hr = vcall(self.compiler(), 3, c_long,
                   [POINTER(L.DxcBuffer), POINTER(c_wchar_p), c_uint, c_void_p, POINTER(L.GUID), POINTER(c_void_p)],
                   byref(buf), argv, len(argv_list), ctypes.addressof(self.handler.obj), byref(L.IID_IDxcResult), byref(result))
        if hr < 0 or not result.value:
            return None, None, f"Compile hr 0x{hr & 0xffffffff:08x}"
        res = result.value
        status = c_long()
        vcall(res, 3, c_long, [POINTER(c_long)], byref(status))
        get = [c_uint, POINTER(L.GUID), POINTER(c_void_p), POINTER(c_void_p)]
        errors = c_void_p()
        vcall(res, 7, c_long, get, L.DXC_OUT_ERRORS, byref(L.IID_IDxcBlobUtf8), byref(errors), None)
        msg = ""
        if errors.value:
            msg = L.dxc_blob_data(errors.value).rstrip(b"\0").decode("utf-8", "replace")
            release(errors.value)
        obj = refl = None
        if status.value >= 0:
            o, r = c_void_p(), c_void_p()
            vcall(res, 7, c_long, get, L.DXC_OUT_OBJECT, byref(L.IID_IDxcBlob), byref(o), None)
            if o.value:
                obj = L.dxc_blob_data(o.value)
                release(o.value)
            vcall(res, 7, c_long, get, L.DXC_OUT_REFLECTION, byref(L.IID_IDxcBlob), byref(r), None)
            if r.value:
                refl = L.dxc_blob_data(r.value)
                release(r.value)
        release(res)
        return obj, refl, msg

    def reflect(self, data):
        buf = L.DxcBuffer(ctypes.cast(ctypes.c_char_p(data), c_void_p), len(data), 0)
        r = c_void_p()
        hr = vcall(self.utils(), 13, c_long, [POINTER(L.DxcBuffer), POINTER(L.GUID), POINTER(c_void_p)], byref(buf),
                   byref(IID_ID3D12ShaderReflection), byref(r))
        return r.value if hr >= 0 else None


# ---- reflection -> plain data ---------------------------------------------------------------

SVT = {0: "void", 1: "bool", 2: "int", 3: "float", 19: "uint", 20: "uint8", 39: "double", 57: "min8float", 58: "min10float",
       59: "min16float", 60: "min12int", 61: "min16int", 62: "min16uint", 63: "int16", 64: "uint16", 65: "float16", 66: "int64", 67: "uint64"}
SIT = {0: "cbuffer", 1: "tbuffer", 2: "texture", 3: "sampler", 4: "uav_rwtyped", 5: "structured", 6: "uav_rwstructured", 7: "byteaddress",
       8: "uav_rwbyteaddress", 9: "uav_append", 10: "uav_consume", 11: "uav_rwstructured_counter", 12: "rtaccel", 13: "uav_feedback"}


def read_reflection(r):
    """ID3D12ShaderReflection -> constant buffers (variables with offsets and types) and bindings.
    Reflection sub-objects have no IUnknown: their vtables start at their own methods."""
    desc = ctypes.create_string_buffer(256)
    vcall(r, 3, c_long, [c_void_p], desc)
    version, = struct.unpack_from("<I", desc, 0)
    ncb, nbound, _nin, _nout = struct.unpack_from("<4I", desc, 20)
    out = {"version": version, "cbs": [], "binds": []}
    for i in range(ncb):
        cb = vcall(r, 4, c_void_p, [c_uint], i)
        bd = ctypes.create_string_buffer(32)
        vcall(cb, 0, c_long, [c_void_p], bd)
        name_ptr, = struct.unpack_from("<Q", bd, 0)
        ctype, nvars, size, _flags = struct.unpack_from("<4I", bd, 8)
        entry = {"name": ctypes.string_at(name_ptr).decode("latin-1") if name_ptr else "", "type": ctype, "size": size, "vars": []}
        for v in range(nvars):
            var = vcall(cb, 1, c_void_p, [c_uint], v)
            vd = ctypes.create_string_buffer(64)
            vcall(var, 0, c_long, [c_void_p], vd)
            vname, = struct.unpack_from("<Q", vd, 0)
            start, vsize, vflags = struct.unpack_from("<3I", vd, 8)
            ty = vcall(var, 1, c_void_p, [])
            td = ctypes.create_string_buffer(48)
            vcall(ty, 0, c_long, [c_void_p], td)
            cls, typ, rows, cols, elems, _members = struct.unpack_from("<6I", td, 0)
            entry["vars"].append({"name": ctypes.string_at(vname).decode("latin-1") if vname else "", "offset": start, "size": vsize,
                                  "used": bool(vflags & 2), "class": cls, "type": typ, "rows": rows, "cols": cols, "elements": elems})
        out["cbs"].append(entry)
    for i in range(nbound):
        b = ctypes.create_string_buffer(64)
        vcall(r, 6, c_long, [c_uint, c_void_p], i, b)
        name_ptr, = struct.unpack_from("<Q", b, 0)
        btype, point, count, _bflags, _ret, dim, _samples, space = struct.unpack_from("<8I", b, 8)
        out["binds"].append({"name": ctypes.string_at(name_ptr).decode("latin-1") if name_ptr else "", "type": btype, "point": point,
                             "count": count, "dim": dim, "space": space})
    return out


def engine_verdict(refl):
    """What R_constant_table::parse (dx11r_constants.cpp) and the layer's BindSlot (d12_objects.cpp)
    would do with this reflection: fatal errors, VERIFY failures, wrong constant-buffer slots."""
    problems = []
    cb_point = {b["name"]: b["point"] for b in refl["binds"] if b["type"] in (0, 1)}
    for i, cb in enumerate(refl["cbs"]):
        if cb["type"] not in (0, 1):
            continue
        # parse() binds a buffer at its reflection index (BindPoint for DXIL once PLAN C6 landed).
        if cb_point.get(cb["name"], i) != i:
            problems.append(("slot", f"cbuffer {cb['name']} is b{cb_point.get(cb['name'])} but reflection index {i}"))
        for v in cb["vars"]:
            cls, typ = v["class"], v["type"]
            if cls == 4:
                continue
            if cls == 5:
                problems.append(("fatal", f"{cb['name']}.{v['name']}: struct ('Pclass D3DXPC_STRUCT unsupported')"))
                continue
            if cls == 3:
                problems.append(("fatal", f"{cb['name']}.{v['name']}: column-major matrix"))
                continue
            if typ not in (1, 2, 3):
                problems.append(("fatal", f"{cb['name']}.{v['name']}: type {SVT.get(typ, typ)} ('unexpected shader variable type')"))
                continue
            if cls == 1 and v["cols"] not in (2, 3, 4):
                problems.append(("fatal", f"{cb['name']}.{v['name']}: 1-component vector"))
            if cls == 2 and (v["cols"] != 4 or v["rows"] not in (2, 3, 4)):
                problems.append(("fatal", f"{cb['name']}.{v['name']}: matrix {v['rows']}x{v['cols']}"))
    for b in refl["binds"]:
        t, p, c = b["type"], b["point"], b["count"]
        # d12_layer.h limits: 14 CBVs, 32 SRVs, 16 samplers, 8 UAVs.
        limit = {0: 14, 1: 32, 2: 32, 3: 16, 5: 32, 7: 32}.get(t, 8)
        if p + max(c, 1) > limit:
            problems.append(("layer", f"{SIT.get(t, t)} {b['name']} at {p}+{c} beyond the layer limit {limit}"))
        if t in (2, 3, 4) and c != 1:
            problems.append(("verify", f"{SIT.get(t, t)} {b['name']} BindCount {c} (VERIFY(BindCount == 1))"))
        if b["space"] != 0:
            problems.append(("layer", f"{b['name']} in register space {b['space']}"))
    return problems


def compare(fxc, dxc):
    """Differences that change what the engine binds or writes."""
    diffs = []
    fcb = {c["name"]: c for c in fxc["cbs"]}
    dcb = {c["name"]: c for c in dxc["cbs"]}
    for name in sorted(set(fcb) | set(dcb)):
        a, b = fcb.get(name), dcb.get(name)
        if not a or not b:
            diffs.append(f"cbuffer {name} only in {'FXC' if a else 'DXC'}")
            continue
        if a["size"] != b["size"]:
            diffs.append(f"cbuffer {name} size {a['size']} vs {b['size']}")
        av = {v["name"]: v for v in a["vars"]}
        bv = {v["name"]: v for v in b["vars"]}
        for vn in sorted(set(av) | set(bv)):
            x, y = av.get(vn), bv.get(vn)
            if not x or not y:
                diffs.append(f"{name}.{vn} only in {'FXC' if x else 'DXC'}")
                continue
            for k in ("offset", "size", "class", "type", "rows", "cols", "elements"):
                if x[k] != y[k]:
                    diffs.append(f"{name}.{vn} {k} {x[k]} vs {y[k]}")
    fi = [c["name"] for c in fxc["cbs"]]
    di = [c["name"] for c in dxc["cbs"]]
    if fi != di:
        diffs.append(f"cbuffer order {fi} vs {di}")
    fb = {b["name"]: b for b in fxc["binds"]}
    db = {b["name"]: b for b in dxc["binds"]}
    for n in sorted(set(fb) | set(db)):
        x, y = fb.get(n), db.get(n)
        if not x or not y:
            diffs.append(f"binding {n} only in {'FXC' if x else 'DXC'}")
            continue
        for k in ("type", "point", "count", "dim"):
            if x[k] != y[k]:
                diffs.append(f"binding {n} {k} {x[k]} vs {y[k]}")
    return diffs


class Fxc:
    def __init__(self, compiler):
        self.c = compiler
        self.reflect_fn = compiler.dll.D3DReflect
        self.reflect_fn.restype = c_long
        self.reflect_fn.argtypes = [c_void_p, c_size_t, POINTER(L.GUID), POINTER(c_void_p)]

    def reflect(self, code):
        r = c_void_p()
        hr = self.reflect_fn(code, len(code), byref(IID_ID3D12ShaderReflection), byref(r))
        return r.value if hr >= 0 else None


# ---- jobs -------------------------------------------------------------------------------------

# r4_shaders.cpp options reachable only from the console (r2_ls_flags, r2_ssao_mode, r2_sun_quality,
# r2_sun_shafts, r2_sun_filter, r2_minmax_sm, r2_gloss, r2_tshadows, r2_mblur...) and the debug switch
# -gloss N. Not among them: USE_SJITTER and SKIN_COLOR (-sjitter and -skinw are no-ops, r2.cpp: the
# shaders lost what they switched on) and USE_R2_STATIC_SUN (only R4's R2A mode sets it).
CONSOLE_EXTRAS = (
    ("HDAO=1",), ("USE_HBAO=1", "SSAO_OPT_DATA=1"), ("USE_HBAO=1", "SSAO_OPT_DATA=2", "VECTORIZED_CODE=1"),
    ("USE_MINMAX_SM=1",), ("USE_SUNFILTER=1",), ("USE_TSHADOWS=1",), ("USE_MBLUR=1",),
    ("FORCE_GLOSS=0.500000",), ("SUN_QUALITY=1",), ("SUN_QUALITY=2",),
    ("SUN_QUALITY=3",), ("SUN_QUALITY=3", "USE_MINMAX_SM=1"), ("SUN_SHAFTS_QUALITY=1",), ("SSAO_QUALITY=1",), ("SSAO_QUALITY=2",),
    ("USE_SSAO_BLUR=1",), ("USE_TECHICOLOR=1", "USE_VIGNETTE=1", "USE_FILMGRAIN=1"), ("SM_4_1=1",), ("DOUBLE_PRECISION=1",),
)


class Result:
    __slots__ = ("stage", "name", "entry", "ok", "error", "warnings", "fxc_ok", "fxc_error", "dx9", "problems", "diffs",
                 "dxc_ms", "fxc_ms", "sig", "obj", "size_dxil", "size_dxbc", "hv2021", "same_as_raw")

    def __init__(self, stage="", name="", entry="main"):
        self.stage, self.name, self.entry = stage, name, entry
        self.ok = self.fxc_ok = self.dx9 = False
        self.error = self.fxc_error = self.warnings = ""
        self.problems, self.diffs = [], []
        self.dxc_ms = self.fxc_ms = 0.0
        self.sig = self.obj = self.hv2021 = self.same_as_raw = None
        self.size_dxil = self.size_dxbc = 0


class Auditor:
    def __init__(self, tree, args):
        self.tree = tree
        self.sdk = L.find_sdk_bin()
        self.fxc_compiler = L.Compiler(self.sdk, tree)
        self.fxc = Fxc(self.fxc_compiler)
        self.dxc = Dxc(tree, args.dxc_dir)
        self.sm, self.hv, self.check2021 = args.sm, args.hv, args.check_2021
        self.preprocess, self.compare_raw = args.preprocess, args.compare_raw
        self.args = tuple(L.DXC_ENGINE_ARGS if args.engine_args else ("-Zpr", "-O3", "-Qstrip_debug", "-Qstrip_reflect")) + \
            tuple(args.dxc_arg)
        self.xfr = L.dxc_macros(args.sm, args.wave_lanes, not args.no_min16)
        self.lock = threading.Lock()
        self.by_input = {}
        self.by_text = {}

    def _once(self, cache, key, fn):
        with self.lock:
            v = cache.get(key)
            if v is not None and not isinstance(v, threading.Event):
                return v
            if v is None:
                ev = threading.Event()
                cache[key] = ev
                owner = True
            else:
                ev = v
                owner = False
        if not owner:
            ev.wait()
            with self.lock:
                return cache[key]
        try:
            value = fn()
        except Exception as exc:  # noqa: BLE001
            value = Result()
            value.error = f"internal: {exc!r}"
        with self.lock:
            cache[key] = value
        ev.set()
        return value

    def build(self, stage, name, defines):
        base = name.split("(")[0]
        return self._once(self.by_input, (stage, base.lower(), defines), lambda: self._build(stage, base, defines))

    def _build(self, stage, name, defines):
        hit = self.tree.lookup(f"{name}.{stage}")
        if not hit:
            return None
        raw = hit[0].read_bytes()
        entry = "main"
        for legacy in ((b"main_vs_1_1", b"main_vs_2_0") if stage == "vs" else
                       (b"main_ps_1_1", b"main_ps_1_2", b"main_ps_1_3", b"main_ps_1_4", b"main_ps_2_0") if stage == "ps" else ()):
            if legacy in raw:
                entry = legacy.decode()
                break
        pairs = tuple(tuple(d.split("=", 1)) if "=" in d else (d, "1") for d in defines)
        # FXC sees the engine's options; DXC also the XFR_* macros.
        text, err = self.fxc_compiler.preprocess(raw, list(pairs))
        dtext, derr = self.fxc_compiler.preprocess(raw, list(pairs) + self.xfr, f"{name}.{stage}".encode())
        digest = hashlib.sha1((text if text is not None else raw + repr(pairs).encode()) +
                              b"\0" + (dtext if dtext is not None else b"")).hexdigest()
        return self._once(self.by_text, (digest, entry, stage),
                          lambda: self._compile(stage, f"{name}.{stage}", raw, text, err, dtext, derr, entry, pairs))

    def _compile(self, stage, file_name, raw, text, pp_err, dtext, dpp_err, entry, pairs):
        r = Result(stage, file_name, entry)
        # FXC as the engine does it (X3523 retry with backwards compatibility); no FXC for mesh stages.
        t = time.perf_counter()
        fxc_code = None
        if stage in L.DXC_ONLY_STAGES:
            r.fxc_ok, r.fxc_error = True, ""
        elif text is None:
            r.fxc_ok, r.fxc_error = False, L.first_error(pp_err)
        else:
            fxc_code, ferr = self.fxc_compiler.compile(text, entry, L.TARGETS[stage], L.BASE_FLAGS)
            if fxc_code is None and "error X3523" in ferr:
                r.dx9 = True
                fxc_code, ferr = self.fxc_compiler.compile(text, entry, L.TARGETS[stage],
                                                           L.BASE_FLAGS | L.D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY)
            r.fxc_ok = fxc_code is not None
            r.fxc_error = "" if r.fxc_ok else L.first_error(ferr)
        r.fxc_ms = (time.perf_counter() - t) * 1000.0
        r.size_dxbc = len(fxc_code) if fxc_code else 0
        # DXC: the engine's FXC-preprocessed text, or (experiment) DXC's own preprocessor on the raw file.
        hv = "2021" if stage in L.HLSL_2021_STAGES else self.hv
        target = L.dxc_target(stage, self.sm)
        t = time.perf_counter()
        if self.preprocess == "fxc":
            if dtext is None:
                obj, refl, msg = None, None, f"preprocess: {L.first_error(dpp_err)}"
            else:
                obj, refl, msg = self.dxc.compile(dtext, file_name, entry, target, hv, self.args)
            if self.compare_raw and dtext is not None:
                obj_raw, _, _ = self.dxc.compile_raw(raw, file_name, entry, target, hv, self.args, list(pairs) + self.xfr)
                r.same_as_raw = (obj_raw == obj) if (obj_raw and obj) else (obj_raw is None and obj is None)
        else:
            obj, refl, msg = self.dxc.compile_raw(raw, file_name, entry, target, hv, self.args, list(pairs) + self.xfr)
        r.dxc_ms = (time.perf_counter() - t) * 1000.0
        r.ok = obj is not None
        r.obj = obj
        r.size_dxil = len(obj) if obj else 0
        r.error = "" if r.ok else L.first_dxc_error(msg)
        r.warnings = summarize_warnings(msg) if r.ok else ""
        if self.check2021 and hv != "2021" and dtext is not None:
            o2, _, m2 = self.dxc.compile(dtext, file_name, entry, target, "2021", self.args)
            r.hv2021 = "" if o2 is not None else L.first_dxc_error(m2)
        if r.ok:
            try:
                r.sig = L.Signatures(obj)
            except Exception as exc:  # noqa: BLE001
                r.problems.append(("internal", f"signature parse: {exc!r}"))
            dref = self.dxc.reflect(refl) if refl else None
            if not dref:
                r.problems.append(("internal", "no DXC reflection"))
            else:
                d = read_reflection(dref)
                release(dref)
                r.problems += engine_verdict(d)
                if fxc_code:
                    fref = self.fxc.reflect(fxc_code)
                    if fref:
                        f = read_reflection(fref)
                        release(fref)
                        fxc_problems = set(engine_verdict(f))
                        # Keep only what DXC adds: what FXC already trips over is today's behaviour.
                        r.problems = [p for p in r.problems if p not in fxc_problems] + \
                                     [("fxc-too", p[1]) for p in r.problems if p in fxc_problems]
                        r.diffs = compare(f, d)
        return r


def summarize_warnings(msg):
    kinds = {}
    for line in msg.splitlines():
        m = re.search(r"warning: (.*?)(\[-W[\w-]+\])?$", line)
        if m:
            key = m.group(2) or m.group(1)[:60]
            kinds[key] = kinds.get(key, 0) + 1
    return ", ".join(f"{k} x{v}" for k, v in sorted(kinds.items(), key=lambda kv: -kv[1]))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--unpack", type=pathlib.Path, default=L.DEFAULT_UNPACK)
    ap.add_argument("--loose", type=pathlib.Path, default=L.DEFAULT_LOOSE)
    ap.add_argument("--filter", default="")
    ap.add_argument("--sm", default="6_0", help="target shader model, 6_0 .. 6_8")
    ap.add_argument("--hv", default="2018", help="HLSL version of the legacy shader base")
    ap.add_argument("--check-2021", action="store_true", help="also count what -HV 2021 refuses")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--out", type=pathlib.Path, help="write DXIL blobs + pipelines.txt for tools\\qa\\dxc\\pso_probe")
    ap.add_argument("--json", type=pathlib.Path)
    ap.add_argument("--dxc-dir", type=pathlib.Path, default=L.DEFAULT_DXC_DIR)
    ap.add_argument("--dxc-arg", action="append", default=[], help="extra DXC argument (repeatable)")
    ap.add_argument("--no-engine-args", dest="engine_args", action="store_false",
                    help="drop -flegacy-resource-reservation and the silenced warnings (plain -Zpr -O3)")
    ap.add_argument("--wave-lanes", type=int, default=32, help="XFR_WAVE_LANES (0 = a device without wave ops)")
    ap.add_argument("--no-min16", action="store_true", help="a device without 16-bit min precision (no XFR_MIN16)")
    ap.add_argument("--console-extras", action="store_true", help="also the console-only switches, one at a time")
    ap.add_argument("--preprocess", choices=("dxc", "fxc"), default="fxc", help="who expands includes and macros")
    ap.add_argument("--compare-raw", action="store_true", help="with --preprocess fxc: also compile the raw file, compare the DXIL")
    args = ap.parse_args()

    tree = L.Tree(args.unpack, args.loose)
    unresolved = []
    r4 = {"USE_DX11": "1", "RENDER": "4", "R_GL": "0", "R_R1": "1", "R_R2": "2", "R_R3": "3", "R_R4": "4",
          "HAS_DX11_2": "1", "HAS_DX11_3": "1", "HAS_DXGI1_4": "1", "HAS_DXGI1_5": "1", "NDEBUG": "1", "XR_D3D12": "1"}
    pipelines = {}
    for p in L.collect_cpp(tree, r4, unresolved) + L.collect_lua(tree, unresolved) + L.EXTRA_PIPELINES:
        k = p.key()
        if k in pipelines:
            for o in p.origins:
                if o not in pipelines[k].origins:
                    pipelines[k].origins.append(o)
            if not (pipelines[k].gate and p.gate):
                pipelines[k].gate = None
        else:
            pipelines[k] = p
    mesh_ok = L.sm_tuple(args.sm) >= L.MESH_MIN_SM
    selected = [p for p in pipelines.values()
                if ("ps" in p.stages or "ms" in p.stages) and (mesh_ok or "ms" not in p.stages) and
                (not args.filter or any(args.filter.lower() in n.lower() for n in p.stages.values()))]
    apis = [a for a in L.APIS if a[0] == "dx12"]

    auditor = Auditor(tree, args)
    jobs = []
    for p in selected:
        for label, perm, defines in L.variants(p, tree, apis, L.PERMS):
            jobs.append(("pipe", p, label, perm, defines))
    # Every shader file of the tree too, the pipelines above or not (compute shaders, unreferenced
    # files, mod overrides): every permutation, bare and +game. Native and ray tracing passes compile
    # through da_dxc_compile_native (HLSL 2021, own includes), not the engine path.
    referenced = {(s, n.split("(")[0].lower()) for p in selected for s, n in p.stages.items()}
    loose = []
    for ext in EXTS:
        for f in tree.names("." + ext):
            name = f[: -len(ext) - 1]
            if name.startswith(("rt/", "native/")):
                continue
            # Below SM 6.5 the engine has no mesh path at all.
            if ext in L.DXC_ONLY_STAGES and L.sm_tuple(args.sm) < L.MESH_MIN_SM:
                continue
            if args.filter and args.filter.lower() not in name:
                continue
            if (ext, name) in referenced:
                continue
            loose.append((ext, name))
    for ext, name in loose:
        for label, perm, defines in L.define_sets(apis, L.PERMS):
            jobs.append(("file", (ext, name), label, perm, defines))
    # Console-only switches of r4_shaders.cpp the preset ladder never sets, one at a time on top of
    # Extreme+game.
    if args.console_extras:
        base = dict(L.PERMS)["Extreme"]
        extreme = tuple(f"{k}={v}" for k, v in L.ALWAYS) + base + L.GAME_COMMON + L.GAME["Extreme"] + ("SMAP_ARRAY=1",)
        for extra in CONSOLE_EXTRAS:
            defs = tuple(d for d in extreme if d.split("=")[0] not in {e.split("=")[0] for e in extra}) + extra
            for p in selected:
                skins = L.SKINS if "vs" in p.stages and L.uses_skinning(tree, p.stages["vs"]) else L.SKINS[:1]
                for skin in skins:
                    jobs.append(("pipe", p, "extra:" + "+".join(extra) + (f":{skin}" if skin != "SKIN_NONE" else ""), "Extreme",
                                 defs + (f"{skin}=1",) + tuple(f"{e}=1" for e in p.extras)))

    print(f"{len(selected)} pipelines, {len(loose)} other shader files, {len(jobs)} jobs; DXC {args.dxc_dir}, SM {args.sm}, "
          f"HLSL {args.hv}, preprocessor {args.preprocess.upper()}, args {' '.join(auditor.args)}, "
          f"macros {' '.join(f'{k}={v}' for k, v in auditor.xfr)}", flush=True)
    t0 = time.perf_counter()
    pipe_results = {}

    def run(job):
        kind, what, label, _perm, defines = job
        if kind == "pipe":
            built = {}
            for s in [s for s in L.STAGES if s in what.stages]:
                b = auditor.build(s, what.stages[s], defines)
                if b is None and not what.dynamic:
                    b = auditor.build(s, "stub_default", defines)
                built[s] = b
            return kind, what, label, built
        ext, name = what
        return kind, what, label, {ext: auditor.build(ext, name, defines)}

    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        for kind, what, label, built in pool.map(run, jobs):
            pipe_results.setdefault((kind, what if kind == "file" else what.key()), (what, []))[1].append((label, built))
    wall = time.perf_counter() - t0

    uniq = [v for v in auditor.by_text.values() if isinstance(v, Result)]
    dxc_fail = [r for r in uniq if not r.ok]
    fxc_fail = [r for r in uniq if not r.fxc_ok]
    both_fail = [r for r in uniq if not r.ok and not r.fxc_ok]
    dxc_only_fail = [r for r in uniq if not r.ok and r.fxc_ok]
    print(f"\n{len(uniq)} unique shader compilations in {wall:.1f} s wall")
    print(f"  compile time, summed over threads: FXC {sum(r.fxc_ms for r in uniq) / 1000:.1f} s, DXC {sum(r.dxc_ms for r in uniq) / 1000:.1f} s")
    heavy = sorted(uniq, key=lambda r: -r.fxc_ms)[:12]
    print("  slowest under FXC (ms FXC / DXC): " + "; ".join(f"{r.name} {r.fxc_ms:.0f}/{r.dxc_ms:.0f}" for r in heavy))
    sz_x = sum(r.size_dxbc for r in uniq if r.ok and r.fxc_ok)
    sz_d = sum(r.size_dxil for r in uniq if r.ok and r.fxc_ok)
    print(f"  bytecode: DXBC {sz_x / 1024:.0f} KB, DXIL (stripped) {sz_d / 1024:.0f} KB")
    print(f"  FXC failures {len(fxc_fail)}, DXC failures {len(dxc_fail)} (both {len(both_fail)}, DXC only {len(dxc_only_fail)})")
    if args.compare_raw:
        cmp = [r for r in uniq if r.same_as_raw is not None]
        print(f"  DXIL from the FXC-preprocessed text equals DXIL from the raw file: {sum(1 for r in cmp if r.same_as_raw)} of {len(cmp)}")

    def group(items, keyfn):
        out = {}
        for r in items:
            out.setdefault(keyfn(r), []).append(r)
        return out

    if dxc_only_fail:
        print("\n== DXC refuses, FXC compiles (by error)")
        for err, rs in sorted(group(dxc_only_fail, lambda r: re.sub(r"^.*?:\d+:\d+: ", "", r.error)).items(), key=lambda kv: -len(kv[1])):
            names = sorted({r.name for r in rs})
            print(f"  [{len(rs)}] {err}\n      in {', '.join(names[:12])}{' ...' if len(names) > 12 else ''}")
            print(f"      e.g. {rs[0].error}")
    if both_fail:
        print("\n== Neither compiles (permutations the game does not build)")
        for n, rs in sorted(group(both_fail, lambda r: r.name).items()):
            print(f"  {n}: FXC {rs[0].fxc_error} | DXC {rs[0].error}")
    dx9 = [r for r in uniq if r.dx9]
    if dx9:
        print("\n== Needed FXC backwards compatibility (X3523; DXC has no equivalent): " + ", ".join(sorted({r.name for r in dx9})))
    if auditor.check2021:
        h21 = [r for r in uniq if r.ok and r.hv2021]
        print(f"\n== HLSL 2021 would refuse {len(h21)} of {len([r for r in uniq if r.ok])} (by error)")
        for err, rs in sorted(group(h21, lambda r: re.sub(r"^.*?:\d+:\d+: ", "", r.hv2021)).items(), key=lambda kv: -len(kv[1]))[:25]:
            names = sorted({r.name for r in rs})
            print(f"  [{len(rs)}] {err}\n      in {', '.join(names[:10])}{' ...' if len(names) > 10 else ''}")
    probs = [(r, p) for r in uniq for p in r.problems if p[0] != "fxc-too"]
    if probs:
        print("\n== Reflection the engine or the layer would mishandle, new with DXC")
        for (kind, text), rs in sorted(group(probs, lambda rp: rp[1]).items(), key=lambda kv: kv[0]):
            print(f"  {kind}: {text}  ({', '.join(sorted({r.name for r, _ in rs})[:8])})")
    fxc_too = [(r, p) for r in uniq for p in r.problems if p[0] == "fxc-too"]
    if fxc_too:
        print("\n== The same mishandling exists with FXC today")
        for text, rs in sorted(group(fxc_too, lambda rp: rp[1][1]).items()):
            print(f"  {text}  ({', '.join(sorted({r.name for r, _ in rs})[:8])})")
    diffs = [(r, d) for r in uniq for d in r.diffs]
    if diffs:
        print("\n== Reflection differences FXC vs DXC (by kind)")
        kinds = group(diffs, lambda rd: re.sub(r"\d+", "N", rd[1]))
        for _k, rs in sorted(kinds.items(), key=lambda kv: -len(kv[1]))[:40]:
            print(f"  [{len(rs)}] {rs[0][1]}  ({', '.join(sorted({r.name for r, _ in rs})[:6])})")
    agg = {}
    for r in uniq:
        for part in filter(None, r.warnings.split(", ")):
            k, _, n = part.rpartition(" x")
            agg[k] = agg.get(k, 0) + int(n)
    if agg:
        print("\n== DXC warnings (summed): " + "; ".join(f"{k} x{v}" for k, v in sorted(agg.items(), key=lambda kv: -kv[1])[:15]))
    if auditor.dxc.handler.missing:
        print("\n== Includes the handler could not resolve: " + ", ".join(sorted(auditor.dxc.handler.missing)[:20]))
    if auditor.dxc.handler.nonutf8:
        print("\n== Included files with non-UTF-8 bytes (sanitized): " + ", ".join(sorted(auditor.dxc.handler.nonutf8)))

    # Linkage of every pipeline with the DXIL signatures (a mesh shader links over its vertex and
    # primitive outputs).
    broken, gated = {}, {}
    for (kind, _key), (what, per) in pipe_results.items():
        if kind != "pipe":
            continue
        for _label, built in per:
            order = [s for s in L.STAGES if s in built]
            if any(built[s] is None or not built[s].ok or built[s].sig is None for s in order):
                continue
            for a, b in zip(order, order[1:]):
                if a == "as":
                    continue
                outputs = built[a].sig.outputs + (built[a].sig.patch if a == "ms" else [])
                for issue in L.link(a, outputs, b, built[b].sig.inputs):
                    if issue[0] == "error":
                        (gated if what.gate else broken).setdefault(what.title(), set()).add((f"{a.upper()}->{b.upper()}",) + issue[1:])
    print(f"\n== Pipelines whose DXIL stages do not link: {len(broken)}")
    for title, issues in sorted(broken.items())[:40]:
        print(f"  {title}")
        for tag, code, elem, msg in sorted(issues)[:6]:
            print(f"      {tag} #{code} {elem} {msg}")
    if gated:
        print(f"== Not linking either, built only behind a legacy switch (not counted): {len(gated)}")
        for title in sorted(gated):
            print(f"  {title}")

    if args.out:
        args.out.mkdir(parents=True, exist_ok=True)
        blobs = args.out / "blobs"
        blobs.mkdir(exist_ok=True)
        names = {}
        for r in uniq:
            if r.ok and r.obj:
                h = hashlib.sha1(r.obj).hexdigest()[:16]
                (blobs / f"{h}.{r.stage}").write_bytes(r.obj)
                names[id(r)] = f"{h}.{r.stage}"
        with open(args.out / "pipelines.txt", "w", encoding="utf-8") as fh:
            seen = set()
            for (kind, _key), (what, per) in pipe_results.items():
                if kind != "pipe":
                    continue
                for label, built in per:
                    order = [s for s in L.STAGES if s in built]
                    if any(built[s] is None or not built[s].ok for s in order):
                        continue
                    line = " ".join(f"{s}={names[id(built[s])]}" for s in order)
                    if line in seen:
                        continue
                    seen.add(line)
                    fh.write(f"{line} # {what.title()} [{label}]{' (gated)' if what.gate else ''}\n")
            # Compute shaders stand alone: one pipeline each.
            for r in uniq:
                if r.ok and r.stage == "cs" and id(r) in names:
                    fh.write(f"cs={names[id(r)]} # {r.name}\n")
    if args.json:
        args.json.write_text(json.dumps([{"name": r.name, "ok": r.ok, "error": r.error, "fxc_ok": r.fxc_ok, "dxc_ms": r.dxc_ms,
                                          "fxc_ms": r.fxc_ms, "problems": r.problems, "diffs": r.diffs[:20]} for r in uniq], indent=1))
    failed = bool(dxc_only_fail or probs or broken)
    print(f"\n{'FAIL' if failed else 'PASS'}: {len(dxc_only_fail)} DXC-only failure(s), {len(probs)} new reflection problem(s), "
          f"{len(broken)} pipeline(s) not linking outside the legacy-switch set")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
