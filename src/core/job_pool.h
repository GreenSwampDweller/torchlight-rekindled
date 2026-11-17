// PORT: fork-join work on the cores the game's thread leaves idle. The game's thread hands out a
// range, takes slices of it itself, and returns once every slice is done; nothing runs on the
// workers outside such a call. What a slice may touch is the caller's business: per-item data
// only, no random draws, nothing OGRE shares between objects.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace tl::core {

class JobPool {
public:
    /// `workers` threads besides the caller's; 0 runs every range on the caller.
    explicit JobPool(unsigned workers);
    ~JobPool();
    JobPool(const JobPool&) = delete;
    JobPool& operator=(const JobPool&) = delete;

    unsigned workers() const { return static_cast<unsigned>(threads_.size()); }

    /// `run(begin, end)` over [0, count) in slices of `grain`, on the workers and the caller;
    /// returns when all are done. Not re-entrant, and only from the thread that made the pool.
    void parallel_for(size_t count, size_t grain, const std::function<void(size_t, size_t)>& run);

    /// The same range handed to the workers alone: returns at once, and the caller goes on with
    /// work of its own until `finish()`, which takes what slices are left and waits for the
    /// rest. One range at a time: nothing else may be handed out before `finish()`.
    void start(size_t count, size_t grain, std::function<void(size_t, size_t)> run);
    void finish();

private:
    void work(unsigned index);
    void take_slices();

    std::vector<std::thread> threads_;
    std::mutex lock_;
    std::condition_variable wake_;
    std::condition_variable done_;
    // The range handed out; set under `lock_` before a round starts.
    std::function<void(size_t, size_t)> run_;
    bool started_ = false;  // between start() and finish()
    size_t count_ = 0;
    size_t grain_ = 1;
    std::atomic<size_t> next_{0};
    unsigned busy_ = 0;             // workers inside the current range
    unsigned long long round_ = 0;  // ranges handed out, so a worker takes each once
    bool stopping_ = false;
};

/// The game's pool: `prepare_worker_thread` places each worker (platform.h). Made on first use
/// with `set_job_workers`' count (default 1).
JobPool& job_pool();
/// How many workers the game's pool has, before its first use (`--workers`).
void set_job_workers(unsigned workers);

}  // namespace tl::core
