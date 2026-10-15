"""Check that every vertex -> pixel stage pair the renderer builds still links under DirectX 12.

D3D11 linked the stages by register and let a mismatched signature through with, at most, a debug
layer complaint. D3D12 validates the whole pipeline when it is created and refuses one whose
signatures disagree; the DirectX 12 module (xrRender_R5) then skips every draw that uses it, so a
signature that D3D11 tolerated becomes an effect that is simply missing. The debug layer names the
three ways it happens:

    #666  the later stage reads a semantic the earlier one never writes
    #660  both have it, but in different registers
    #662  the reader's component mask is not a subset of the writer's

This finds them without the game. It assembles the shader tree the way tools\\qa\\Test-Shaders.ps1
does (the unpacked archive layers, then the loose overlay on top) and collects every stage
combination the R4/R5 source can ask for: string literals in the C++ blenders the R4 sources.cmake
compiles (r_Pass, r_TessPass, PassSET_Shaders, with the preprocessor branches for R4 resolved), the
Lua shader scripts (:begin), the names uber_deffer / uber_shadow put together at run time
(mirrored, tessellation included), and the few variables pinned down in DYNAMIC_PASSES. A name with
no file is linked with stub_default in its place, as CreateShader does. Each shader is compiled with
the d3dcompiler_47 next to the SDK's fxc and the engine's flags, in every Test-Shaders.ps1
permutation (plus the console-only MSAA alpha-test modes), bare and with the defines the game adds
("+game"), for DirectX 11 and again with the DirectX 12 set (SMAP_ARRAY), a model vertex shader in
every skinning mode; the signatures are read straight out of the bytecode.

    python tools\\qa\\Check-ShaderLinkage.py                   # everything, about two minutes
    python tools\\qa\\Check-ShaderLinkage.py --filter sunshafts
    python tools\\qa\\Check-ShaderLinkage.py --list            # just the pipelines and where they come from
    python tools\\qa\\Check-ShaderLinkage.py --dump da_taa.ps  # one shader's signatures, per permutation
    python tools\\qa\\Check-ShaderLinkage.py --verbose         # also the combinations with a missing file
    python tools\\qa\\Check-ShaderLinkage.py --compiler dxc    # the DirectX 12 module's DXC mode (dxc.md 3.3)

--compiler dxc links what xrRender_R5 builds when its engine shaders are DXIL: FXC's preprocessor
with the engine's includer, then DXC (Externals/dxc) with the engine's arguments and GRIZZLY_* macros,
Shader Model --sm. A stage DXC refuses falls back to FXC and its whole pass is rebuilt as DXBC twins
(a D3D12 pipeline never mixes DXIL and DXBC, dxc.md 3.6); such pipelines are listed, and linked as
DXBC. Mesh pipelines (a "#ms" vertex slot, an optional "#as" geometry slot) exist only in DXC mode;
one whose stages are not all DXIL is unavailable (the engine takes the vertex path). A mesh shader
links to the pixel shader by semantic, over its vertex and primitive outputs.

A stage name may carry engine options, "name(OPT_A,OPT_B,)": they become defines, as when the
blender adds them with addShaderOption. Pipelines the source does not spell out (lane passes built
in code) are listed in the [dx12:<lane>] blocks of EXTRA_PIPELINES.

Exit code 1 when a pipeline does not link. Warnings (#661 component type, #664 a read of components
the earlier stage never writes), pipelines built only behind a legacy switch (GATES) and shaders
that fail to compile are reported without failing the run.
"""
import argparse
import concurrent.futures
import ctypes
import fnmatch
import hashlib
import os
import pathlib
import re
import struct
import sys
import threading
from ctypes import POINTER, byref, c_char_p, c_int, c_long, c_size_t, c_uint, c_ulong, c_void_p

REPO = pathlib.Path(__file__).resolve().parents[2]
DEFAULT_UNPACK = pathlib.Path(r"D:\Games\Dead Air\_work\da_unpack")
DEFAULT_LOOSE = REPO / "packaging" / "dead-air-x64" / "compatibility" / "gamedata" / "shaders" / "r3"
ARCHIVE_LAYERS = ("configs", "xtra", "ours")

# Test-Shaders.ps1, both lists. The engine sets the first three for every shader regardless of the
# quality tier; the permutations are the ones the preset ladder produces.
ALWAYS = (("SM_5", "1"), ("SMAP_size", "2048"), ("USE_HWSMAP", "1"))
PERMS = (
    ("Extreme", ("USE_SOFT_WATER=1", "USE_REFLECTIONS=1", "SSR_QUALITY=4", "SSR_JITTER=1", "GBUFFER_OPTIMIZATION=1", "MSAA_SAMPLES=1")),
    ("High", ("USE_SOFT_WATER=1", "USE_REFLECTIONS=1", "SSR_QUALITY=3", "GBUFFER_OPTIMIZATION=1", "MSAA_SAMPLES=1")),
    ("Default", ("USE_SOFT_WATER=1", "USE_REFLECTIONS=1", "SSR_QUALITY=2", "GBUFFER_OPTIMIZATION=1", "MSAA_SAMPLES=1")),
    ("Low", ("USE_REFLECTIONS=1", "SSR_QUALITY=1", "GBUFFER_OPTIMIZATION=1", "MSAA_SAMPLES=1")),
    ("Minimum", ("MSAA_SAMPLES=1",)),
    ("NoGbufOpt", ("USE_SOFT_WATER=1", "USE_REFLECTIONS=1", "SSR_QUALITY=3", "MSAA_SAMPLES=1")),
    ("MSAA4", ("USE_SOFT_WATER=1", "USE_REFLECTIONS=1", "SSR_QUALITY=3", "GBUFFER_OPTIMIZATION=1", "USE_MSAA=1", "MSAA_SAMPLES=4")),
    # Not in Test-Shaders.ps1: the console-only MSAA alpha-test modes (r3_msaa_alphatest, r3_msaa_opt;
    # r2.cpp picks one of three), which move SV_Position and SV_Coverage around in the pixel inputs.
    ("MSAA4-atoc", ("USE_SOFT_WATER=1", "USE_REFLECTIONS=1", "SSR_QUALITY=3", "GBUFFER_OPTIMIZATION=1", "USE_MSAA=1", "MSAA_SAMPLES=4",
                    "MSAA_ALPHATEST_DX10_0_ATOC=1")),
    ("MSAA4-opt-atoc", ("USE_SOFT_WATER=1", "USE_REFLECTIONS=1", "SSR_QUALITY=3", "GBUFFER_OPTIMIZATION=1", "USE_MSAA=1", "MSAA_SAMPLES=4",
                        "MSAA_OPTIMIZATION=1", "MSAA_ALPHATEST_DX10_1_ATOC=1")),
    ("MSAA4-opt-native", ("USE_SOFT_WATER=1", "USE_REFLECTIONS=1", "SSR_QUALITY=3", "GBUFFER_OPTIMIZATION=1", "USE_MSAA=1", "MSAA_SAMPLES=4",
                          "MSAA_OPTIMIZATION=1", "MSAA_ALPHATEST_DX10_1=1")),
)
# r4_shaders.cpp: the DirectX 12 module adds SMAP_ARRAY to every shader it compiles (#ifdef XR_D3D12).
APIS = (("dx11", ()), ("dx12", ("SMAP_ARRAY=1",)))
SKINS = ("SKIN_NONE", "SKIN_0", "SKIN_1", "SKIN_2", "SKIN_3", "SKIN_4")

# The rest of what r4_shaders.cpp defines in the game, where Test-Shaders.ps1 leaves it out: the R4
# constants (advanced post-process on, hardware shadow maps with PCF) and what each preset of the
# ladder switches on (rspec_*.ltx plus the *_by_preset tables in xrRender_console.cpp). Some of these
# change signatures - USE_SOFT_PARTICLES adds an input to the particle pixel shaders - so every
# permutation is also checked with them, labelled "+game".
GAME_COMMON = ("FP16_FILTER=1", "FP16_BLEND=1", "USE_HWSMAP_PCF=1", "USE_BRANCHING=1", "USE_VTF=1",
               "USE_LENS_DIRT=1", "USE_DOF=1")
GAME_HIGH = ("USE_SOFT_PARTICLES=1", "ALLOW_STEEPPARALLAX=1", "SSAO_QUALITY=2", "USE_GTAO=1",
             "SUN_SHAFTS_QUALITY=2", "SUN_QUALITY=3", "USE_SSHAFTS=1", "USE_LENS_WATER=1")
GAME = {
    "Extreme": ("USE_SOFT_PARTICLES=1", "ALLOW_STEEPPARALLAX=1", "SSAO_QUALITY=3", "USE_GTAO=1",
                "SUN_SHAFTS_QUALITY=3", "SUN_QUALITY=4", "USE_SSHAFTS=1", "USE_LENS_WATER=1"),
    "High": GAME_HIGH,
    "Default": ("USE_SOFT_PARTICLES=1", "ALLOW_STEEPPARALLAX=1", "SSAO_QUALITY=1", "SUN_SHAFTS_QUALITY=1",
                "SUN_QUALITY=2", "USE_SSHAFTS=1", "USE_LENS_WATER=1"),
    "Low": ("SUN_QUALITY=1",),
    "Minimum": (),
    "NoGbufOpt": GAME_HIGH,
    # No GTAO under MSAA (r4_shaders.cpp).
    "MSAA4": tuple(d for d in GAME_HIGH if d != "USE_GTAO=1"),
}
for _perm in ("MSAA4-atoc", "MSAA4-opt-atoc", "MSAA4-opt-native"):
    GAME[_perm] = GAME["MSAA4"]

