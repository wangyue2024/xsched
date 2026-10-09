#pragma once
/// Win32 -> POSIX compatibility so the tests_eval suites build and run on Linux
/// without touching the test sources (see run_tests.sh).

#include <dlfcn.h>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>

typedef void *HMODULE;
typedef void *FARPROC;

#ifndef MAX_PATH
#define MAX_PATH 4096
#endif

inline bool IsWinDriverPath(const char *value)
{
    if (value == nullptr) return false;
    size_t len = strlen(value);
    return len >= 10 && strcmp(value + len - 10, "nvcuda.dll") == 0;
}

/// The tests hard-code the Windows shim path (…\build\platforms\cuda\nvcuda.dll);
/// redirect it to the Linux shim: XSCHED_SHIM_PATH, else -DTEST_SHIM_PATH,
/// else libshimcuda.so.
inline HMODULE LoadLibraryA(const char *path)
{
    std::string resolved(path ? path : "");
    if (IsWinDriverPath(path)) {
        const char *shim = getenv("XSCHED_SHIM_PATH");
#ifdef TEST_SHIM_PATH
        if (shim == nullptr || shim[0] == '\0') shim = TEST_SHIM_PATH;
#endif
        resolved = (shim && shim[0]) ? shim : "libshimcuda.so";
    }
    return (HMODULE)dlopen(resolved.c_str(), RTLD_NOW | RTLD_GLOBAL);
}

inline FARPROC GetProcAddress(HMODULE module, const char *name)
{
    return (FARPROC)dlsym(module, name);
}

inline int FreeLibrary(HMODULE module)
{
    return dlclose((void *)module);
}

inline const char *GetLastError()
{
    const char *err = dlerror();
    return err ? err : "no error";
}

inline int SetEnvironmentVariableA(const char *name, const char *value)
{
    /// XSCHED_CUDA_LIB must be an absolute file path (XSched::FindLibrary), so
    /// Windows driver values are dropped instead of being exported as a soname.
    if (value == nullptr || value[0] == '\0' || IsWinDriverPath(value)) {
        return unsetenv(name) == 0 ? 1 : 0;
    }
    return setenv(name, value, 1) == 0 ? 1 : 0;
}

/// _putenv_s returns 0 (errno_t) on success.
inline int _putenv_s(const char *name, const char *value)
{
    if (value == nullptr || value[0] == '\0') return unsetenv(name);
    return SetEnvironmentVariableA(name, value) ? 0 : -1;
}

inline unsigned long GetEnvironmentVariableA(const char *name, char *buffer, unsigned long size)
{
    const char *value = getenv(name);
    if (buffer != nullptr && size > 0) buffer[0] = '\0';
    if (value == nullptr) return 0;
    if (buffer == nullptr || size == 0) return (unsigned long)strlen(value) + 1;
    strncpy(buffer, value, size - 1);
    buffer[size - 1] = '\0';
    return (unsigned long)strlen(buffer) + 1;
}
