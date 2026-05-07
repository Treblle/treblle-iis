#include "precomp.h"
#include "AsyncQueue.h"

AsyncQueue::AsyncQueue()
    : semaphore_(CreateSemaphoreW(nullptr, 0, static_cast<LONG>(kMaxSize * 2), nullptr))
    , shutdown_(false) {
}

AsyncQueue::~AsyncQueue() {
    if (semaphore_) CloseHandle(semaphore_);
}

void AsyncQueue::Push(std::string payload) {
    if (shutdown_.load(std::memory_order_relaxed)) return;

    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.size() >= kMaxSize)
        queue_.pop(); // drop oldest to keep memory bounded

    queue_.push(std::move(payload));
    ReleaseSemaphore(semaphore_, 1, nullptr);
}

bool AsyncQueue::Pop(std::string& out, DWORD timeoutMs) {
    DWORD wait = WaitForSingleObject(semaphore_, timeoutMs);
    if (wait != WAIT_OBJECT_0) return false;

    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.empty()) return false;
    out = std::move(queue_.front());
    queue_.pop();
    return true;
}

void AsyncQueue::Shutdown() {
    shutdown_.store(true, std::memory_order_release);
    // Wake the consumer so it can observe the shutdown flag
    ReleaseSemaphore(semaphore_, 1, nullptr);
}