# Compile failures that are not linkage problems and that Test-Shaders.ps1 already explains
# (permutation names match as shell patterns).
KNOWN_UNBUILDABLE = {("gather.ps", "*"), ("gtao_render.ps", "MSAA4*")}
# DirectX 12 feature lanes add theirs between their markers: KNOWN_UNBUILDABLE |= {("file.ps", "MSAA4*")}.
# [dx12:async] begin known
# [dx12:async] end known
# [dx12:vrs] begin known
# [dx12:vrs] end known
# [dx12:gd] begin known
# [dx12:gd] end known
# [dx12:mesh] begin known
# [dx12:mesh] end known
# [dx12:rt] begin known
# [dx12:rt] end known

# Pipeline order. "as"/"ms" (amplification, mesh) are DXC-only stages of the DirectX 12 module.
STAGES = ("as", "ms", "vs", "hs", "ds", "gs", "ps")
TARGETS = {"vs": "vs_5_0", "hs": "hs_5_0", "ds": "ds_5_0", "gs": "gs_5_0", "ps": "ps_5_0", "cs": "cs_5_0"}
DXC_ONLY_STAGES = ("as", "ms")

# ShaderResourceTraits.h: release builds compile with these.
D3DCOMPILE_PACK_MATRIX_ROW_MAJOR = 1 << 3
D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY = 1 << 12
D3DCOMPILE_OPTIMIZATION_LEVEL3 = 1 << 15
BASE_FLAGS = D3DCOMPILE_PACK_MATRIX_ROW_MAJOR | D3DCOMPILE_OPTIMIZATION_LEVEL3

SV_NAMES = {0: "", 1: "SV_Position", 2: "SV_ClipDistance", 3: "SV_CullDistance", 4: "SV_RenderTargetArrayIndex",
            5: "SV_ViewportArrayIndex", 6: "SV_VertexID", 7: "SV_PrimitiveID", 8: "SV_InstanceID",
            9: "SV_IsFrontFace", 10: "SV_SampleIndex", 64: "SV_Target", 65: "SV_Depth", 66: "SV_Coverage"}
COMPONENT_TYPES = {0: "unknown", 1: "uint", 2: "int", 3: "float"}


# ---------------------------------------------------------------------------------------------
# Shader tree


class Tree:
    """The r3 folder as the game sees it: archive layers in order, the loose overlay last."""

    def __init__(self, unpack, loose):
        self.files = {}
        for layer in ARCHIVE_LAYERS:
            self._add(unpack / layer / "shaders" / "r3", layer)
        self._add(loose, "loose")

    def _add(self, root, tag):
        if not root.is_dir():
            return
        for path in root.rglob("*"):
            if path.is_file():
                self.files[path.relative_to(root).as_posix().lower()] = (path, tag)

    def lookup(self, name):
        return self.files.get(name.replace("\\", "/").lower())

    def names(self, suffix):
        return sorted(k for k in self.files if k.endswith(suffix))


# ---------------------------------------------------------------------------------------------
# d3dcompiler_47 from the SDK folder fxc lives in, driven in process: the bytecode never touches the
# disk (Test-Shaders.ps1 avoids /Fo for the same reason), and the include handler resolves names
# the way r4_shaders.cpp's includer does - always from the r3 root.


def find_sdk_bin():
    base = pathlib.Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Windows Kits" / "10" / "bin"
    found = [p for p in base.rglob("fxc.exe") if p.parent.name.lower() == "x64"]
    if not found:
        sys.exit("fxc.exe not found - install the Windows SDK")
    # Test-Shaders.ps1: Sort-Object FullName -Descending | Select-Object -First 1.
    found.sort(key=lambda p: str(p).lower(), reverse=True)
    return found[0].parent


class ShaderMacro(ctypes.Structure):
    _fields_ = [("Name", c_char_p), ("Definition", c_char_p)]


OPEN_FN = ctypes.WINFUNCTYPE(c_long, c_void_p, c_int, c_char_p, c_void_p, POINTER(c_void_p), POINTER(c_uint))
CLOSE_FN = ctypes.WINFUNCTYPE(c_long, c_void_p, c_void_p)


class IncludeVtbl(ctypes.Structure):
    _fields_ = [("Open", OPEN_FN), ("Close", CLOSE_FN)]


class IncludeObj(ctypes.Structure):
    _fields_ = [("lpVtbl", POINTER(IncludeVtbl))]


def blob_bytes(blob):
    if not blob:
        return b""
    vtbl = ctypes.cast(blob, POINTER(POINTER(c_void_p)))[0]
    get_ptr = ctypes.WINFUNCTYPE(c_void_p, c_void_p)(vtbl[3])
    get_size = ctypes.WINFUNCTYPE(c_size_t, c_void_p)(vtbl[4])
    release = ctypes.WINFUNCTYPE(c_ulong, c_void_p)(vtbl[2])
    data = ctypes.string_at(get_ptr(blob), get_size(blob))
    release(blob)
    return data


class Compiler:
    def __init__(self, sdk_bin, tree):
        self.tree = tree
        self.dll = ctypes.WinDLL(str(sdk_bin / "d3dcompiler_47.dll"))
        self.dll.D3DCompile.restype = c_long
        self.dll.D3DCompile.argtypes = [c_void_p, c_size_t, c_char_p, c_void_p, c_void_p, c_char_p, c_char_p,
                                        c_uint, c_uint, POINTER(c_void_p), POINTER(c_void_p)]
        self.dll.D3DPreprocess.restype = c_long
        self.dll.D3DPreprocess.argtypes = [c_void_p, c_size_t, c_char_p, c_void_p, c_void_p,
                                           POINTER(c_void_p), POINTER(c_void_p)]
        self._buffers = {}
        self._lock = threading.Lock()
        self._open = OPEN_FN(self._on_open)
        self._close = CLOSE_FN(self._on_close)
        self._vtbl = IncludeVtbl(self._open, self._close)
        self._include = IncludeObj(ctypes.pointer(self._vtbl))
        self.missing_includes = set()

    def _on_open(self, _this, _type, name, _parent, pp_data, p_bytes):
        try:
            text = name.decode("latin-1")
            hit = self.tree.lookup(text)
            if not hit and re.match(r"(?i)r3[\\/]", text):
                hit = self.tree.lookup(text[3:])
            if not hit:
                self.missing_includes.add(text)
                return -2147467259  # E_FAIL
            data = hit[0].read_bytes()
            buf = ctypes.create_string_buffer(data, len(data) + 1)
            with self._lock:
                self._buffers[ctypes.addressof(buf)] = buf
            pp_data[0] = ctypes.addressof(buf)
            p_bytes[0] = len(data)
            return 0
        except Exception:  # noqa: BLE001 - a callback must not raise into the compiler
            return -2147467259

    def _on_close(self, _this, data):
        with self._lock:
            self._buffers.pop(data, None)
        return 0

    @staticmethod
    def _macros(defines):
        arr = (ShaderMacro * (len(defines) + 1))()
        for i, (k, v) in enumerate(defines):
            arr[i].Name = k.encode()
            arr[i].Definition = v.encode()
        return arr

    def preprocess(self, source, defines, name=b""):
        macros = self._macros(defines)
        text, errors = c_void_p(), c_void_p()
        hr = self.dll.D3DPreprocess(source, len(source), name, ctypes.cast(macros, c_void_p),
                                    ctypes.addressof(self._include), byref(text), byref(errors))
        out, err = blob_bytes(text), blob_bytes(errors).decode("latin-1", "replace")
        return (out if hr >= 0 else None), err

    def compile(self, source, entry, target, flags):
        code, errors = c_void_p(), c_void_p()
        hr = self.dll.D3DCompile(source, len(source), b"", None, None, entry.encode(), target.encode(),
                                 flags, 0, byref(code), byref(errors))
        out, err = blob_bytes(code), blob_bytes(errors).decode("latin-1", "replace")
        return (out if hr >= 0 else None), err


# ---------------------------------------------------------------------------------------------
# DXC (dxcompiler.dll + dxil.dll from Externals/dxc) driven in process, the way the DirectX 12
# module compiles an engine shader (dxc.md 3.3): FXC's preprocessor with the engine's includer
# first, then DXC on the self-contained text with the engine's argument list.

DEFAULT_DXC_DIR = REPO / "Externals" / "dxc" / "bin" / "x64"
# dxc.md 3.3 step 4, release build; -HV and -T/-E are added per shader.
DXC_ENGINE_ARGS = ("-Zpr", "-O3", "-flegacy-resource-reservation", "-Qstrip_debug", "-Qstrip_reflect",
                   "-Wno-conversion", "-Wno-for-redefinition", "-Wno-effects-syntax")
# New-only extensions are written for HLSL 2021 (PLAN C5); the legacy base stays on 2018.
HLSL_2021_STAGES = ("ms", "as")
# Mesh and amplification shaders need Shader Model 6.5.
MESH_MIN_SM = (6, 5)


def sm_tuple(sm):
    return tuple(int(x) for x in sm.split("_"))


def dxc_target(stage, sm):
    return f"{stage}_{sm}"


