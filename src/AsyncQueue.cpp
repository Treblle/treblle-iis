#include "precomp.h"
#include "AsyncQueue.h"

AsyncQueue::AsyncQueue()
    : semaphore_(CreateSemaphoreW(nullptr, 0, static_cast<LONG>(kMaxSize * 2), nullptr))
    , hShutdown_(CreateEventW(nullptr, TRUE, FALSE, nullptr))  // manual-reset, initially unset
    , shutdown_(false) {
    // If handle creation fails, Push/Pop will see null handles and do nothing
    // rather than crashing — WaitForMultipleObjects rejects null handles so
    // we guard it in Pop() below.
}

AsyncQueue::~AsyncQueue() {
    if (hShutdown_) CloseHandle(hShutdown_);
    if (semaphore_) CloseHandle(semaphore_);
}

void AsyncQueue::Push(std::string payload) {
    if (shutdown_.load(std::memory_order_relaxed)) return;
    if (!semaphore_) return;

    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.size() >= kMaxSize)
        queue_.pop(); // drop oldest to keep memory bounded

    queue_.push(std::move(payload));
    ReleaseSemaphore(semaphore_, 1, nullptr);
}

bool AsyncQueue::Pop(std::string& out, DWORD timeoutMs) {
    if (!semaphore_ || !hShutdown_) return false;
    HANDLE handles[2] = { semaphore_, hShutdown_ };
    DWORD wait = WaitForMultipleObjects(2, handles, FALSE, timeoutMs);

    if (wait == WAIT_OBJECT_0) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) return false;
        out = std::move(queue_.front());
        queue_.pop();
        return true;
    }

    // WAIT_OBJECT_0+1 = shutdown event, WAIT_TIMEOUT, or error — all mean "stop"
    return false;
}

void AsyncQueue::Shutdown() {
    shutdown_.store(true, std::memory_order_release);
    SetEvent(hShutdown_); // wake the consumer immediately
}
