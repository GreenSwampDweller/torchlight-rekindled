#include "core/job_pool.h"

#include "core/platform.h"

#include <algorithm>

namespace tl::core {

JobPool::JobPool(unsigned workers) {
    threads_.reserve(workers);
    for (unsigned i = 0; i < workers; ++i) threads_.emplace_back([this, i] { work(i); });
}

JobPool::~JobPool() {
    {
        const std::lock_guard<std::mutex> hold(lock_);
        stopping_ = true;
    }
    wake_.notify_all();
    for (std::thread& thread : threads_) thread.join();
}

void JobPool::take_slices() {
    for (;;) {
        const size_t begin = next_.fetch_add(grain_, std::memory_order_relaxed);
        if (begin >= count_) return;
        run_(begin, std::min(begin + grain_, count_));
    }
}

void JobPool::parallel_for(size_t count, size_t grain,
                           const std::function<void(size_t, size_t)>& run) {
    if (count == 0) return;
    grain = std::max<size_t>(grain, 1);
    if (threads_.empty() || count <= grain) {
        for (size_t begin = 0; begin < count; begin += grain)
            run(begin, std::min(begin + grain, count));
        return;
    }
    start(count, grain, run);
    finish();
}

void JobPool::start(size_t count, size_t grain, std::function<void(size_t, size_t)> run) {
    finish();
    if (count == 0) return;
    {
        const std::lock_guard<std::mutex> hold(lock_);
        run_ = std::move(run);
        count_ = count;
        grain_ = std::max<size_t>(grain, 1);
        next_.store(0, std::memory_order_relaxed);
        busy_ = static_cast<unsigned>(threads_.size());
        ++round_;
    }
    started_ = true;
    wake_.notify_all();
}

void JobPool::finish() {
    if (!started_) return;
    started_ = false;
    take_slices();
    std::unique_lock<std::mutex> hold(lock_);
    done_.wait(hold, [this] { return busy_ == 0; });
    run_ = nullptr;
}

void JobPool::work(unsigned index) {
    prepare_worker_thread(index);
    unsigned long long seen = 0;
    std::unique_lock<std::mutex> hold(lock_);
    for (;;) {
        wake_.wait(hold, [&] { return stopping_ || round_ != seen; });
        if (stopping_) return;
        seen = round_;
        hold.unlock();
        take_slices();
        hold.lock();
        if (--busy_ == 0) done_.notify_one();
    }
}

namespace {
unsigned g_job_workers = 1;
}  // namespace

void set_job_workers(unsigned workers) { g_job_workers = workers; }

JobPool& job_pool() {
    static JobPool pool(g_job_workers);
    return pool;
}

}  // namespace tl::core