def dxc_macros(sm, wave_lanes, min16):
    """The macros the engine adds for DXC only (dxc.md 3.3 step 5); FXC never sees them."""
    out = [("GRIZZLY_DXC", "1"), ("GRIZZLY_SM", sm.replace("_", ""))]
    if wave_lanes:
        out.append(("GRIZZLY_WAVE_LANES", str(wave_lanes)))
    if min16:
        out.append(("GRIZZLY_MIN16", "1"))
    return out


def sanitize(data):
    """What the engine hands DXC: UTF-8 as is; anything else (cp1251 comments) with the high bytes
    replaced, so the text never depends on the machine's code page (dxc.md 3.3 step 2)."""
    try:
        data.decode("utf-8")
        return data
    except UnicodeDecodeError:
        return bytes(b if b < 0x80 else 0x3F for b in data)


class GUID(ctypes.Structure):
    _fields_ = [("Data1", c_uint), ("Data2", ctypes.c_ushort), ("Data3", ctypes.c_ushort), ("Data4", ctypes.c_ubyte * 8)]

    @staticmethod
    def of(text):
        import uuid
        u = uuid.UUID(text)
        g = GUID()
        g.Data1, g.Data2, g.Data3 = u.fields[0], u.fields[1], u.fields[2]
        for i, b in enumerate(u.bytes[8:]):
            g.Data4[i] = b
        return g


CLSID_DxcCompiler = GUID.of("73e22d93-e6ce-47f3-b5bf-f0664f39c1b0")
CLSID_DxcUtils = GUID.of("6245d6af-66e0-48fd-80b4-4d271796748c")
IID_IDxcCompiler3 = GUID.of("228B4687-5A6A-4730-900C-9702B2203F54")
IID_IDxcUtils = GUID.of("4605C4CB-2019-492A-ADA4-65F20BB7D67F")
IID_IDxcResult = GUID.of("58346CDA-DDE7-4497-9461-6F87AF5E0659")
IID_IDxcBlob = GUID.of("8BA5FB08-5195-40e2-AC58-0D989C3A0102")
IID_IDxcBlobUtf8 = GUID.of("3DA636C9-BA71-4024-A301-30CBF125305B")
DXC_OUT_OBJECT, DXC_OUT_ERRORS, DXC_OUT_REFLECTION = 1, 2, 8
DXC_CP_UTF8 = 65001


def vcall(obj, index, restype, argtypes, *args):
    """Call a COM method by vtable index."""
    vtbl = ctypes.cast(obj, POINTER(POINTER(c_void_p)))[0]
    fn = ctypes.WINFUNCTYPE(restype, c_void_p, *argtypes)(vtbl[index])
    return fn(obj, *args)


def com_release(obj):
    if obj:
        vcall(obj, 2, c_ulong, [])


def dxc_blob_data(blob):
    ptr = vcall(blob, 3, c_void_p, [])
    size = vcall(blob, 4, c_size_t, [])
    return ctypes.string_at(ptr, size) if size else b""


class DxcBuffer(ctypes.Structure):
    _fields_ = [("Ptr", c_void_p), ("Size", c_size_t), ("Encoding", c_uint)]


class Dxc:
    """One IDxcCompiler3/IDxcUtils per thread, as the engine keeps them (dxc.md 3.1)."""

    def __init__(self, dxc_dir=DEFAULT_DXC_DIR):
        dxc_dir = pathlib.Path(dxc_dir)
        # dxil.dll first: dxcompiler.dll signs with the one already loaded.
        self.dll_dxil = ctypes.WinDLL(str(dxc_dir / "dxil.dll"))
        self.dll = ctypes.WinDLL(str(dxc_dir / "dxcompiler.dll"))
        self.dir = dxc_dir
        self.create = self.dll.DxcCreateInstance
        self.create.restype = c_long
        self.create.argtypes = [POINTER(GUID), POINTER(GUID), POINTER(c_void_p)]
        self.local = threading.local()

    def utils(self):
        if not hasattr(self.local, "utils"):
            u = c_void_p()
            self.create(byref(CLSID_DxcUtils), byref(IID_IDxcUtils), byref(u))
            self.local.utils = u.value
        return self.local.utils

    def compiler(self):
        if not hasattr(self.local, "compiler"):
            c = c_void_p()
            self.create(byref(CLSID_DxcCompiler), byref(IID_IDxcCompiler3), byref(c))
            self.local.compiler = c.value
        return self.local.compiler

    def compile(self, source, file_name, entry, target, hv, args=DXC_ENGINE_ARGS, defines=()):
        """Self-contained source (no include handler) -> (object or None, reflection or None, messages)."""
        argv_list = [file_name, "-E", entry, "-T", target, "-HV", hv] + list(args)
        for k, v in defines:
            argv_list += ["-D", f"{k}={v}"]
        argv = (ctypes.c_wchar_p * len(argv_list))(*argv_list)
        src = sanitize(source)
        buf = DxcBuffer(ctypes.cast(ctypes.c_char_p(src), c_void_p), len(src), DXC_CP_UTF8)
        result = c_void_p()
        hr = vcall(self.compiler(), 3, c_long,
                   [POINTER(DxcBuffer), POINTER(ctypes.c_wchar_p), c_uint, c_void_p, POINTER(GUID), POINTER(c_void_p)],
                   byref(buf), argv, len(argv_list), None, byref(IID_IDxcResult), byref(result))
        if hr < 0 or not result.value:
            return None, None, f"Compile hr 0x{hr & 0xffffffff:08x}"
        res = result.value
        status = c_long()
        vcall(res, 3, c_long, [POINTER(c_long)], byref(status))
        get = [c_uint, POINTER(GUID), POINTER(c_void_p), POINTER(c_void_p)]
        errors = c_void_p()
        vcall(res, 7, c_long, get, DXC_OUT_ERRORS, byref(IID_IDxcBlobUtf8), byref(errors), None)
        msg = ""
        if errors.value:
            msg = dxc_blob_data(errors.value).rstrip(b"\0").decode("utf-8", "replace")
            com_release(errors.value)
        obj = refl = None
        if status.value >= 0:
            o = c_void_p()
            vcall(res, 7, c_long, get, DXC_OUT_OBJECT, byref(IID_IDxcBlob), byref(o), None)
            if o.value:
                obj = dxc_blob_data(o.value)
                com_release(o.value)
            r = c_void_p()
            vcall(res, 7, c_long, get, DXC_OUT_REFLECTION, byref(IID_IDxcBlob), byref(r), None)
            if r.value:
                refl = dxc_blob_data(r.value)
                com_release(r.value)
        com_release(res)
        return obj, refl, msg


def first_dxc_error(msg):
    for line in msg.splitlines():
        if "error:" in line:
            # FXC's #line markers carry the working directory in front of the shader name.
            return re.sub(r"^[A-Za-z]:[\\/](?:[^:]*[\\/])?", "", line.strip())
    return (msg.strip().splitlines() or ["compilation failed"])[0]


# ---------------------------------------------------------------------------------------------
# DXBC container and its signature chunks (DXIL containers use the same header)


class Element:
    __slots__ = ("name", "index", "sysval", "ctype", "reg", "mask", "rw", "stream")

    def __init__(self, name, index, sysval, ctype, reg, mask, rw, stream):
        self.name, self.index, self.sysval, self.ctype = name, index, sysval, ctype
        self.reg, self.mask, self.rw, self.stream = reg, mask, rw, stream

    def key(self):
        return self.name.upper(), self.index

    def label(self):
        return f"{self.name}{self.index}"


def mask_str(mask):
    return "".join(c for i, c in enumerate("xyzw") if mask & (1 << i)) or "-"


def parse_dxbc(data):
    if data[:4] != b"DXBC":
        raise ValueError("not a DXBC container")
    count = struct.unpack_from("<I", data, 28)[0]
    chunks = {}
    for off in struct.unpack_from(f"<{count}I", data, 32):
        size = struct.unpack_from("<I", data, off + 4)[0]
        chunks[data[off:off + 4].decode("ascii", "replace")] = data[off + 8:off + 8 + size]
    return chunks


def parse_signature(chunk, fourcc):
    count = struct.unpack_from("<I", chunk, 0)[0]
    size, lead = {"OSG5": (28, 4), "ISG1": (32, 4), "OSG1": (32, 4), "PSG1": (32, 4)}.get(fourcc, (24, 0))
    out = []
    for i in range(count):
        base = 8 + i * size
        stream = struct.unpack_from("<I", chunk, base)[0] if lead else 0
        name_off, index, sysval, ctype, reg = struct.unpack_from("<5I", chunk, base + lead)
        mask, rw = chunk[base + lead + 20], chunk[base + lead + 21]
        end = chunk.index(b"\0", name_off)
        out.append(Element(chunk[name_off:end].decode("latin-1"), index, sysval, ctype, reg, mask, rw, stream))
    return out


class Signatures:
    def __init__(self, bytecode):
        chunks = parse_dxbc(bytecode)

        def pick(*names):
            for n in names:
                if n in chunks:
                    return parse_signature(chunks[n], n)
            return []

        self.inputs = pick("ISG1", "ISGN")
        self.outputs = pick("OSG1", "OSG5", "OSGN")
        self.patch = pick("PSG1", "PCSG")


# ---------------------------------------------------------------------------------------------
# The linkage rules D3D12 applies between two consecutive stages


