#pragma once

#include <atomic>

// Written by the render thread, read by the settings UI. Relaxed atomics: display-only values.
struct RenderStats {
    std::atomic<float> fps{0};        // lens frames presented per second
    std::atomic<float> refreshHz{0};  // monitor refresh rate (the ceiling for fps)
    std::atomic<float> delayMs{0};    // desktop frame update -> our Present returned (smoothed)
};
