#pragma once
#include <cstddef>
#include <cstdint>

// Single source of truth for all tuneable constants.
// Include this directly in .cpp files that need it — do NOT add to precomp.h.
namespace TreblleConst {
    // SDK identity sent in every payload
    constexpr const char* kSdkName         = "iis";
    constexpr const char* kSdkVersion      = "1.0.0";
    constexpr int         kApiVersion      = 20;

    // Body capture
    constexpr size_t   kMaxBodyBytes       = 2 * 1024 * 1024;  // 2 MB hard cap
    constexpr uint32_t kBodyReadBuffer     = 8192;              // ReadEntityBody chunk
    constexpr uint32_t kMultipartWindow    = 16384;             // bytes read for multipart scan

    // Data masking
    constexpr size_t kMaskSizeLimit        = 500 * 1024;        // skip masking above this size
    constexpr int    kMaxMaskDepth         = 64;                // max JSON nesting before bail-out

    // Background queue
    constexpr size_t kQueueMaxSize         = 5000;

    // WinHTTP (all in milliseconds)
    constexpr uint32_t kHttpTimeoutMs      = 5000;

    // Shutdown: how long the factory waits for the worker to drain.
    // Must exceed kHttpTimeoutMs so the worker can finish its last send before Terminate() gives up.
    constexpr uint32_t kShutdownDrainMs    = 8000;
}