def system_generated(consumer, producer, element):
    """Inputs the hardware makes up for the consumer; no earlier stage has to write them."""
    sv = element.sysval
    if consumer == "ps":
        if sv in (9, 10, 66, 70):  # SV_IsFrontFace, SV_SampleIndex, SV_Coverage, SV_InnerCoverage
            return True
        # SV_PrimitiveID comes from a GS when there is one, and from a mesh shader's primitive outputs.
        if sv == 7 and producer not in ("gs", "ms"):
            return True
    if consumer in ("gs", "hs", "ds") and sv == 7:
        return True
    return False


def link(producer, outputs, consumer, inputs):
    """-> [(severity, code, element label, message)]. A mesh shader's outputs are its vertex and
    primitive signatures together; D3D12 matches them to the pixel shader by semantic, not by register
    (measured with tools\\qa\\dxc\\pso_probe: reordered inputs link, a missing semantic is #666, a
    wider mask #662)."""
    by_register = producer != "ms"
    issues = []
    by_key = {}
    for e in outputs:
        if e.stream == 0:
            by_key.setdefault(e.key(), e)
    for e in inputs:
        if system_generated(consumer, producer, e):
            continue
        unused = "" if e.rw else " (declared, never read)"
        where = f"{consumer.upper()} v{e.reg}.{mask_str(e.mask)}"
        o = by_key.get(e.key())
        if o is None:
            issues.append(("error", 666, e.label(),
                           f"{where} is not written by the {producer.upper()}{unused}"))
            continue
        there = f"{producer.upper()} o{o.reg}.{mask_str(o.mask)}"
        if by_register and o.reg != e.reg:
            issues.append(("error", 660, e.label(), f"register mismatch: {where} <- {there}{unused}"))
        if e.mask & ~o.mask:
            issues.append(("error", 662, e.label(), f"mask not a subset: {where} <- {there}{unused}"))
        if o.sysval != e.sysval:
            issues.append(("error", 663, e.label(),
                           f"system value differs: {SV_NAMES.get(e.sysval, e.sysval) or 'none'} <- "
                           f"{SV_NAMES.get(o.sysval, o.sysval) or 'none'}"))
        if o.ctype != e.ctype and o.ctype and e.ctype:
            issues.append(("warning", 661, e.label(),
                           f"component type {COMPONENT_TYPES.get(e.ctype, e.ctype)} <- "
                           f"{COMPONENT_TYPES.get(o.ctype, o.ctype)}: {where} <- {there}"))
        # For an output signature the rw byte is the never-written mask.
        if e.rw & o.rw & o.mask and (o.reg == e.reg or not by_register):
            issues.append(("warning", 664, e.label(),
                           f"reads {mask_str(e.rw & o.rw)} which the {producer.upper()} never writes: {where} <- {there}"))
    return issues


# ---------------------------------------------------------------------------------------------
# Where the pipelines come from


def split_name_options(name):
    """"file(OPT_A,OPT_B,)" -> ("file", ("OPT_A", "OPT_B")): the options-in-name form the blenders use
    next to addShaderOption (uber_deffer.cpp's tessellation, gpudriven.md's DA_TREE_GD)."""
    base, sep, rest = name.partition("(")
    if not sep:
        return name, ()
    return base, tuple(o.strip() for o in rest.rstrip(")").split(",") if o.strip())


class Pipeline:
    def __init__(self, stages, extras=(), origin="", dynamic=False, gate=None):
        stages = {k: v for k, v in stages.items() if v and v.lower() != "null"}
        # The engine's mesh names: the mesh shader in the vertex slot ("name#ms"), the amplification
        # shader in the geometry slot ("name#as"). Options follow the suffix as CreateShader expects
        # them ("name#ms(OPT,)"); "name(OPT,)#ms" is accepted too.
        for slot, suffix, stage in (("vs", "#ms", "ms"), ("gs", "#as", "as")):
            name = stages.get(slot, "")
            base, sep, rest = name.partition("(")
            if base.lower().endswith(suffix):
                name = base[: -len(suffix)] + sep + rest
            elif name.lower().endswith(suffix):
                name = name[: -len(suffix)]
            else:
                continue
            del stages[slot]
            stages[stage] = name
        options = []
        for s in list(stages):
            base, opts = split_name_options(stages[s])
            if opts:
                stages[s] = base
                options += [o for o in opts if o not in options]
        self.stages = stages
        self.extras = tuple(extras) + tuple(o for o in options if o not in extras)
        self.origins = [origin] if origin else []
        self.dynamic = dynamic
        # Set when every call site that builds it sits behind a switch listed in GATES.
        self.gate = gate

    def key(self):
        return tuple(self.stages.get(s, "").lower() for s in STAGES) + (self.extras,)

    def title(self):
        names = [f"{self.stages[s]}.{s}" for s in STAGES if s in self.stages]
        extra = f"  [{','.join(self.extras)}]" if self.extras else ""
        return " -> ".join(names) + extra


def strip_cpp_comments(text):
    out, i, n = [], 0, len(text)
    while i < n:
        if text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j
            out.append("\n" * text.count("\n", i, j))
            i = j + 2
        elif text[i] in "\"'":
            q, j = text[i], i + 1
            while j < n and text[j] != q and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            out.append(text[i:j + 1])
            i = j + 1
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def eval_condition(expr, macros):
    expr = re.sub(r"defined\s*\(\s*(\w+)\s*\)", lambda m: "1" if m.group(1) in macros else "0", expr)
    expr = re.sub(r"defined\s+(\w+)", lambda m: "1" if m.group(1) in macros else "0", expr)
    expr = re.sub(r"__has_include\s*\([^)]*\)", "1", expr)

    def ident(m):
        v = macros.get(m.group(0), "0")
        return str(v) if v != "" else "1"

    expr = re.sub(r"\b[A-Za-z_]\w*\b", ident, expr)
    expr = expr.replace("&&", " and ").replace("||", " or ")
    expr = re.sub(r"!(?!=)", " not ", expr)
    try:
        return bool(eval(expr, {"__builtins__": {}}, {}))  # noqa: S307 - digits and operators only
    except Exception:  # noqa: BLE001
        return True


def active_cpp(text, macros):
    """Blank out what the preprocessor drops for this renderer, keep the line numbers."""
    lines = text.split("\n")
    out, stack, active = [], [], True
    i = 0
    while i < len(lines):
        line = lines[i]
        m = re.match(r"\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)", line)
        if not m:
            out.append(line if active else "")
            i += 1
            continue
        expr = m.group(2)
        skip = 1
        while expr.rstrip().endswith("\\") and i + skip < len(lines):
            expr = expr.rstrip()[:-1] + " " + lines[i + skip]
            skip += 1
        kw = m.group(1)
        if kw in ("if", "ifdef", "ifndef"):
            name = expr.strip().split()[0] if expr.strip() else ""
            cond = (eval_condition(expr, macros) if kw == "if" else
                    (name in macros) if kw == "ifdef" else (name not in macros))
            stack.append([active, cond])
            active = active and cond
        elif kw == "elif" and stack:
            parent, taken = stack[-1]
            cond = not taken and eval_condition(expr, macros)
            stack[-1][1] = taken or cond
            active = parent and cond
        elif kw == "else" and stack:
            parent, taken = stack[-1]
            active = parent and not taken
            stack[-1][1] = True
        elif kw == "endif" and stack:
            active = stack.pop()[0]
        out.extend([""] * skip)
        i += skip
    return "\n".join(out)


def cpp_literal(arg):
    """The value of an argument made only of string literals (and DELIMITER), else None."""
    arg = arg.strip()
    parts = re.split(r'("(?:[^"\\\n]|\\.)*")', arg)
    value, seen = [], False
    for part in parts:
        if part.startswith('"'):
            value.append(re.sub(r"\\(.)", r"\1", part[1:-1]))
            seen = True
        elif part.strip() == "DELIMITER":
            value.append("\\")
        elif part.strip():
            return None
    return "".join(value) if seen else None


def cpp_alternatives(arg):
    """`cond ? "a" : "b"` -> ["a", "b"]; a plain literal -> [it]; anything else -> None."""
    lit = cpp_literal(arg)
    if lit is not None:
        return [lit]
    m = re.fullmatch(r"\s*[^?]+\?\s*(\"[^\"]*\")\s*:\s*(\"[^\"]*\")\s*", arg)
    if m:
        return [cpp_literal(m.group(1)), cpp_literal(m.group(2))]
    return None


def cpp_bool(arg):
    a = arg.strip()
    if a in ("true", "TRUE", "1"):
        return (True,)
    if a in ("false", "FALSE", "0", "nullptr", "NULL"):
        return (False,)
    return (False, True)


def split_args(text):
    args, depth, cur, i = [], 0, [], 0
    while i < len(text):
        c = text[i]
        if c == '"':
            j = i + 1
            while j < len(text) and text[j] != '"':
                j += 2 if text[j] == "\\" else 1
            cur.append(text[i:j + 1])
            i = j + 1
            continue
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        if c == "," and depth == 0:
            args.append("".join(cur))
            cur = []
        else:
            cur.append(c)
        i += 1
    if cur:
        args.append("".join(cur))
    return [a.strip() for a in args]


def call_args(text, start):
    depth, j = 1, start
    while depth and j < len(text):
        c = text[j]
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
        elif c == '"':
            j += 1
            while j < len(text) and text[j] != '"':
                j += 2 if text[j] == "\\" else 1
        j += 1
    return text[start:j - 1]


