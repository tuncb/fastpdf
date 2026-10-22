#include "search.h"
#include <chrono>
#include <iostream>

using namespace pdf;
using Clock = std::chrono::steady_clock;
static void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
template <typename F> static void wait_for(F ready, const char* message) {
    const auto until = Clock::now() + std::chrono::seconds(20);
    while (!ready()) {
        check(Clock::now() < until, message);
        Sleep(1);
    }
}
int wmain(int argc, wchar_t** argv) {
    SearchWorker worker;
    worker.window = CreateWindowExW(0, L"STATIC", L"Search worker test", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                    GetModuleHandleW(nullptr), nullptr);
    worker.message = WM_APP + 1;
    worker.thread = std::thread(run_search_worker, std::ref(worker));
    try {
        check(argc == 2 && worker.window, "Missing fixture or message window");
        const auto submit = [&](uint64_t ticket, const wchar_t* query) {
            submit_search(worker, {1, ticket, argv[1], L"", query, {}});
        };
        const auto queued = [&] {
            std::lock_guard lock(worker.mutex);
            return !worker.updates.empty();
        };
        submit(1, L"word");
        wait_for(queued, "First large page did not publish");
        Sleep(100); // Leave the consumer idle to exercise producer backpressure.
        {
            std::lock_guard lock(worker.mutex);
            check(worker.updates.size() == 1 && worker.queued_matches == 9000,
                  "An oversized page must publish alone without growing the queue");
        }
        MSG message{};
        int notifications = 0;
        while (PeekMessageW(&message, worker.window, worker.message, worker.message, PM_REMOVE))
            ++notifications;
        check(notifications == 1, "The worker floods the UI message queue");
        cancel_search(worker);
        submit(2, L"word");
        SearchResults results;
        bool complete = false;
        wait_for(
            [&] {
                SearchUpdate update;
                while (take_search_update(worker, 1, 2, update)) {
                    check(update.error.empty(), "Search failed");
                    complete = update.complete;
                    append_search_results(results, std::move(update.matches));
                    update = {};
                }
                finish_search_updates(worker);
                return complete;
            },
            "Search did not finish after cancelling a blocked publisher");
        check(results.count == 85800 && results.batches.size() == 241, "Search lost page batches");
        check(search_result(results, 8999).page == 0 && search_result(results, 9000).page == 1 &&
                  search_result(results, 85799).page == 240,
              "Result order changed at a page boundary");
        cancel_search(worker);
        retire_search_results(worker, results);
        check(!results.count && results.batches.empty(), "Retirement kept UI ownership");
        for (uint64_t ticket = 3; ticket < 23; ++ticket) {
            submit(ticket, L"word");
            Sleep(2);
            SearchUpdate update;
            if (take_search_update(worker, 1, ticket, update))
                append_search_results(results, std::move(update.matches));
            cancel_search(worker);
            retire_search_results(worker, results);
        }
        submit(23, L"absent");
        complete = false;
        wait_for(
            [&] {
                SearchUpdate update;
                while (take_search_update(worker, 1, 23, update)) {
                    check(update.matches.empty() && update.error.empty(), "A stale query leaked results");
                    complete = update.complete;
                }
                finish_search_updates(worker);
                return complete;
            },
            "Latest query was lost during rapid replacements");
        submit(24, L"word");
        wait_for(queued, "Shutdown fixture did not fill the queue");
        const auto start = Clock::now();
        stop_search_worker(worker);
        check(Clock::now() - start < std::chrono::seconds(2), "Shutdown blocked on the full queue");
        check(worker.updates.empty() && !worker.retired.count, "Shutdown left queued result data");
        DestroyWindow(worker.window);
        std::cout << "Search backpressure, notifications, retirement, replacements, and shutdown passed\n";
        return 0;
    } catch (const std::exception& error) {
        stop_search_worker(worker);
        DestroyWindow(worker.window);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
