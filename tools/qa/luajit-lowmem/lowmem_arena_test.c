/*
** Low-address arena test for the Dead Air LuaJIT build (Windows x64, no LJ_GC64).
**
** The engine runs LuaJIT without LJ_GC64, so every GC object has to live below 2 GB. This program
** puts a LuaJIT.dll into the situation that killed the game on some machines: all address space
** below 2 GB that is still free once the DLL is loaded gets taken away, the way 16 GB of mapped
** archives take it away when the OS places them bottom-up. A DLL with the arena keeps working,
** a DLL without it cannot allocate a single byte.
**
** Usage: lowmem_arena_test.exe <path to LuaJIT.dll> [expect-starved | run <file.lua>]
**   expect-starved: the DLL is known to have no arena; the run passes when it fails to allocate.
**   run: no test at all, the file is executed with that DLL. The runner compares what two builds
**        print for table_order_fingerprint.lua.
*/

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>

#pragma comment(lib, "dbghelp.lib")

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct lua_State lua_State;

/* Mirrors luaJIT_LowMem from luajit.h: the DLL is loaded by path, so nothing is linked. */
typedef struct LowMem {
  size_t reserved, used, peak, outside;
} LowMem;

typedef lua_State *(*fn_newstate)(void);
typedef void (*fn_openlibs)(lua_State *);
typedef int (*fn_loadstring)(lua_State *, const char *);
typedef int (*fn_pcall)(lua_State *, int, int, int);
typedef const char *(*fn_tolstring)(lua_State *, int, size_t *);
typedef void (*fn_settop)(lua_State *, int);
typedef void (*fn_close)(lua_State *);
typedef int (*fn_loadfile)(lua_State *, const char *);
typedef int (*fn_lowmem)(LowMem *);

static struct {
  fn_newstate newstate;
  fn_openlibs openlibs;
  fn_loadstring loadstring;
  fn_pcall pcall;
  fn_tolstring tolstring;
  fn_settop settop;
  fn_close close;
  fn_loadfile loadfile;
  fn_lowmem lowmem;  /* NULL for a DLL that predates the arena. */
} lua;

#define MB(n) ((double)(n) / (1024.0 * 1024.0))

static int failures;