# Calls whose names are built out of variables the source pins down a few lines above. Read off the
# code; the key is (file, the call's own argument text), the value every real combination.
DYNAMIC_PASSES = {
    # Blender_tree_deferred.cpp: tvs_s is the *_aref shadow VS exactly when oBlend is set, and that
    # is the branch with the aref PS; the other branch has no PS at all.
    ("blender_tree_deferred.cpp", ("tvs_s", '"shadow_direct_base_aref"')):
        [("shadow_direct_tree_aref", "shadow_direct_base_aref"), ("shadow_direct_tree_s_aref", "shadow_direct_base_aref")],
    ("blender_tree_deferred.cpp", ("tvs_s", '"shadow_direct_base"')):
        [("shadow_direct_tree", "shadow_direct_base"), ("shadow_direct_tree_s", "shadow_direct_base")],
    ("blender_tree_deferred.cpp", ("tvs_s", '"null"')):
        [("shadow_direct_tree", "null"), ("shadow_direct_tree_s", "null")],
    # Forward-rendered models: vsname = psname = "<literal>" right before the call.
    ("blender_model_ebb_deferred.cpp", ("vsname", "psname")): [("model_env_lq", "model_env_lq")],
    ("blender_deffer_model.cpp", ("vsname", "psname")): [("model_def_lq", "model_def_lq")],
}
# uber_deffer's vertex spec when the caller passes a variable (tvs = oNotAnTree ? "tree_s" : "tree").
DYNAMIC_VSPEC = {("blender_tree_deferred.cpp", "tvs"): ["tree", "tree_s"]}

# The recorder's own forwarding calls and the uber functions' insides are not call sites.
FORWARDERS = ("blender_recorder", "resourcemanager_scripting", "uber_deffer.cpp")

# A call that sits right under one of these conditions builds a pipeline only behind a legacy switch.
# Such a pipeline is still checked, but reported apart and not counted: its pixel shader was never
# written for the vertex shader the switch pairs it with, so there is no linkage to keep.
GATES = (
    (re.compile(r"if\s*\(\s*RImplementation\.o\.oldshadowcascades\s*\)\s*$"),
     "old shadow cascades only (r2_shadow_cascede_old, or a shader pack whose accum_sun_near.ps takes "
     "v2p_TL2uv - ours takes v2p_volume): the pixel shader expects accum_sun.vs's volume data, and "
     "the TL2uv quad hands it the jitter coordinates as tc.w on D3D11 too"),
)

TESS_METHODS = (("TESS_PN",), ("TESS_HM",), ("TESS_PN", "TESS_HM"))

# Pipelines built in code the collectors cannot read (names put together at run time, passes created
# by a lane's own file). Each DirectX 12 feature lane lists its own between its markers, e.g.
#   EXTRA_PIPELINES.append(Pipeline({"vs": "da_fullscreen", "ps": "da_vrs_debug"}, (), "r4_rendertarget_phase_vrs.cpp"))
#   EXTRA_PIPELINES.append(Pipeline({"vs": "deffer_tree_flat(DA_TREE_GD,)", "ps": "deffer_base_flat"}, (), "da_gpu_trees.cpp"))
#   EXTRA_PIPELINES.append(Pipeline({"vs": "deffer_detail_mesh#ms", "gs": "deffer_detail_cull#as",
#                                    "ps": "deffer_base_aref_flat"}, (), "Blender_detail_still_deferred.cpp"))
EXTRA_PIPELINES = []
# [dx12:async] begin pipelines
# [dx12:async] end pipelines
# [dx12:vrs] begin pipelines
# [dx12:vrs] end pipelines
# [dx12:gd] begin pipelines
# The GPU-driven trees (da_gpu_trees.cpp) pair every tree pass's pixel shader with its vertex shader
# compiled again with DA_TREE_GD; the outputs are the same as the plain variant's.
# Every tree pipeline of Blender_tree_deferred.cpp (uber_deffer: flat/bump, detail, HQ; base, aref,
# atoc; steep and detail-bump pixel shaders) and its two shadow casters.
_gd_pairs = [("shadow_direct_tree", "null"), ("shadow_direct_tree_aref", "shadow_direct_base_aref")]
for _pspec in ("base", "base_aref", "base_atoc", "base_atoc_aref"):
    for _d in ("", "_d"):
        _gd_pairs.append((f"deffer_tree_flat{_d}", f"deffer_{_pspec}_flat{_d}"))
    _gd_pairs.append(("deffer_tree_bump", f"deffer_{_pspec}_bump"))
    for _kind in ("bump", "steep"):
        _gd_pairs.append(("deffer_tree_bump-hq", f"deffer_{_pspec}_{_kind}-hq"))
        for _d in ("_d", "_db"):
            _gd_pairs.append(("deffer_tree_bump_d-hq", f"deffer_{_pspec}_{_kind}{_d}-hq"))
# Put together like uber_deffer's names: a combination the content never builds has no file (dynamic).
for _vs, _ps in _gd_pairs:
    EXTRA_PIPELINES.append(Pipeline({"vs": f"{_vs}(DA_TREE_GD,)", "ps": _ps}, (), "da_gpu_trees.cpp", dynamic=True))
# [dx12:gd] end pipelines
# [dx12:mesh] begin pipelines
# Mesh-shader grass (Blender_detail_still_deferred.cpp, R5 in DXC mode): detail elements 2 (wave) and
# 3 (still) pair the grass mesh shader and its amplification shader with the detail pixel shader.
# The names as the engine spells them (Blender_detail_still_deferred.cpp).
for _ms in ("deffer_detail_mesh#ms(DETAIL_WAVE,)", "deffer_detail_mesh#ms"):
    EXTRA_PIPELINES.append(Pipeline({"vs": _ms, "gs": "deffer_detail_cull#as", "ps": "deffer_base_aref_flat"}, (),
                                    "Blender_detail_still_deferred.cpp"))
# Elements 4 (wave) and 5 (still): the shadow-pass variants with the depth-only pixel shader.
for _ms in ("deffer_detail_mesh#ms(DETAIL_WAVE,DETAIL_DEPTH,)", "deffer_detail_mesh#ms(DETAIL_DEPTH,)"):
    EXTRA_PIPELINES.append(Pipeline({"vs": _ms, "gs": "deffer_detail_cull#as", "ps": "deffer_detail_mesh_depth"}, (),
                                    "Blender_detail_still_deferred.cpp"))
# [dx12:mesh] end pipelines
# [dx12:rt] begin pipelines
# [dx12:rt] end pipelines


def lightmapped(tree, vspec):
    """Only level geometry carries a lightmap (a third texture named lmap*); a spec the content
    never lightmaps has no deffer_<spec>_lmh_* shader at all, and then its tessellated variant
    never gets USE_LM_HEMI either."""
    return (False, True) if any(n.startswith(f"deffer_{vspec}_lmh") for n in tree.files) else (False,)


def uber_deffer(tree, hq, vspec, pspec, aref, tessellated, origin):
    """Mirror of uber_deffer.cpp: every name pair it can put together, with its defines."""
    for lmap in lightmapped(tree, vspec):
        for bump in (False, True):
            for dd, db in ((False, False), (True, False), (False, True), (True, True)):
                for has_db in ((False, True) if db else (False,)):
                    for steep in (False, True):
                        vs = f"deffer_{vspec}{'_lmh' if lmap else ''}"
                        ps = f"deffer_{pspec}{'_lmh' if lmap else ''}{'_aref' if aref else ''}"
                        if not bump:
                            vs += "_flat"
                            ps += "_flat"
                            if hq and (dd or db):
                                vs += "_d"
                                ps += "_d"
                            # The flat "_steep" probe (FS.exist on $game_shaders$ itself, outside r3)
                            # never finds a file, so that suffix is never added.
                        else:
                            vs += "_bump"
                            ps += "_steep" if hq and steep else "_bump"
                            if hq and (dd or db):
                                vs += "_d"
                                ps += "_db" if has_db else "_d"
                        if bump and hq:
                            vs += "-hq"
                            ps += "-hq"
                        yield Pipeline({"vs": vs, "ps": ps}, (), origin, True)
                        if bump and hq and tessellated:
                            for method in TESS_METHODS:
                                extras = method + (("USE_LM_HEMI",) if lmap else ()) + \
                                    (("USE_TDETAIL",) if dd else ()) + (("USE_TDETAIL_BUMP",) if db else ())
                                yield Pipeline({"vs": f"deffer_{vspec}_bump", "hs": "dx11\\tess", "ds": "dx11\\tess",
                                                "ps": f"deffer_{pspec}{'_aref' if aref else ''}_bump"},
                                               extras, origin + " [tessellated]", True)


def uber_shadow(tree, vspec, tessellated, origin):
    yield Pipeline({"vs": "shadow_direct_base", "ps": "dumb"}, (), origin, True)
    if tessellated:
        for lmap in lightmapped(tree, vspec):
            for dd, db in ((False, False), (True, False), (False, True), (True, True)):
                for method in TESS_METHODS:
                    extras = method + (("USE_LM_HEMI",) if lmap else ()) + \
                        (("USE_TDETAIL",) if dd else ()) + (("USE_TDETAIL_BUMP",) if db else ())
                    yield Pipeline({"vs": f"deffer_{vspec}_bump", "hs": "dx11\\tess", "ds": "dx11\\tess_shadow",
                                    "ps": "dumb"}, extras, origin + " [tessellated]", True)


