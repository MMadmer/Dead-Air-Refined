/*
** Starts a program the way a Windows with "Randomize memory allocations (Bottom-up ASLR)" switched
** off starts every program: allocations and file views are placed from the lowest free address up.
**
** That is the condition under which the engine's archive views used to fill the range below
** 2 GB before LuaJIT got any of it. The policy is handed to this one child through its process
** creation attributes, so neither the system setting nor the image options are touched.
**
** Usage: launch_bottom_up_off.exe <program> [arguments...]
** The child starts in the directory of <program> and dies with this process, so a QA harness that
** gives up on the launcher cannot leave an engine behind. The exit code is the child's.
*/

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

/* Skips the first token of a command line, quoted or not. */
static const wchar_t *skip_token(const wchar_t *p)
{
  if (*p == L'"') {
    p++;
    while (*p && *p != L'"') p++;
    if (*p) p++;
  } else {
    while (*p && *p != L' ' && *p != L'\t') p++;
  }
  while (*p == L' ' || *p == L'\t') p++;
  return p;
}

int wmain(int argc, wchar_t **argv)
{
  DWORD64 policy = PROCESS_CREATION_MITIGATION_POLICY_BOTTOM_UP_ASLR_ALWAYS_OFF |
		   PROCESS_CREATION_MITIGATION_POLICY_HIGH_ENTROPY_ASLR_ALWAYS_OFF;
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
  STARTUPINFOEXW startup;
  PROCESS_INFORMATION process;
  wchar_t directory[MAX_PATH];
  wchar_t *command, *name;
  SIZE_T size = 0;
  HANDLE job;
  DWORD code = 1;

  if (argc < 2) {
    fwprintf(stderr, L"usage: %ls <program> [arguments...]\n", argv[0]);
    return 2;
  }

  if (!GetFullPathNameW(argv[1], MAX_PATH, directory, &name) || !name) {
    fwprintf(stderr, L"cannot resolve %ls (error %lu)\n", argv[1], GetLastError());
    return 2;
  }
  *name = L'\0';

  /* CreateProcessW may write to the command line: hand it a copy of our own tail. */
  command = _wcsdup(skip_token(GetCommandLineW()));
  if (!command) return 2;

  memset(&startup, 0, sizeof(startup));
  startup.StartupInfo.cb = sizeof(startup);
  InitializeProcThreadAttributeList(NULL, 1, 0, &size);
  startup.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(size);
  if (!startup.lpAttributeList ||
      !InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &size) ||
      !UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY,
				 &policy, sizeof(policy), NULL, NULL)) {
    fwprintf(stderr, L"cannot set up the mitigation policy (error %lu)\n", GetLastError());
    return 2;
  }

  memset(&limits, 0, sizeof(limits));
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  job = CreateJobObjectW(NULL, NULL);
  if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
    fwprintf(stderr, L"cannot create the job object (error %lu)\n", GetLastError());
    return 2;
  }

  /* Suspended, so that the child is inside of the job before it runs a single instruction. */
  if (!CreateProcessW(NULL, command, NULL, NULL, FALSE, EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED,
		      NULL, directory, &startup.StartupInfo, &process)) {
    fwprintf(stderr, L"cannot start %ls (error %lu)\n", argv[1], GetLastError());
    return 2;
  }
  if (!AssignProcessToJobObject(job, process.hProcess)) {
    fwprintf(stderr, L"cannot put the child into the job (error %lu)\n", GetLastError());
    TerminateProcess(process.hProcess, 2);
    return 2;
  }

  wprintf(L"started pid %lu with bottom-up ASLR off\n", process.dwProcessId);
  fflush(stdout);
  ResumeThread(process.hThread);
  WaitForSingleObject(process.hProcess, INFINITE);
  GetExitCodeProcess(process.hProcess, &code);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  CloseHandle(job);
  DeleteProcThreadAttributeList(startup.lpAttributeList);
  free(startup.lpAttributeList);
  free(command);
  return (int)code;
}