/* A fault inside of the DLL under test has to name the place, the exit code alone says nothing. */
static LONG WINAPI report_fault(EXCEPTION_POINTERS *ep)
{
  HANDLE process = GetCurrentProcess();
  void *frames[32];
  USHORT nframes, i;
  if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
    return EXCEPTION_CONTINUE_SEARCH;
  printf("ACCESS VIOLATION at %p, touching %p\n", ep->ExceptionRecord->ExceptionAddress,
	 (void *)ep->ExceptionRecord->ExceptionInformation[1]);
  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
  SymInitialize(process, NULL, TRUE);
  frames[0] = ep->ExceptionRecord->ExceptionAddress;
  nframes = (USHORT)(1 + CaptureStackBackTrace(0, 31, frames + 1, NULL));
  for (i = 0; i < nframes; i++) {
    char buffer[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *symbol = (SYMBOL_INFO *)buffer;
    DWORD64 displacement = 0;
    memset(buffer, 0, sizeof(buffer));
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 255;
    if (SymFromAddr(process, (DWORD64)(uintptr_t)frames[i], &displacement, symbol))
      printf("  %2u %p %s + 0x%llx\n", i, frames[i], symbol->Name, (unsigned long long)displacement);
    else
      printf("  %2u %p\n", i, frames[i]);
  }
  ExitProcess(3);
}

static void check(int ok, const char *what)
{
  printf("  [%s] %s\n", ok ? " ok " : "FAIL", what);
  if (!ok) failures++;
}

/* Left free below 2 GB, so that the fallback behind a full arena has something to find. */
#define SPARE_LOW_MEMORY ((size_t)96 << 20)

/* Take the free blocks below 2 GB, except for one piece of 'spare' bytes. Returns the bytes taken. */
static size_t starve_low_memory(size_t spare)
{
  uintptr_t addr = 0x10000;
  size_t taken = 0;
  while (addr < 0x7ffe0000u) {
    MEMORY_BASIC_INFORMATION minfo;
    uintptr_t end;
    if (VirtualQuery((void *)addr, &minfo, sizeof(minfo)) == 0) break;
    end = (uintptr_t)minfo.BaseAddress + minfo.RegionSize;
    if (end <= addr) break;
    if (minfo.State == MEM_FREE) {
      uintptr_t lo = (addr + 0xffff) & ~(uintptr_t)0xffff;
      uintptr_t hi = end < 0x7ffe0000u ? end : 0x7ffe0000u;
      if (spare && hi > lo && hi - lo > spare) {
	lo += spare;
	spare = 0;
      }
      if (hi > lo && VirtualAlloc((void *)lo, hi - lo, MEM_RESERVE, PAGE_NOACCESS))
	taken += hi - lo;
    }
    addr = end;
  }
  return taken;
}

static int run(lua_State *L, const char *chunk, const char **message)
{
  int status = lua.loadstring(L, chunk);
  if (status == 0) status = lua.pcall(L, 0, 0, 0);
  *message = status ? lua.tolstring(L, -1, NULL) : NULL;
  return status;
}

/* Small objects grow the segments, the long strings go through the direct (top-down) path. */
static const char *const workload =
  "local keep = {}\n"
  "for i = 1, 200000 do keep[i] = { i, tostring(i), x = i * 0.5 } end\n"
  "for i = 1, 48 do keep[#keep + 1] = string.rep(string.char(65 + i % 26), 4 * 1024 * 1024) .. i end\n"
  "local sum = 0\n"
  "for i = 1, 200000 do sum = sum + keep[i][1] end\n"
  "assert(sum == 200000 * 200001 / 2)\n"
  "_G.keep = keep\n";

static const char *const release_chunk =
  "_G.keep = nil\n"
  "collectgarbage() collectgarbage()\n";

/* Grows until the arena is gone: the error has to be a catchable one. The interpreter reports
** an allocation failure reliably, a trace exit in the middle of one is LuaJIT's own business. */
static const char *const exhaust_chunk =
  "jit.off()\n"
  "local hog = {}\n"
  "for i = 1, 100000 do hog[i] = string.rep('x', 8 * 1024 * 1024) .. i end\n";

typedef struct Worker {
  int id;
  int status;
} Worker;

static DWORD WINAPI worker_main(LPVOID arg)
{
  Worker *w = (Worker *)arg;
  const char *message;
  int round;
  w->status = 0;
  for (round = 0; round < 3 && w->status == 0; round++) {
    lua_State *L = lua.newstate();
    if (!L) { w->status = -1; break; }
    lua.openlibs(L);
    w->status = run(L, workload, &message);
    if (w->status == 0) w->status = run(L, release_chunk, &message);
    lua.close(L);
  }
  return 0;
}

int main(int argc, char **argv)
{
  HMODULE dll;
  LowMem info;
  lua_State *L;
  const char *message;
  size_t taken;
  int expect_starved, status, regions = 0;

  if (argc < 2) {
    fprintf(stderr, "usage: %s <LuaJIT.dll> [expect-starved | run <file.lua>]\n", argv[0]);
    return 2;
  }
  expect_starved = argc > 2 && strcmp(argv[2], "expect-starved") == 0;
  memset(&info, 0, sizeof(info));
  setvbuf(stdout, NULL, _IONBF, 0);  /* A crash must not swallow the report. */
  AddVectoredExceptionHandler(1, report_fault);

  /* First thing in the process, like the loader does it for the engine. */
  dll = LoadLibraryA(argv[1]);
  if (!dll) {
    fprintf(stderr, "cannot load %s (error %lu)\n", argv[1], GetLastError());
    return 2;
  }
  lua.newstate = (fn_newstate)GetProcAddress(dll, "luaL_newstate");
  lua.openlibs = (fn_openlibs)GetProcAddress(dll, "luaL_openlibs");
  lua.loadstring = (fn_loadstring)GetProcAddress(dll, "luaL_loadstring");
  lua.pcall = (fn_pcall)GetProcAddress(dll, "lua_pcall");
  lua.tolstring = (fn_tolstring)GetProcAddress(dll, "lua_tolstring");
  lua.settop = (fn_settop)GetProcAddress(dll, "lua_settop");
  lua.close = (fn_close)GetProcAddress(dll, "lua_close");
  lua.loadfile = (fn_loadfile)GetProcAddress(dll, "luaL_loadfile");
  lua.lowmem = (fn_lowmem)GetProcAddress(dll, "luaJIT_lowmem");
  if (!lua.newstate || !lua.openlibs || !lua.loadstring || !lua.pcall || !lua.tolstring ||
      !lua.settop || !lua.close || !lua.loadfile) {
    fprintf(stderr, "%s does not export the Lua API\n", argv[1]);
    return 2;
  }

  if (argc > 3 && strcmp(argv[2], "run") == 0) {
    /* Only the script may write to stdout here: the runner compares the output of two builds. */
    L = lua.newstate();
    if (!L) return 1;
    lua.openlibs(L);
    status = lua.loadfile(L, argv[3]);
    if (status == 0) status = lua.pcall(L, 0, 0, 0);
    if (status) fprintf(stderr, "%s\n", lua.tolstring(L, -1, NULL));
    lua.close(L);
    return status ? 1 : 0;
  }

  printf("DLL: %s\n", argv[1]);
  if (lua.lowmem) {
    regions = lua.lowmem(&info);
    printf("arena: %.0f MB reserved in %d region(s)\n", MB(info.reserved), regions);
  } else {
    printf("arena: none (the DLL has no luaJIT_lowmem)\n");
  }

  taken = starve_low_memory(SPARE_LOW_MEMORY);
  printf("starved: %.0f MB below 2 GB taken away, %.0f MB left alone\n", MB(taken),
	 MB(SPARE_LOW_MEMORY));

  if (expect_starved) {
    printf("expecting the allocator to fail:\n");
    L = lua.newstate();
    if (!L) {
      check(1, "luaL_newstate() fails without low memory");
    } else {
      lua.openlibs(L);
      status = run(L, workload, &message);
      check(status != 0, "the workload runs out of memory");
      if (message) printf("         Lua says: %s\n", message);
    }
    return failures ? 1 : 0;
  }

  printf("single state:\n");
  check(regions > 0, "the arena exists");
  check(info.reserved >= ((size_t)512 << 20), "at least 512 MB are reserved");
  L = lua.newstate();
  check(L != NULL, "luaL_newstate() succeeds with next to nothing else left below 2 GB");
  if (!L) return 1;
  lua.openlibs(L);
  status = run(L, workload, &message);
  check(status == 0, "200k tables and 192 MB of long strings fit");
  if (message) printf("         Lua says: %s\n", message);
  lua.lowmem(&info);
  printf("         in use %.0f MB, peak %.0f MB, outside %.0f MB\n", MB(info.used), MB(info.peak),
	 MB(info.outside));
  check(info.used >= ((size_t)192 << 20), "the memory came from the arena");
  check(info.outside == 0, "nothing had to come from outside of the arena");
  status = run(L, release_chunk, &message);
  check(status == 0, "a full collection runs");
  lua.lowmem(&info);
  check(info.used < ((size_t)64 << 20), "freed memory goes back to the arena");
  lua.close(L);
  lua.lowmem(&info);
  check(info.used == 0, "closing the state returns every page");

  /* What LuaJIT does after it ran out of memory is not the subject here: the arena has to fail
  ** cleanly, spill over into what is left outside of it, and get all of it back afterwards. */
  printf("exhaustion:\n");
  L = lua.newstate();
  check(L != NULL, "a fresh state for the hog");
  if (!L) return 1;
  lua.openlibs(L);
  status = run(L, exhaust_chunk, &message);
  check(status != 0, "filling the arena ends in a Lua error, not in a crash");
  if (message) printf("         Lua says: %s\n", message);
  lua.lowmem(&info);
  printf("         in use %.0f MB of %.0f MB, outside %.0f MB\n", MB(info.used), MB(info.reserved),
	 MB(info.outside));
  check(info.used + ((size_t)32 << 20) >= info.reserved, "the arena was used up first");
  check(info.outside > 0 && info.outside <= SPARE_LOW_MEMORY, "then the kernel provided the rest");
  lua.close(L);
  lua.lowmem(&info);
  check(info.used == 0, "closing the state returns every page");
  check(info.outside == 0, "and every outside allocation");
  L = lua.newstate();
  check(L != NULL, "the next state starts");
  if (!L) return 1;
  lua.openlibs(L);
  status = run(L, workload, &message);
  check(status == 0, "and the workload fits again");
  if (message) printf("         Lua says: %s\n", message);
  lua.close(L);

  printf("two states on two threads:\n");
  {
    Worker workers[2];
    HANDLE threads[2];
    int i;
    for (i = 0; i < 2; i++) {
      workers[i].id = i;
      workers[i].status = 1;
      threads[i] = CreateThread(NULL, 0, worker_main, &workers[i], 0, NULL);
    }
    WaitForMultipleObjects(2, threads, TRUE, INFINITE);
    for (i = 0; i < 2; i++) CloseHandle(threads[i]);
    check(workers[0].status == 0 && workers[1].status == 0, "both finish three rounds each");
    lua.lowmem(&info);
    check(info.used == 0, "nothing is left behind");
    printf("         peak %.0f MB\n", MB(info.peak));
  }

  printf(failures ? "FAILED: %d check(s)\n" : "PASSED\n", failures);
  return failures ? 1 : 0;
}