def cpp_sources():
    r4 = REPO / "src" / "Layers" / "xrRenderPC_R4"
    cmake = (r4 / "sources.cmake").read_text("utf-8", errors="replace")
    files = [(r4 / f).resolve() for f in re.findall(r'"\$\{CMAKE_CURRENT_LIST_DIR\}/([^"]+)"', cmake)]
    r5 = REPO / "src" / "Layers" / "xrRenderPC_R5"
    files += [p for p in r5.glob("*.cpp")] + [p for p in r5.glob("*.h")]
    return [p for p in dict.fromkeys(files) if p.suffix in (".cpp", ".h") and p.exists()]


def collect_cpp(tree, api_macros, unresolved):
    calls = re.compile(r"\b(r_Pass|r_TessPass|PassSET_Shaders|uber_deffer|uber_shadow)\s*\(")
    found = []
    for path in cpp_sources():
        rel = path.relative_to(REPO).as_posix()
        base = path.name.lower()
        if any(f in base for f in FORWARDERS):
            continue
        raw = path.read_text("utf-8", errors="replace")
        text = active_cpp(strip_cpp_comments(raw), api_macros)
        tessellated = re.search(r"\bTessMethod\s*=", text) is not None
        for m in calls.finditer(text):
            head = text[max(0, m.start() - 16):m.start()]
            if head.rstrip().endswith("::") or re.search(r"\b(void|IC)\s*$", head):
                continue
            args = split_args(call_args(text, m.end()))
            line = text.count("\n", 0, m.start()) + 1
            origin = f"{rel}:{line}"
            fn = m.group(1)
            if fn in ("uber_deffer", "uber_shadow"):
                if fn == "uber_shadow":
                    specs = cpp_alternatives(args[1]) or DYNAMIC_VSPEC.get((base, args[1]))
                    if not specs:
                        unresolved.append(f"{origin}: uber_shadow({', '.join(args)})")
                        continue
                    for spec in specs:
                        found.extend(uber_shadow(tree, spec, tessellated, f"{origin} uber_shadow({spec})"))
                    continue
                vspecs = cpp_alternatives(args[2]) or DYNAMIC_VSPEC.get((base, args[2]))
                pspecs = cpp_alternatives(args[3]) or DYNAMIC_VSPEC.get((base, args[3]))
                if not vspecs or not pspecs:
                    unresolved.append(f"{origin}: uber_deffer({', '.join(args)})")
                    continue
                for hq in cpp_bool(args[1]):
                    for aref in cpp_bool(args[4]):
                        for vspec in vspecs:
                            for pspec in pspecs:
                                label = f"{origin} uber_deffer(hq={int(hq)}, {vspec}, {pspec}, aref={int(aref)})"
                                found.extend(uber_deffer(tree, hq, vspec, pspec, aref, tessellated, label))
                continue
            if fn == "r_TessPass":
                order = ("vs", "hs", "ds", "gs", "ps")
                names = [cpp_literal(a) for a in args[:5]]
            elif fn == "PassSET_Shaders":
                order = ("vs", "ps", "gs", "hs", "ds")
                names = [cpp_literal(a) for a in args[:5]]
            else:
                # r_Pass(vs, ps, bFog...) or r_Pass(vs, gs, ps, bFog...): the third argument is a
                # name only in the second form.
                third = cpp_literal(args[2]) if len(args) > 2 else None
                if third is not None:
                    order, names = ("vs", "gs", "ps"), [cpp_literal(a) for a in args[:3]]
                else:
                    order, names = ("vs", "ps"), [cpp_literal(a) for a in args[:2]]
            if None not in names[:len(order)]:
                before = re.sub(r"\w+\s*(\.|->)\s*$", "", text[max(0, m.start() - 300):m.start()])
                gate = next((why for cond, why in GATES if cond.search(before)), None)
                found.append(Pipeline(dict(zip(order, names)), (), origin, gate=gate))
                continue
            combos = DYNAMIC_PASSES.get((base, tuple(args[:2])))
            if combos:
                for vs, ps in combos:
                    found.append(Pipeline({"vs": vs, "ps": ps}, (), f"{origin} ({args[0]}={vs})", True))
            else:
                unresolved.append(f"{origin}: {fn}({', '.join(args)})")
    return found


def strip_lua_comments(text):
    text = re.sub(r"--\[(=*)\[.*?\]\1\]", lambda m: "\n" * m.group(0).count("\n"), text, flags=re.S)
    out = []
    for line in text.split("\n"):
        i, q = 0, None
        while i < len(line):
            c = line[i]
            if q:
                if c == "\\":
                    i += 1
                elif c == q:
                    q = None
            elif c in "\"'":
                q = c
            elif line.startswith("--", i):
                line = line[:i]
                break
            i += 1
        out.append(line)
    return "\n".join(out)


def lua_literal(arg):
    m = re.fullmatch(r"\s*([\"'])((?:\\.|(?!\1).)*)\1\s*", arg)
    return re.sub(r"\\(.)", r"\1", m.group(2)) if m else None


def collect_lua(tree, unresolved):
    found = []
    for name in tree.names(".s"):
        path, tag = tree.lookup(name)
        text = strip_lua_comments(path.read_text("utf-8", errors="replace"))
        for m in re.finditer(r":\s*begin\s*\(", text):
            args = split_args(call_args(text, m.end()))
            line = text.count("\n", 0, m.start()) + 1
            origin = f"{name} ({tag}):{line}"
            names = [lua_literal(a) for a in args]
            if None in names or len(names) not in (2, 3):
                unresolved.append(f"{origin}: begin({', '.join(args)})")
                continue
            order = ("vs", "ps") if len(names) == 2 else ("vs", "gs", "ps")
            found.append(Pipeline(dict(zip(order, names)), (), origin))
    return found


# ---------------------------------------------------------------------------------------------
# Compilation, cached per (file, defines) and again per preprocessed text


class Built:
    __slots__ = ("sig", "error", "dx9", "missing", "fmt", "dxc_error")

    def __init__(self, sig=None, error=None, dx9=False, missing=False, fmt="dxbc", dxc_error=None):
        self.sig, self.error, self.dx9, self.missing = sig, error, dx9, missing
        # "dxil" or "dxbc"; dxc_error is set on a DXBC stage that DXC refused (DXC mode).
        self.fmt, self.dxc_error = fmt, dxc_error


class Builder:
    """FXC by default; with dxc set, the DirectX 12 module's DXC path with FXC as the per-shader
    fallback (dxc.md 3.3, 3.6)."""

    def __init__(self, compiler, tree, dxc=None, sm="6_6", hv="2018", xfr=()):
        self.compiler, self.tree = compiler, tree
        self.dxc, self.sm, self.hv, self.xfr = dxc, sm, hv, list(xfr)
        self._by_input = {}
        self._by_text = {}
        self._lock = threading.Lock()
        self._events = {}

    def _once(self, cache, key, fn):
        with self._lock:
            if key in cache:
                value = cache[key]
                if not isinstance(value, threading.Event):
                    return value
                event = value
                owner = False
            else:
                event = threading.Event()
                cache[key] = event
                owner = True
        if not owner:
            event.wait()
            with self._lock:
                return cache[key]
        try:
            value = fn()
        except Exception as exc:  # noqa: BLE001
            value = Built(error=f"internal: {exc!r}")
        with self._lock:
            cache[key] = value
        event.set()
        return value

    def build(self, stage, name, defines, want_dx9=False, fxc_only=False):
        base = name.split("(")[0]
        use_dxc = bool(self.dxc) and not fxc_only
        return self._once(self._by_input, (stage, base.lower(), defines, want_dx9 and not use_dxc, use_dxc),
                          lambda: self._build(stage, base, defines, want_dx9, use_dxc))

    def _build(self, stage, name, defines, want_dx9, use_dxc):
        hit = self.tree.lookup(f"{name}.{stage}")
        if not hit:
            return Built(missing=True)
        raw = hit[0].read_bytes()
        entry = "main"
        for legacy in ((b"main_vs_1_1", b"main_vs_2_0") if stage == "vs" else
                       (b"main_ps_1_1", b"main_ps_1_2", b"main_ps_1_3", b"main_ps_1_4", b"main_ps_2_0")
                       if stage == "ps" else ()):
            if legacy in raw:
                entry = legacy.decode()
                break
        pairs = [tuple(d.split("=", 1)) if "=" in d else (d, "1") for d in defines]
        if use_dxc:
            # The file name goes into the #line markers, so DXC's messages name the file.
            text, err = self.compiler.preprocess(raw, pairs + self.xfr, f"{name}.{stage}".encode())
            if text is None:
                return Built(error=first_error(err), fmt="dxil")
            digest = hashlib.sha1(text).hexdigest()
            dxil = self._once(self._by_text, (digest, entry, stage, "dxil"),
                              lambda: self._compile_dxc(text, f"{name}.{stage}", entry, stage))
            if dxil.sig is not None or stage in DXC_ONLY_STAGES:
                return dxil
            # The engine compiles this one shader with FXC instead (its pass then gets DXBC twins).
            fxc = self.build(stage, name, defines, want_dx9, fxc_only=True)
            return Built(sig=fxc.sig, error=fxc.error, dx9=fxc.dx9, missing=fxc.missing, fmt="dxbc",
                         dxc_error=dxil.error)
        if stage in DXC_ONLY_STAGES:
            return Built(error=f"{stage} shaders exist only in DXC mode")
        text, err = self.compiler.preprocess(raw, pairs)
        if text is None:
            return Built(error=first_error(err))
        flags = BASE_FLAGS | (D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY if want_dx9 else 0)
        digest = hashlib.sha1(text).hexdigest()
        return self._once(self._by_text, (digest, entry, stage, flags),
                          lambda: self._compile(text, entry, TARGETS[stage], flags))

    def _compile(self, text, entry, target, flags):
        code, err = self.compiler.compile(text, entry, target, flags)
        dx9 = bool(flags & D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY)
        if code is None and "error X3523" in err and not dx9:
            flags |= D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY
            dx9 = True
            code, err = self.compiler.compile(text, entry, target, flags)
        if code is None:
            return Built(error=first_error(err))
        return Built(sig=Signatures(code), dx9=dx9)

    def _compile_dxc(self, text, file_name, entry, stage):
        hv = "2021" if stage in HLSL_2021_STAGES else self.hv
        obj, _refl, msg = self.dxc.compile(text, file_name, entry, dxc_target(stage, self.sm), hv)
        if obj is None:
            return Built(error=first_dxc_error(msg), fmt="dxil")
        return Built(sig=Signatures(obj), fmt="dxil")


