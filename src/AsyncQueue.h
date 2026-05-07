#pragma once
#include "precomp.h"

// Thread-safe, bounded FIFO queue of serialized JSON payload strings.
// Producer: IIS request threads (Push).
// Consumer: single background worker thread (Pop).
class AsyncQueue {
public:
    static const size_t kMaxSize = 5000;

    AsyncQueue();
    ~AsyncQueue();

    // Enqueue a payload. If the queue is full, the oldest entry is dropped.
    void Push(std::string payload);

    // Block until a payload is available or timeoutMs elapses.
    // Returns true and fills 'out' on success; false on timeout or shutdown.
    bool Pop(std::string& out, DWORD timeoutMs);

    // Signal the consumer to drain and exit.
    void Shutdown();

    bool IsShutdown() const { return shutdown_.load(std::memory_order_relaxed); }

private:
    std::queue<std::string> queue_;
    mutable std::mutex      mutex_;
    HANDLE                  semaphore_;
    std::atomic<bool>       shutdown_;
};
