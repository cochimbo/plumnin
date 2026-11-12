#include "crash.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
// dbghelp.h needs windows.h first.
#include <dbghelp.h>
#ifdef _MSC_VER
#pragma comment(lib, "dbghelp.lib")  // for whatever links this file, frontend_tests too
#endif
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__GLIBC__)
#include <execinfo.h>
#endif
#endif

#ifndef PLUMNIN_VERSION
#define PLUMNIN_VERSION "dev"
#endif

namespace plumnin {

namespace {

// Fixed buffers: when the handler runs the heap may be what broke, so nothing in it
// allocates.
char gDir[1024];
char gGame[1024];
bool gInstalled = false;

const char* const kPending = "last-crash.txt";  // the report nobody has been told about

void copyTo(char* out, size_t size, const std::string& s) {
    const size_t n = s.size() < size - 1 ? s.size() : size - 1;
    std::memcpy(out, s.data(), n);
    out[n] = 0;
}

// The report's path without its extension, from the time of the crash.
void reportBase(char* out, size_t size) {
    const std::time_t now = std::time(nullptr);
    std::tm t{};
#ifdef _WIN32
    localtime_s(&t, &now);
#else
    localtime_r(&now, &t);
#endif
    std::snprintf(out, size, "%splumnin-%04d%02d%02d-%02d%02d%02d", gDir, t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                  t.tm_hour, t.tm_min, t.tm_sec);
}

#ifdef _WIN32

void writeAll(HANDLE f, const char* text) {
    DWORD written = 0;
    WriteFile(f, text, static_cast<DWORD>(std::strlen(text)), &written, nullptr);
}

// The report and the pending note; dump is the minidump's path, or "" for none.
void writeReport(const char* base, const char* what, const char* where, const char* dump) {
    char path[1200];
    std::snprintf(path, sizeof path, "%s.txt", base);
    HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    char text[4096];
    std::snprintf(text, sizeof text,
                  "plumnin %s closed because of a crash.\r\n\r\nWhat happened: %s\r\nWhere: %s\r\nGame: %s\r\n%s%s%s",
                  PLUMNIN_VERSION, what, where, gGame[0] ? gGame : "(none)", dump[0] ? "Dump: " : "", dump,
                  dump[0] ? "\r\n" : "");
    writeAll(f, text);
    CloseHandle(f);
    char pending[1200];
    std::snprintf(pending, sizeof pending, "%s%s", gDir, kPending);
    HANDLE p = CreateFileA(pending, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (p == INVALID_HANDLE_VALUE) return;
    writeAll(p, path);
    CloseHandle(p);
}

LONG WINAPI onException(EXCEPTION_POINTERS* info) {
    static volatile LONG once = 0;
    if (InterlockedExchange(&once, 1)) return EXCEPTION_CONTINUE_SEARCH;
    char base[1100];
    reportBase(base, sizeof base);

    char dump[1200];
    std::snprintf(dump, sizeof dump, "%s.dmp", base);
    HANDLE f = CreateFileA(dump, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    bool dumped = false;
    if (f != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei{GetCurrentThreadId(), info, FALSE};
        dumped = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f,
                                   static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory |
                                                              MiniDumpScanMemory),
                                   &mei, nullptr, nullptr);
        CloseHandle(f);
        if (!dumped) DeleteFileA(dump);
    }

    const EXCEPTION_RECORD* r = info->ExceptionRecord;
    char what[256];
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2) {
        std::snprintf(what, sizeof what, "invalid memory access (%s at %p)",
                      r->ExceptionInformation[0] == 0 ? "read" : r->ExceptionInformation[0] == 1 ? "write" : "execute",
                      reinterpret_cast<void*>(r->ExceptionInformation[1]));
    } else {
        std::snprintf(what, sizeof what, "exception 0x%08lX", static_cast<unsigned long>(r->ExceptionCode));
    }
    // Where: the module and the offset in it, which stays the same from run to run.
    char where[600];
    HMODULE module = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(r->ExceptionAddress), &module) &&
        GetModuleFileNameA(module, name, sizeof name)) {
        const char* slash = std::strrchr(name, '\\');
        std::snprintf(where, sizeof where, "%s+0x%llX", slash ? slash + 1 : name,
                      static_cast<unsigned long long>(static_cast<const char*>(r->ExceptionAddress) -
                                                      reinterpret_cast<const char*>(module)));
    } else {
        std::snprintf(where, sizeof where, "%p", r->ExceptionAddress);
    }
    writeReport(base, what, where, dumped ? dump : "");
    return EXCEPTION_CONTINUE_SEARCH;
}

