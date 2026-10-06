#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>

#include "app/Settings.h"

// Settings snapshot shared between the UI thread (writer) and the render thread (reader).
// The render thread polls version() cheaply and only locks when it changed.
class SharedSettings {
public:
    explicit SharedSettings(const Settings& initial) : s_(initial) {}

    Settings get() const {
        std::lock_guard lock(m_);
        return s_;
    }

    void set(Settings s) {
        s.clamp();
        {
            std::lock_guard lock(m_);
            if (s == s_) return;
            s_ = s;
        }
        version_.fetch_add(1, std::memory_order_release);
    }

    uint64_t version() const { return version_.load(std::memory_order_acquire); }

private:
    mutable std::mutex m_;
    Settings s_;
    std::atomic<uint64_t> version_{1};
};
