#pragma once
#include "pdf.h"
#include <exception>

namespace pdf {
struct DecodePool {
    PTP_POOL pool = nullptr;
    TP_CALLBACK_ENVIRON environment{};
    DecodePool() {
        InitializeThreadpoolEnvironment(&environment);
        pool = CreateThreadpool(nullptr);
        if (pool) {
            SetThreadpoolThreadMaximum(pool, 16);
            unsigned minimum =
                std::min(8u, std::max(1u, unsigned(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)) / 2));
            if (!SetThreadpoolThreadMinimum(pool, minimum)) {
                CloseThreadpool(pool);
                pool = nullptr;
            } else
                SetThreadpoolCallbackPool(&environment, pool);
        }
    }
    ~DecodePool() {
        DestroyThreadpoolEnvironment(&environment);
        if (pool)
            CloseThreadpool(pool);
    }
    DecodePool(const DecodePool&) = delete;
    DecodePool& operator=(const DecodePool&) = delete;
};
inline DecodePool& decode_pool() {
    static DecodePool pool;
    return pool;
}
// A bounded number of callbacks share the Windows pool. The calling thread also
// works, and all callbacks finish before captured buffers can leave this scope.
// Worker failures (including cancellation) are rethrown on the calling thread.
template <typename Function>
void parallel_for(size_t count, size_t grain, unsigned workers, Function&& function) {
    if (!workers)
        workers = std::min(8u, std::max(1u, unsigned(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)) / 2));
    workers = unsigned(std::min<size_t>(std::min(workers, 16u), count / grain));
    if (workers < 2) {
        for (size_t i = 0; i < count; ++i)
            function(i);
        return;
    }
    struct Work {
        Function& function;
        size_t count, grain;
        std::atomic<size_t> next = 0;
        std::atomic<bool> failed = false;
        std::exception_ptr error;
    } work{function, count, grain};
    auto run = [](Work& state) {
        try {
            while (!state.failed.load(std::memory_order_relaxed)) {
                size_t first = state.next.fetch_add(state.grain, std::memory_order_relaxed);
                if (first >= state.count)
                    break;
                size_t end = std::min(state.count, first + state.grain);
                for (size_t i = first; i < end; ++i)
                    state.function(i);
            }
        } catch (...) {
            if (!state.failed.exchange(true, std::memory_order_relaxed))
                state.error = std::current_exception();
        }
    };
    auto callback = [](PTP_CALLBACK_INSTANCE, void* context, PTP_WORK) {
        // The captureless runner has no state and can be reconstructed here.
        decltype(run){}(*static_cast<Work*>(context));
    };
    // The default pool ramps up too slowly for short CPU-bound decode batches.
    // Keep a small dedicated pool ready, shared across images and renderers.
    PTP_WORK task = CreateThreadpoolWork(callback, &work, &decode_pool().environment);
    if (!task) {
        for (size_t i = 0; i < count; ++i)
            function(i);
        return;
    }
    for (unsigned i = 1; i < workers; ++i)
        SubmitThreadpoolWork(task);
    run(work);
    WaitForThreadpoolWorkCallbacks(task, FALSE);
    CloseThreadpoolWork(task);
    if (work.error)
        std::rethrow_exception(work.error);
}
} // namespace pdf
