#pragma once
#include <cstdio>
#include <cstdarg>
#include <mutex>
#include <windows.h>

namespace tankaq
{

inline void Log(const char* fmt, ...)
{
    static std::mutex m;
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    _vsnprintf_s(buf, _TRUNCATE, fmt, args);
    va_end(args);

    std::lock_guard<std::mutex> lock(m);
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");
    static FILE* f = nullptr;
    if (!f)
    {
        char name[64];
        sprintf_s(name, "tankaq_log_%lu.txt", GetCurrentProcessId());
        fopen_s(&f, name, "w");
    }
    if (f)
    {
        fprintf(f, "%s\n", buf);
        fflush(f);
    }
}

} // namespace tankaq
