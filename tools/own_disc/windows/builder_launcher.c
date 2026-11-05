/* builder.exe: the one program a player starts on Windows.

   It holds no logic of the builder. It finds the folder it lies in, and runs the Python that lies beside it with
   the builder's front end and the arguments it was given:

       <folder>\python\python.exe -B <folder>\tools\own_disc\builder.py <arguments>

   with BUILDER_PACKAGE_ROOT, BUILDER_PYTHON and BUILDER_HOST_TOOLS set to this package's own folders, PYTHONUTF8=1
   (UTF-8 mode for the front end and for every tool it starts; the one switch, so no -X utf8 beside it) and
   PYTHONNOUSERSITE=1 (a package folder in the player's own profile is not put on the path, for the front end and for
   every tool it starts), so nothing
   installed on the machine is used and nothing on it is changed (no registry, no PATH, no administrator rights). (The
   shader tools name the compiler library's folder themselves for the programs they start.)
   Its exit code is the front end's. Started by a double click (no arguments) it prints the usage and waits for a
   key, so the window does not close before it can be read.

   Build (llvm-mingw, any host):  x86_64-w64-mingw32-clang -O2 -municode -o builder.exe builder_launcher.c
   Nothing here comes from the game. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#define MAX_COMMAND 32768     /* the limit of CreateProcessW's command line */

/* Appends one argument, quoted by the rules the Microsoft C runtime parses (backslashes before a quote doubled). */
static int append_argument(wchar_t *out, size_t room, size_t *at, const wchar_t *arg) {
    size_t i = *at;
#define PUT(c) do { if (i + 1 >= room) return 0; out[i++] = (c); } while (0)
    if (i) PUT(L' ');
    PUT(L'"');
    for (const wchar_t *p = arg;; p++) {
        size_t slashes = 0;
        while (*p == L'\\') { p++; slashes++; }
        if (*p == 0) {                       /* before the closing quote: every backslash doubled */
            for (size_t k = 0; k < slashes * 2; k++) PUT(L'\\');
            break;
        }
        if (*p == L'"') {                    /* before a quote: doubled, and one more for the quote */
            for (size_t k = 0; k < slashes * 2 + 1; k++) PUT(L'\\');
        } else {
            for (size_t k = 0; k < slashes; k++) PUT(L'\\');
        }
        PUT(*p);
    }
    PUT(L'"');
    out[i] = 0;
    *at = i;
    return 1;
#undef PUT
}

static void wait_for_a_key(void) {
    fwprintf(stderr, L"\nPress Enter to close this window.\n");
    (void)getwchar();
}

int wmain(int argc, wchar_t **argv) {
    static wchar_t self[MAX_COMMAND], folder[MAX_COMMAND], python[MAX_COMMAND], script[MAX_COMMAND], tools[MAX_COMMAND];
    static wchar_t command[MAX_COMMAND];
    DWORD n = GetModuleFileNameW(NULL, self, MAX_COMMAND);
    if (n == 0 || n >= MAX_COMMAND) {
        fwprintf(stderr, L"builder: cannot find its own folder (error %lu)\n", GetLastError());
        return 70;
    }
    wcscpy(folder, self);
    wchar_t *slash = wcsrchr(folder, L'\\');
    if (!slash) {
        fwprintf(stderr, L"builder: its own path has no folder: %ls\n", self);
        return 70;
    }
    *slash = 0;
    if (_snwprintf(python, MAX_COMMAND, L"%ls\\python\\python.exe", folder) < 0 ||
        _snwprintf(script, MAX_COMMAND, L"%ls\\tools\\own_disc\\builder.py", folder) < 0 ||
        _snwprintf(tools, MAX_COMMAND, L"%ls\\host-tools", folder) < 0) {
        fwprintf(stderr, L"builder: its folder's path is too long: %ls\n", folder);
        return 70;
    }
    if (GetFileAttributesW(python) == INVALID_FILE_ATTRIBUTES || GetFileAttributesW(script) == INVALID_FILE_ATTRIBUTES) {
        fwprintf(stderr, L"builder: the package is not complete. Missing beside builder.exe:\n  %ls\n  or\n  %ls\n"
                         L"Unpack the whole package into one folder and start builder.exe from there.\n", python, script);
        if (argc <= 1) wait_for_a_key();
        return 69;
    }
    if (!SetEnvironmentVariableW(L"BUILDER_PACKAGE_ROOT", folder) || !SetEnvironmentVariableW(L"BUILDER_PYTHON", python) ||
        !SetEnvironmentVariableW(L"BUILDER_HOST_TOOLS", tools) || !SetEnvironmentVariableW(L"PYTHONUTF8", L"1") ||
        !SetEnvironmentVariableW(L"PYTHONHOME", NULL) || !SetEnvironmentVariableW(L"PYTHONPATH", NULL) ||
        !SetEnvironmentVariableW(L"PYTHONNOUSERSITE", L"1")) {
        fwprintf(stderr, L"builder: cannot set its environment (error %lu)\n", GetLastError());
        return 70;
    }
    size_t at = 0;
    const wchar_t *fixed[] = {python, L"-B", script};
    for (int i = 0; i < 3; i++)
        if (!append_argument(command, MAX_COMMAND, &at, fixed[i])) goto too_long;
    for (int i = 1; i < argc; i++)
        if (!append_argument(command, MAX_COMMAND, &at, argv[i])) goto too_long;
    STARTUPINFOW start;
    PROCESS_INFORMATION child;
    ZeroMemory(&start, sizeof start);
    start.cb = sizeof start;
    if (!CreateProcessW(python, command, NULL, NULL, TRUE, 0, NULL, NULL, &start, &child)) {
        fwprintf(stderr, L"builder: cannot start %ls (error %lu)\n", python, GetLastError());
        if (argc <= 1) wait_for_a_key();
        return 71;
    }
    WaitForSingleObject(child.hProcess, INFINITE);
    DWORD code = 70;
    GetExitCodeProcess(child.hProcess, &code);
    CloseHandle(child.hProcess);
    CloseHandle(child.hThread);
    if (argc <= 1) wait_for_a_key();     /* a double click: keep the usage on the screen */
    return (int)code;
too_long:
    fwprintf(stderr, L"builder: the command line is too long (Windows allows %d characters)\n", MAX_COMMAND - 1);
    return 64;
}
