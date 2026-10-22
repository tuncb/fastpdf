#pragma once
#include "render.h"
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>

namespace pdf {
struct SearchOptions {
    bool match_case = false, whole_words = false, regex = false;
    int page = -1;
    bool operator==(const SearchOptions&) const = default;
};
struct SearchPosition {
    size_t span = SIZE_MAX, character = 0;
};
struct SearchText {
    std::wstring text;
    std::vector<SearchPosition> positions;
};
struct SearchPiece {
    size_t span = 0, first = 0, count = 0;
};
struct SearchMatch {
    int page = 0;
    size_t offset = 0;
    std::wstring snippet;
    std::vector<SearchPiece> pieces;
};
// Keep each page's allocation intact when it moves from the worker to the UI.
struct SearchBatch {
    size_t first = 0;
    std::vector<SearchMatch> matches;
};
struct SearchResults {
    std::vector<SearchBatch> batches;
    size_t count = 0;
};
void append_search_results(SearchResults& results, std::vector<SearchMatch> matches);
const SearchMatch& search_result(const SearchResults& results, size_t index);
const SearchBatch* search_result_page(const SearchResults& results, int page);
SearchText search_text(const RenderedPage& page);
std::vector<SearchMatch> search_matches(const SearchText& text, std::wstring_view query,
                                        const SearchOptions& options, int page = 0);
std::vector<Rect> search_rects(const RenderedPage& page, const SearchMatch& match);

struct SearchRequest {
    uint64_t document = 0, ticket = 0;
    std::wstring path, password, query;
    SearchOptions options;
};
struct SearchUpdate {
    uint64_t document = 0, ticket = 0;
    int checked = 0, total = 0;
    bool complete = false, has_text = false;
    std::wstring error;
    std::vector<SearchMatch> matches;
};
struct SearchWorker {
    std::mutex mutex;
    std::condition_variable ready;
    std::optional<SearchRequest> request;
    std::deque<SearchUpdate> updates;
    size_t queued_matches = 0;
    bool notified = false;
    SearchResults retired;
    std::atomic<bool> cancel = false, stop = false;
    std::thread thread;
    HWND window = nullptr;
    UINT message = 0;
};
void run_search_worker(SearchWorker& worker);
void submit_search(SearchWorker& worker, SearchRequest request);
void cancel_search(SearchWorker& worker);
// Cancel before retiring the current results, and retire before submitting the next query.
void retire_search_results(SearchWorker& worker, SearchResults& results);
bool take_search_update(SearchWorker& worker, uint64_t document, uint64_t ticket, SearchUpdate& update);
bool finish_search_updates(SearchWorker& worker);
void stop_search_worker(SearchWorker& worker);
} // namespace pdf