def first_error(err):
    for line in err.splitlines():
        if ": error " in line:
            return line.strip()
    return (err.strip().splitlines() or ["compilation failed"])[0]


# ---------------------------------------------------------------------------------------------


def uses_skinning(tree, vs):
    hit = tree.lookup(vs.split("(")[0] + ".vs")
    if not hit:
        return False
    text = hit[0].read_text("utf-8", errors="replace")
    return bool(re.search(r"SKIN_0|skin\.h|deffer_model_", text)) or vs.lower().startswith("deffer_model_")


def define_sets(apis, perms, skins=SKINS[:1], extras=()):
    """(label, permutation, defines) for every API x permutation x (bare, +game) x skinning mode."""
    for api, api_defs in apis:
        for perm, perm_defs in perms:
            for game in (False, True):
                game_defs = GAME_COMMON + GAME[perm] if game else ()
                for skin in skins:
                    defines = tuple(f"{k}={v}" for k, v in ALWAYS) + perm_defs + game_defs + api_defs + \
                        (f"{skin}=1",) + tuple(f"{e}=1" for e in extras)
                    label = f"{api}:{perm}" + ("+game" if game else "") + (f":{skin}" if skin != "SKIN_NONE" else "")
                    yield label, perm, defines


def variants(pipeline, tree, apis, perms):
    skins = SKINS if "vs" in pipeline.stages and uses_skinning(tree, pipeline.stages["vs"]) else SKINS[:1]
    return define_sets(apis, perms, skins, pipeline.extras)


def check_pipeline(builder, pipeline, label, perm, defines):
    """-> (issues, compile problems, missing files, files replaced by the stub, note). note is None or
    (kind, text): "mesh-fxc" a mesh pipeline in FXC mode (not built), "twins" DXC refused a stage and
    the pass links as DXBC, "unavailable" a mesh pipeline with a stage that is not DXIL."""
    order = [s for s in STAGES if s in pipeline.stages]
    mesh = any(s in DXC_ONLY_STAGES for s in order)
    if mesh and not builder.dxc:
        return [], [], [], [], ("mesh-fxc", "mesh pipelines are built only when engine shaders are DXIL")
    if mesh and sm_tuple(builder.sm) < MESH_MIN_SM:
        return [], [], [], [], ("unavailable", f"shader model {builder.sm.replace('_', '.')} has no mesh shaders")

    def build_all(fxc_only):
        built = {}
        dx9 = False
        # PS first, like the recorder: a PS that needed DX9 compatibility drags the VS along.
        if "ps" in pipeline.stages:
            built["ps"] = builder.build("ps", pipeline.stages["ps"], defines, fxc_only=fxc_only)
            dx9 = built["ps"].dx9
        for s in order:
            if s != "ps":
                built[s] = builder.build(s, pipeline.stages[s], defines, dx9 and s == "vs", fxc_only=fxc_only)
        # CreateShader falls back to stub_default.<stage> for a file that is not there (R4 always
        # allows it), so a name the source spells out still makes a pipeline - with the stub in it.
        # Put-together names with no file are combinations the content never asks for.
        stubbed = []
        if not pipeline.dynamic:
            for s in order:
                if built[s].missing:
                    stubbed.append(f"{pipeline.stages[s]}.{s}")
                    built[s] = builder.build(s, "stub_default", defines, dx9 and s == "vs", fxc_only=fxc_only)
        return built, stubbed

    built, stubbed = build_all(False)
    note = None
    fell_back = [s for s in order if built[s].dxc_error and built[s].sig is not None]
    if builder.dxc and fell_back:
        refused = "; ".join(f"{pipeline.stages[s]}.{s}: {built[s].dxc_error}" for s in fell_back)
        if mesh:
            return [], [], [], stubbed, ("unavailable", refused)
        # UnifyShaderFormats: every stage of the pass becomes its FXC twin.
        if any(built[s].fmt == "dxil" for s in order):
            built, stubbed = build_all(True)
        note = ("twins", refused)
    missing = [f"{pipeline.stages[s]}.{s}" for s in order if built[s].missing]
    problems = []
    for s in order:
        b = built[s]
        if b.error:
            fname = pipeline.stages[s].split("(")[0].split("\\")[-1].lower() + "." + s
            if any(fname == k and fnmatch.fnmatchcase(perm, p) for k, p in KNOWN_UNBUILDABLE):
                continue
            problems.append(f"{pipeline.stages[s]}.{s}: {b.error}")
    if missing or any(built[s].sig is None for s in order):
        return [], problems, missing, stubbed, note
    issues = []
    for a, b in zip(order, order[1:]):
        # An amplification shader hands the mesh shader a payload, not a signature.
        if a == "as":
            continue
        tag = f"{a.upper()}->{b.upper()}" + ("(stub)" if {a, b} & {x.rsplit(".", 1)[1] for x in stubbed} else "")
        outputs = built[a].sig.outputs + (built[a].sig.patch if a == "ms" else [])
        for issue in link(a, outputs, b, built[b].sig.inputs):
            issues.append((tag,) + issue)
        if a == "hs" and b == "ds":
            for issue in link("hs", built[a].sig.patch, "ds", built[b].sig.patch):
                issues.append(("HS->DS patch",) + issue)
    return issues, problems, missing, stubbed, note