#else

void writeAll(int fd, const char* text) {
    size_t left = std::strlen(text);
    while (left > 0) {
        const ssize_t n = write(fd, text, left);
        if (n <= 0) return;
        text += n;
        left -= static_cast<size_t>(n);
    }
}

void writeReport(const char* base, const char* what, const char* where, const char*) {
    char path[1200];
    std::snprintf(path, sizeof path, "%s.txt", base);
    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    char text[4096];
    std::snprintf(text, sizeof text, "plumnin %s closed because of a crash.\n\nWhat happened: %s\nWhere: %s\nGame: %s\n",
                  PLUMNIN_VERSION, what, where, gGame[0] ? gGame : "(none)");
    writeAll(fd, text);
#if defined(__GLIBC__)
    writeAll(fd, "\nStack:\n");
    void* frames[64];
    backtrace_symbols_fd(frames, backtrace(frames, 64), fd);
#endif
    close(fd);
    char pending[1200];
    std::snprintf(pending, sizeof pending, "%s%s", gDir, kPending);
    const int p = open(pending, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (p < 0) return;
    writeAll(p, path);
    close(p);
}

#endif

void onSignal(int sig) {
    static volatile std::sig_atomic_t once = 0;
    if (!once) {
        once = 1;
        const char* what = sig == SIGSEGV ? "invalid memory access (SIGSEGV)"
                           : sig == SIGFPE ? "arithmetic error (SIGFPE)"
                           : sig == SIGILL ? "illegal instruction (SIGILL)"
                           : sig == SIGABRT ? "the program aborted (SIGABRT)"
                                            : "unexpected signal";
        char base[1100];
        reportBase(base, sizeof base);
        writeReport(base, what, "-", "");
    }
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}

// An exception nobody caught: its message says more than the abort that follows.
void onTerminate() {
    static bool once = false;
    if (!once) {
        once = true;
        char what[600] = "uncaught C++ exception";
        try {
            if (std::exception_ptr e = std::current_exception()) std::rethrow_exception(e);
        } catch (const std::exception& e) {
            std::snprintf(what, sizeof what, "uncaught C++ exception: %s", e.what());
        } catch (...) {
        }
        char base[1100];
        reportBase(base, sizeof base);
        writeReport(base, what, "-", "");
    }
    std::signal(SIGABRT, SIG_DFL);
    std::abort();
}

}  // namespace

void installCrashHandler(const std::string& dir) {
    std::string d = dir;
    if (!d.empty() && d.back() != '/' && d.back() != '\\') d += '/';
#ifdef _WIN32
    CreateDirectoryA(d.c_str(), nullptr);
#else
    mkdir(d.c_str(), 0755);
#endif
    copyTo(gDir, sizeof gDir, d);
    if (gInstalled) return;
    gInstalled = true;
#ifdef _WIN32
    SetUnhandledExceptionFilter(onException);
#else
    for (int sig : {SIGSEGV, SIGBUS, SIGFPE, SIGILL}) std::signal(sig, onSignal);
#endif
    std::signal(SIGABRT, onSignal);
    std::set_terminate(onTerminate);
}

void setCrashContext(const std::string& game) { copyTo(gGame, sizeof gGame, game); }

std::string takeCrashReport(const std::string& dir) {
    std::string d = dir;
    if (!d.empty() && d.back() != '/' && d.back() != '\\') d += '/';
    const std::string pending = d + kPending;
    std::ifstream in(pending, std::ios::binary);
    if (!in) return {};
    std::stringstream s;
    s << in.rdbuf();
    in.close();
    std::remove(pending.c_str());
    return s.str();
}

const char* appVersion() { return PLUMNIN_VERSION; }

std::string appTitle() { return std::string("plumnin ") + PLUMNIN_VERSION; }

}  // namespace plumnin
