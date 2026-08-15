"""Headless LuaJIT runner for the NQ runtime tests (test tooling, not shipped).

Loads the game's LuaJIT.dll through ctypes and executes a Lua file with the standard
libraries opened, or syntax-checks files without running them.

    python run_lua.py test_examples.lua [args...]      run a script (exit code 0 = passed)
    python run_lua.py --check file.lua [file2 ...]     compile only (syntax check)

Script arguments are exposed to Lua as the global table `arg` (arg[0] = script path).
"""
import ctypes
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
GAME_DIR = os.environ.get("NQ_GAME_DIR", r"D:\Games\Dead Air")
DLL_CANDIDATES = [os.path.join(GAME_DIR, "LuaJIT.dll"), os.path.join(GAME_DIR, "lua51.dll")]

LUA_MULTRET = -1
LUA_GLOBALSINDEX = -10002


def load_lua():
    last = None
    for path in DLL_CANDIDATES:
        if not os.path.exists(path):
            continue
        try:
            os.add_dll_directory(os.path.dirname(path))
            dll = ctypes.CDLL(path)
            dll.luaL_newstate.restype = ctypes.c_void_p
            dll.luaL_openlibs.argtypes = [ctypes.c_void_p]
            dll.lua_close.argtypes = [ctypes.c_void_p]
            dll.lua_pcall.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int]
            dll.lua_settop.argtypes = [ctypes.c_void_p, ctypes.c_int]
            dll.lua_gettop.argtypes = [ctypes.c_void_p]
            dll.lua_tolstring.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.POINTER(ctypes.c_size_t)]
            dll.lua_tolstring.restype = ctypes.c_char_p
            dll.lua_pushstring.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
            dll.lua_createtable.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
            dll.lua_rawseti.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
            dll.lua_setfield.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_char_p]
            if hasattr(dll, "luaL_loadfilex"):
                dll.luaL_loadfilex.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p]
                dll.luaL_loadfilex.restype = ctypes.c_int
                dll._loadfile = lambda L, p: dll.luaL_loadfilex(L, p, None)
            else:
                dll.luaL_loadfile.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
                dll.luaL_loadfile.restype = ctypes.c_int
                dll._loadfile = lambda L, p: dll.luaL_loadfile(L, p)
            return dll, path
        except OSError as e:  # 32-bit dll under 64-bit python etc.
            last = e
    raise SystemExit("cannot load LuaJIT: %s (tried %s)" % (last, DLL_CANDIDATES))


def lua_error(dll, L):
    n = ctypes.c_size_t(0)
    s = dll.lua_tolstring(L, -1, ctypes.byref(n))
    dll.lua_settop(L, -2)
    return s.decode("cp1251", "replace") if s else "<no message>"


def push_args(dll, L, script, args):
    dll.lua_createtable(L, len(args), 1)
    dll.lua_pushstring(L, script.encode("utf-8"))
    dll.lua_rawseti(L, -2, 0)
    for i, a in enumerate(args, 1):
        dll.lua_pushstring(L, a.encode("utf-8"))
        dll.lua_rawseti(L, -2, i)
    dll.lua_setfield(L, LUA_GLOBALSINDEX, b"arg")


def check_files(dll, files):
    bad = 0
    for f in files:
        L = dll.luaL_newstate()
        dll.luaL_openlibs(L)
        rc = dll._loadfile(L, os.path.abspath(f).encode("utf-8"))
        if rc != 0:
            bad += 1
            print("SYNTAX ERROR %s: %s" % (f, lua_error(dll, L)))
        else:
            print("ok %s" % f)
        dll.lua_close(L)
    return bad


def run_file(dll, script, args):
    L = dll.luaL_newstate()
    dll.luaL_openlibs(L)
    push_args(dll, L, os.path.abspath(script), args)
    rc = dll._loadfile(L, os.path.abspath(script).encode("utf-8"))
    if rc != 0:
        print("LOAD ERROR: %s" % lua_error(dll, L))
        dll.lua_close(L)
        return 2
    rc = dll.lua_pcall(L, 0, LUA_MULTRET, 0)
    if rc != 0:
        print("RUNTIME ERROR: %s" % lua_error(dll, L))
        dll.lua_close(L)
        return 1
    dll.lua_close(L)
    return 0


def main(argv):
    if not argv:
        print(__doc__)
        return 2
    dll, path = load_lua()
    if argv[0] == "--check":
        bad = check_files(dll, argv[1:])
        print("%d file(s) with syntax errors" % bad)
        return 1 if bad else 0
    return run_file(dll, argv[0], argv[1:])


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.exit(main(sys.argv[1:]))