def compress_labels(labels, run):
    """A permutation list short enough to read. Labels are api:Perm[+game][:SKIN_n]; a group whose
    every skinning mode is hit collapses to its permutation, a permutation hit with and without the
    game defines to its bare name, an API hit everywhere to 'all'."""
    hit, ran = set(labels), list(dict.fromkeys(run))
    if hit >= set(ran):
        return "every permutation"

    def split(label):
        api, rest = label.split(":", 1)
        perm, _, skin = rest.partition(":")
        return api, perm, skin

    groups = {}
    for label in ran:
        api, perm, skin = split(label)
        groups.setdefault((api, perm), []).append(label)
    per_api = {}
    for (api, perm), members in groups.items():
        got = [m for m in members if m in hit]
        if not got:
            continue
        text = perm if len(got) == len(members) else ", ".join(f"{perm}:{split(m)[2]}" for m in got)
        per_api.setdefault(api, []).append((perm, text, len(got) == len(members)))
    parts = {}
    for api, entries in per_api.items():
        perms = [p for (a, p) in groups if a == api]
        names = {p for p, _, full in entries if full}
        if names >= set(perms):
            parts[api] = "all"
            continue
        # "High" and "High+game" both whole -> "High (both)".
        out = []
        for p, text, full in entries:
            base = p.replace("+game", "")
            if p.endswith("+game") and full and base in names:
                continue
            if not p.endswith("+game") and full and f"{base}+game" in names:
                out.append(f"{base} (both)")
            else:
                out.append(text)
        parts[api] = ", ".join(out)
    if len(set(parts.values())) == 1 and len(parts) > 1:
        return f"{'+'.join(parts)} {next(iter(parts.values()))}"
    return "; ".join(f"{api} {text}" for api, text in parts.items())


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--unpack", type=pathlib.Path, default=DEFAULT_UNPACK)
    ap.add_argument("--loose", type=pathlib.Path, default=DEFAULT_LOOSE)
    ap.add_argument("--filter", default="", help="only pipelines with a stage name containing this")
    ap.add_argument("--api", choices=("dx11", "dx12", "both"), default="both")
    ap.add_argument("--perm", default="", help="only this permutation (Test-Shaders.ps1 names)")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--list", action="store_true", help="print the pipelines and their origins, compile nothing")
    ap.add_argument("--dump", metavar="NAME.STAGE", help="print one shader's signatures (e.g. da_taa.ps) and exit")
    ap.add_argument("--no-warnings", action="store_true")
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--compiler", choices=("fxc", "dxc"), default="fxc",
                    help="dxc: the DirectX 12 module's DXIL mode (DirectX 12 define set only)")
    ap.add_argument("--sm", default="6_6", help="DXC mode: shader model of the engine shaders (6_0 .. 6_6)")
    ap.add_argument("--hv", default="2018", help="DXC mode: HLSL version of the legacy shader base")
    ap.add_argument("--dxc-dir", type=pathlib.Path, default=DEFAULT_DXC_DIR)
    ap.add_argument("--wave-lanes", type=int, default=32, help="DXC mode: GRIZZLY_WAVE_LANES (0 = no wave ops)")
    ap.add_argument("--no-min16", action="store_true", help="DXC mode: a device without 16-bit min precision")
    args = ap.parse_args()

    tree = Tree(args.unpack, args.loose)
    if not tree.files:
        sys.exit(f"no shader tree under {args.unpack} / {args.loose}")

    unresolved = []
    pipelines = {}
    r4 = {"USE_DX11": "1", "RENDER": "4", "R_GL": "0", "R_R1": "1", "R_R2": "2", "R_R3": "3", "R_R4": "4",
          "HAS_DX11_2": "1", "HAS_DX11_3": "1", "HAS_DXGI1_4": "1", "HAS_DXGI1_5": "1", "NDEBUG": "1"}
    # DXC mode exists only in the DirectX 12 module: its sources alone.
    sources = [] if args.compiler == "dxc" else collect_cpp(tree, r4, unresolved)
    sources += collect_cpp(tree, dict(r4, XR_D3D12="1"), unresolved if args.compiler == "dxc" else [])
    sources += collect_lua(tree, unresolved)
    sources += EXTRA_PIPELINES
    for p in sources:
        k = p.key()
        if k in pipelines:
            for o in p.origins:
                if o not in pipelines[k].origins:
                    pipelines[k].origins.append(o)
            pipelines[k].dynamic = pipelines[k].dynamic and p.dynamic
            if not (pipelines[k].gate and p.gate):
                pipelines[k].gate = None
        else:
            pipelines[k] = p
    selected = [p for p in pipelines.values()
                if ("ps" in p.stages or "ms" in p.stages) and
                (not args.filter or any(args.filter.lower() in n.lower() for n in p.stages.values()))]

    if args.list:
        for p in sorted(selected, key=lambda p: p.title().lower()):
            print(p.title())
            for o in p.origins[:6]:
                print(f"    {o}")
            if len(p.origins) > 6:
                print(f"    ... {len(p.origins) - 6} more")
        for u in unresolved:
            print(f"UNRESOLVED  {u}")
        return 0

    sdk = find_sdk_bin()
    compiler = Compiler(sdk, tree)
    dxc = Dxc(args.dxc_dir) if args.compiler == "dxc" else None
    builder = Builder(compiler, tree, dxc, args.sm, args.hv, dxc_macros(args.sm, args.wave_lanes, not args.no_min16))
    # DXC exists only in the DirectX 12 module.
    api = "dx12" if dxc else args.api
    apis = [a for a in APIS if api in ("both", a[0])]
    perms = [p for p in PERMS if not args.perm or p[0].lower() == args.perm.lower()]

    if args.dump:
        name, _, stage = args.dump.rpartition(".")
        seen = {}
        for label, _perm, defines in define_sets(apis, perms):
            b = builder.build(stage, name, defines)
            if b.missing or b.sig is None:
                print(f"{label}: {'missing' if b.missing else b.error}")
                continue
            text = []
            for title, sig in (("in", b.sig.inputs), ("out", b.sig.outputs), ("patch", b.sig.patch)):
                for e in sig:
                    text.append(f"  {title:<5} {e.label():<22} reg {e.reg:<2} mask {mask_str(e.mask):<4} "
                                f"rw {mask_str(e.rw):<4} {COMPONENT_TYPES.get(e.ctype, e.ctype):<6} "
                                f"{SV_NAMES.get(e.sysval, e.sysval)}")
            seen.setdefault("\n".join(text), []).append(label)
        for text, labels in seen.items():
            print(", ".join(labels))
            print(text)
        return 0
    jobs = []
    for p in selected:
        for label, perm, defines in variants(p, tree, apis, perms):
            jobs.append((p, label, perm, defines))

    what = f"DXC {args.dxc_dir}, SM {args.sm.replace('_', '.')}, HLSL {args.hv}, FXC fallback {sdk}" if dxc else f"compiler {sdk}"
    print(f"{len(selected)} pipeline(s), {len(jobs)} stage combination(s) to link; {what}", flush=True)
    results = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        futures = {pool.submit(check_pipeline, builder, *job): job for job in jobs}
        for fut in concurrent.futures.as_completed(futures):
            p, label, _perm, _defines = futures[fut]
            results.setdefault(p.key(), []).append((label, fut.result()))
    order = [label for _, label, _, _ in jobs]
    order = sorted(set(order), key=order.index)

    errors = 0
    broken, gated, warned, compile_fail, missing_literal, missing_dynamic = [], [], [], {}, {}, {}
    notes = {}
    for p in selected:
        per = results.get(p.key(), [])
        run = sorted((label for label, _ in per), key=order.index)
        grouped = {}
        for label, (issues, problems, missing, stubbed, note) in per:
            if note:
                notes.setdefault(note[0], {}).setdefault((p.title(), note[1]), [[], run])[0].append(label)
            for issue in issues:
                grouped.setdefault(issue, []).append(label)
            for prob in problems:
                compile_fail.setdefault((p.title(), prob), [[], run])[0].append(label)
            if missing:
                missing_dynamic.setdefault(p.title(), set()).update(missing)
            if stubbed:
                missing_literal.setdefault(p.title(), set()).update(stubbed)
        errs = {k: v for k, v in grouped.items() if k[1] == "error"}
        warns = {k: v for k, v in grouped.items() if k[1] != "error"}
        if errs and p.gate:
            gated.append((p, errs, run))
        elif errs:
            broken.append((p, errs, run))
            errors += len(errs)
        if warns:
            warned.append((p, warns, run))

    def show(p, issues, run):
        print(f"{p.title()}")
        # One permutation line per distinct set, the issues that share it underneath.
        by_labels = {}
        for issue, labels in issues.items():
            by_labels.setdefault(compress_labels(labels, run), []).append(issue)
        for where, group in by_labels.items():
            print(f"    in {where}:")
            for link_name, _sev, code, elem, msg in sorted(group, key=lambda i: (i[0], i[3], i[2])):
                print(f"      {link_name:<8} {elem:<14} #{code} {msg}")
        for o in p.origins[:4]:
            print(f"    from {o}")
        if len(p.origins) > 4:
            print(f"    from ... {len(p.origins) - 4} more")

    if broken:
        print("\n== Pipelines D3D12 refuses")
        for p, issues, run in sorted(broken, key=lambda t: t[0].title().lower()):
            show(p, issues, run)
    if gated:
        print("\n== Refused too, but built only behind a legacy switch the game never sets (not counted)")
        for why in dict.fromkeys(p.gate for p, _, _ in gated):
            print(f"  {why}")
            for p, issues, run in sorted((g for g in gated if g[0].gate == why), key=lambda t: t[0].title().lower()):
                codes = sorted({f"#{i[2]}" for i in issues})
                print(f"    {p.title()}  ({', '.join(codes)}; {p.origins[0]})")
    if warned and not args.no_warnings:
        print("\n== Warnings (the pipeline is created, but the data is suspect)")
        for p, issues, run in sorted(warned, key=lambda t: t[0].title().lower()):
            show(p, issues, run)
    if notes.get("twins"):
        print("\n== DXC refuses a stage: the engine compiles it with FXC and the pass runs as DXBC twins (linked as such)")
        for (title, why), (labels, run) in sorted(notes["twins"].items()):
            print(f"{title}\n    {why}\n    in {compress_labels(labels, run)}")
    if notes.get("unavailable"):
        print("\n== Mesh pipelines with a stage that is not DXIL: unavailable, the engine draws the vertex path")
        for (title, why), (labels, run) in sorted(notes["unavailable"].items()):
            print(f"{title}\n    {why}\n    in {compress_labels(labels, run)}")
    if notes.get("mesh-fxc"):
        print("\n== Mesh pipelines (checked only with --compiler dxc): " +
              ", ".join(sorted({title for title, _ in notes["mesh-fxc"]})))
    if compile_fail:
        print("\n== Shaders that do not compile in a permutation (no linkage verdict for those)")
        for (title, prob), (labels, run) in sorted(compile_fail.items()):
            print(f"{title}\n    {prob}\n    in {compress_labels(labels, run)}")
    if missing_literal:
        print("\n== Named in the source, but no such shader file (linked with stub_default in its place, as the engine does)")
        for title, files in sorted(missing_literal.items()):
            print(f"{title}: missing {', '.join(sorted(files))}")
    if missing_dynamic and args.verbose:
        print("\n== Put-together names with no file (combinations the content never produces)")
        for title, files in sorted(missing_dynamic.items()):
            print(f"{title}: missing {', '.join(sorted(files))}")
    if unresolved:
        print("\n== Call sites whose names could not be resolved")
        for u in unresolved:
            print(f"    {u}")
    if compiler.missing_includes and args.verbose:
        print("\n== Includes not found: " + ", ".join(sorted(compiler.missing_includes)))
    dxc_part = (f", {len({t for t, _ in notes.get('twins', {})})} as DXBC twins, "
                f"{len({t for t, _ in notes.get('unavailable', {})})} mesh pipeline(s) unavailable") if dxc else ""
    print(f"\n{len(selected)} pipeline(s), {len(broken)} refused by D3D12 ({errors} problem(s)), "
          f"{len(gated)} behind a legacy switch, {len(warned)} with warnings, "
          f"{len(unresolved)} unresolved call site(s){dxc_part}")
    return 1 if broken else 0


if __name__ == "__main__":
    sys.exit(main())
