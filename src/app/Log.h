#pragma once

#include <filesystem>

#include <windows.h>

void logInit(const std::filesystem::path& file);
void logf(const char* fmt, ...);

// Evaluates an HRESULT expression, logs file:line and the code on failure, and yields the HRESULT.
// Usage: if (FAILED(HR_CHECK(device->CreateTexture2D(...)))) return false;
#define HR_CHECK(expr)                                                                  \
    ([&]() -> HRESULT {                                                                 \
        HRESULT hr_ = (expr);                                                           \
        if (FAILED(hr_)) logf("%s:%d %s failed: 0x%08X", __FILE__, __LINE__, #expr,     \
                              static_cast<unsigned>(hr_));                              \
        return hr_;                                                                     \
    }())
