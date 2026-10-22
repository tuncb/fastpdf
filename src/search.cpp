#include "search.h"
#include <cwctype>
#include <chrono>
#include <regex>
#include <cassert>

namespace pdf {
void append_search_results(SearchResults& results, std::vector<SearchMatch> matches) {
    if (matches.empty())
        return;
    const size_t count = matches.size();
    results.batches.push_back({results.count, std::move(matches)});
    results.count += count;
}
const SearchMatch& search_result(const SearchResults& results, size_t index) {
    assert(index < results.count);
    auto batch = std::upper_bound(results.batches.begin(), results.batches.end(), index,
                                  [](size_t at, const SearchBatch& value) { return at < value.first; });
    --batch;
    return batch->matches[index - batch->first];
}
const SearchBatch* search_result_page(const SearchResults& results, int page) {
    auto batch =
        std::lower_bound(results.batches.begin(), results.batches.end(), page,
                         [](const SearchBatch& value, int at) { return value.matches.front().page < at; });
    return batch != results.batches.end() && batch->matches.front().page == page ? &*batch : nullptr;
}
static bool space(wchar_t c) {
    return std::iswspace(c) != 0 || c == 0x00a0;
}
static void append(SearchText& out, wchar_t c, SearchPosition position) {
    if (c == 0x00ad)
        return;
    if (space(c)) {
        if (out.text.empty() || out.text.back() == L' ')
            return;
        c = L' ';
    }
    const wchar_t* expanded = nullptr;
    switch (c) {
    case 0xfb00:
        expanded = L"ff";
        break;
    case 0xfb01:
        expanded = L"fi";
        break;
    case 0xfb02:
        expanded = L"fl";
        break;
    case 0xfb03:
        expanded = L"ffi";
        break;
    case 0xfb04:
        expanded = L"ffl";
        break;
    case 0xfb05:
    case 0xfb06:
        expanded = L"st";
        break;
    }
    if (expanded) {
        while (*expanded) {
            out.text += *expanded++;
            out.positions.push_back(position);
        }
    } else {
        out.text += c;
        out.positions.push_back(position);
    }
}
SearchText search_text(const RenderedPage& page) {
    SearchText result;
    std::optional<Rect> previous;
    const auto to_page = inverse(page.page_matrix);
    for (size_t i = 0; i < page.text.size(); ++i) {
        const auto& span = page.text[i];
        const auto start = transform(to_page, {span.bounds.x0, span.bounds.y0});
        const auto end = transform(to_page, {span.bounds.x1, span.bounds.y1});
        const Rect bounds{std::min(start.x, end.x), std::min(start.y, end.y), std::max(start.x, end.x),
                          std::max(start.y, end.y)};
        if (previous && !result.text.empty()) {
            double h = std::max(1.0, previous->y1 - previous->y0);
            if (std::abs(bounds.y0 - previous->y0) > h * 0.55 || bounds.x0 - previous->x1 > h * 0.12)
                append(result, L' ', {});
        }
        for (size_t j = 0; j < span.text.size(); ++j)
            append(result, span.text[j], {i, j});
        previous = bounds;
    }
    return result;
}
static std::wstring fold(std::wstring text) {
    // Windows invariant casing does not depend on the process C locale.
    if (!text.empty()) {
        int count = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, text.data(), int(text.size()),
                                  nullptr, 0, nullptr, nullptr, 0);
        std::wstring lowered(size_t(std::max(0, count)), L'\0');
        if (count == int(text.size()) &&
            LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, text.data(), int(text.size()),
                          lowered.data(), count, nullptr, nullptr, 0))
            return lowered;
    }
    return text;
}
static bool word(wchar_t c) {
    WORD kind = 0;
    GetStringTypeW(CT_CTYPE1, &c, 1, &kind);
    return (kind & (C1_ALPHA | C1_DIGIT)) != 0 || c == L'_' || (c >= 0x0300 && c <= 0x036f) ||
           (c >= 0xd800 && c <= 0xdfff);
}
struct SearchPattern {
    SearchOptions options;
    std::wstring literal;
    std::optional<std::wregex> expression;
};
static SearchPattern compile_search(std::wstring_view query, const SearchOptions& options) {
    SearchPattern pattern;
    pattern.options = options;
    if (options.regex) {
        if (query.size() > 4095 || std::count(query.begin(), query.end(), L'(') > 128)
            throw std::runtime_error("Regular expression is too large. Use a shorter pattern.");
        pattern.expression.emplace();
        // Keep Unicode character classes and case matching independent of the user's locale.
        pattern.expression->imbue(std::locale("en-US"));
        auto flags = std::regex_constants::ECMAScript;
        if (!options.match_case)
            flags |= std::regex_constants::icase;
        std::wstring source(query);
        // Validate the user's expression before adding a boundary assertion.
        // Otherwise an unmatched '[' could consume brackets from that assertion.
        pattern.expression->assign(source, flags);
        if (options.whole_words) {
            source = L"(?:" + source + L")(?![[:alnum:]_\u0300-\u036f\\uD800-\\uDFFF])";
            pattern.expression->assign(source, flags);
        }
        return pattern;
    }
    SearchText normalized;
    for (auto c : query)
        append(normalized, c, {});
    while (!normalized.text.empty() && normalized.text.back() == L' ')
        normalized.text.pop_back();
    pattern.literal = options.match_case ? normalized.text : fold(normalized.text);
    return pattern;
}
struct SearchBudget {
    std::chrono::steady_clock::time_point deadline;
    const std::atomic<bool>* cancel = nullptr;
    size_t steps = 0;
};
static void check_budget(SearchBudget& budget) {
    if ((++budget.steps & 255) != 0)
        return;
    if (budget.cancel && *budget.cancel)
        throw std::runtime_error("Search cancelled.");
    if (std::chrono::steady_clock::now() > budget.deadline)
        throw std::runtime_error("Pattern took too long. Use a simpler regular expression.");
}
// Checked iterators stop backtracking; constant-time distance avoids rescanning
// the rest of a page for every match.
struct SearchIterator {
    using iterator_category = std::random_access_iterator_tag;
    using value_type = wchar_t;
    using difference_type = ptrdiff_t;
    using pointer = const wchar_t*;
    using reference = const wchar_t&;
    const wchar_t* cursor = nullptr;
    SearchBudget* budget = nullptr;
    reference operator[](difference_type offset) const {
        check_budget(*budget);
        return cursor[offset];
    }
};
inline const wchar_t& operator*(SearchIterator iterator) {
    check_budget(*iterator.budget);
    return *iterator.cursor;
}
inline SearchIterator& operator++(SearchIterator& iterator) {
    check_budget(*iterator.budget);
    ++iterator.cursor;
    return iterator;
}
inline SearchIterator operator++(SearchIterator& iterator, int) {
    const auto previous = iterator;
    ++iterator;
    return previous;
}
inline SearchIterator& operator--(SearchIterator& iterator) {
    check_budget(*iterator.budget);
    --iterator.cursor;
    return iterator;
}
inline SearchIterator operator--(SearchIterator& iterator, int) {
    const auto previous = iterator;
    --iterator;
    return previous;
}
inline bool operator==(SearchIterator left, SearchIterator right) {
    if (left.budget)
        check_budget(*left.budget);
    return left.cursor == right.cursor;
}
inline auto operator<=>(SearchIterator left, SearchIterator right) {
    return left.cursor <=> right.cursor;
}
inline ptrdiff_t operator-(SearchIterator left, SearchIterator right) {
    return left.cursor - right.cursor;
}
inline SearchIterator& operator+=(SearchIterator& iterator, ptrdiff_t distance) {
    check_budget(*iterator.budget);
    iterator.cursor += distance;
    return iterator;
}
inline SearchIterator& operator-=(SearchIterator& iterator, ptrdiff_t distance) {
    return iterator += -distance;
}
inline SearchIterator operator+(SearchIterator iterator, ptrdiff_t distance) {
    return iterator += distance;
}
inline SearchIterator operator+(ptrdiff_t distance, SearchIterator iterator) {
    return iterator += distance;
}
inline SearchIterator operator-(SearchIterator iterator, ptrdiff_t distance) {
    return iterator -= distance;
}
static std::vector<SearchMatch> find_matches(const SearchText& text, const SearchPattern& pattern, int page,
                                             const std::atomic<bool>* cancel = nullptr,
                                             std::vector<SearchMatch> matches = {}) {
    if (!pattern.expression && pattern.literal.empty())
        return matches;
    const auto haystack = pattern.expression || pattern.options.match_case ? text.text : fold(text.text);
    SearchBudget budget{std::chrono::steady_clock::now() + std::chrono::milliseconds(250), cancel};
    for (size_t next = 0; next < haystack.size();) {
        size_t at = 0, end = 0;
        if (cancel && *cancel)
            throw std::runtime_error("Search cancelled.");
        if (pattern.expression) {
            std::match_results<SearchIterator> result;
            auto flags = std::regex_constants::match_not_null;
            if (next)
                flags |= std::regex_constants::match_prev_avail;
            if (!std::regex_search(SearchIterator{haystack.data() + next, &budget},
                                   SearchIterator{haystack.data() + haystack.size(), &budget}, result,
                                   *pattern.expression, flags))
                break;
            at = size_t(result[0].first.cursor - haystack.data());
            end = size_t(result[0].second.cursor - haystack.data());
        } else {
            at = haystack.find(pattern.literal, next);
            if (at == std::wstring::npos)
                break;
            end = at + pattern.literal.size();
        }
        next = end;
        if (pattern.options.whole_words &&
            ((at && word(haystack[at - 1])) || (end < haystack.size() && word(haystack[end])))) {
            next = at + 1;
            continue;
        }
        SearchMatch match;
        match.page = page;
        match.offset = at;
        size_t first = at > 32 ? at - 32 : 0, last = std::min(text.text.size(), end + 56);
        // Keep context within the surrounding text runs, so headings and footers
        // do not appear in a result for an unrelated sentence.
        while (first < at && text.positions[first].span != text.positions[at].span)
            ++first;
        while (last > end && text.positions[last - 1].span != text.positions[end - 1].span)
            --last;
        while (first && first < at && !space(text.text[first - 1]))
            ++first;
        const bool clipped_start = first && text.positions[first - 1].span == text.positions[first].span;
        const bool clipped_end =
            last < text.text.size() && text.positions[last].span == text.positions[last - 1].span;
        match.snippet =
            (clipped_start ? L"…" : L"") + text.text.substr(first, last - first) + (clipped_end ? L"…" : L"");
        for (size_t i = at; i < end; ++i) {
            const auto p = text.positions[i];
            if (p.span == SIZE_MAX)
                continue;
            if (!match.pieces.empty()) {
                auto& piece = match.pieces.back();
                if (piece.span == p.span && p.character >= piece.first &&
                    p.character <= piece.first + piece.count) {
                    piece.count = std::max(piece.count, p.character - piece.first + 1);
                    continue;
                }
            }
            match.pieces.push_back({p.span, p.character, 1});
        }
        matches.push_back(std::move(match));
    }
    return matches;
}
std::vector<SearchMatch> search_matches(const SearchText& text, std::wstring_view query,
                                        const SearchOptions& options, int page) {
    return find_matches(text, compile_search(query, options), page);
}
static const char* regex_error_message(std::regex_constants::error_type error) {
    switch (error) {
    case std::regex_constants::error_brack:
        return "Invalid regex: check the square brackets.";
    case std::regex_constants::error_paren:
        return "Invalid regex: check the parentheses.";
    case std::regex_constants::error_escape:
        return "Invalid regex: check the backslash escapes.";
    case std::regex_constants::error_range:
        return "Invalid regex: check the character range.";
    case std::regex_constants::error_backref:
        return "Invalid regex: check the group reference.";
    case std::regex_constants::error_badrepeat:
    case std::regex_constants::error_badbrace:
    case std::regex_constants::error_brace:
        return "Invalid regex: check the repetition operator.";
    case std::regex_constants::error_complexity:
    case std::regex_constants::error_stack:
    case std::regex_constants::error_space:
        return "Pattern is too complex. Use a simpler regular expression.";
    default:
        return "Invalid regex: check the expression syntax.";
    }
}
std::vector<Rect> search_rects(const RenderedPage& page, const SearchMatch& match) {
    std::vector<Rect> result;
    for (const auto& piece : match.pieces) {
        if (piece.span >= page.text.size())
            continue;
        const auto& span = page.text[piece.span];
        if (span.characters.size() != span.text.size()) {
            result.push_back(span.bounds);
            continue;
        }
        // Merge adjacent glyph boxes only within a text run. Keep line fragments separate.
        std::optional<Rect> bounds;
        for (size_t i = piece.first; i < std::min(span.characters.size(), piece.first + piece.count); ++i) {
            const auto& r = span.characters[i];
            if (!bounds)
                bounds = r;
            else {
                bounds->x0 = std::min(bounds->x0, r.x0);
                bounds->y0 = std::min(bounds->y0, r.y0);
                bounds->x1 = std::max(bounds->x1, r.x1);
                bounds->y1 = std::max(bounds->y1, r.y1);
            }
        }
        if (bounds)
            result.push_back(*bounds);
    }
    return result;
}
static void publish(SearchWorker& worker, SearchUpdate update) {
    bool notify = false;
    {
        std::unique_lock lock(worker.mutex);
        // Backpressure bounds queued pages and matches. A single large page is
        // always allowed, so it cannot deadlock waiting for a smaller payload.
        worker.ready.wait(lock, [&] {
            return worker.cancel || worker.stop || worker.updates.empty() ||
                   (worker.updates.size() < 32 && worker.queued_matches + update.matches.size() <= 8192);
        });
        if (worker.cancel || worker.stop)
            return;
        worker.queued_matches += update.matches.size();
        worker.updates.push_back(std::move(update));
        notify = !worker.notified;
        worker.notified = true;
    }
    if (notify)
        PostMessageW(worker.window, worker.message, 0, 0);
}
void run_search_worker(SearchWorker& worker) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Document document;
    document.cancel = &worker.cancel;
    Renderer renderer;
    uint64_t generation = 0;
    std::unordered_map<int, SearchText> cache;
    size_t cache_size = 0;
    std::vector<SearchMatch> reusable;
    try {
        for (;;) {
            SearchRequest request;
            SearchResults retired;
            std::deque<SearchUpdate> discarded;
            {
                std::unique_lock lock(worker.mutex);
                worker.ready.wait(lock, [&] {
                    return worker.stop || worker.request || worker.retired.count ||
                           (worker.cancel && !worker.updates.empty());
                });
                std::swap(retired, worker.retired);
                if (worker.cancel || worker.stop || worker.request) {
                    discarded.swap(worker.updates);
                    worker.queued_matches = 0;
                }
            }
            // Free old matches outside the mutex and on this thread. Keep at
            // most one modest allocation for the next page, not a whole query.
            for (auto& batch : retired.batches)
                if (batch.matches.capacity() > reusable.capacity() && batch.matches.capacity() <= 8192) {
                    reusable = std::move(batch.matches);
                    reusable.clear();
                }
            retired.batches.clear();
            discarded.clear();
            {
                std::lock_guard lock(worker.mutex);
                if (worker.stop)
                    break;
                // A new cancellation can arrive while cleanup is in progress.
                if (worker.retired.count || (worker.cancel && !worker.updates.empty()) || !worker.request)
                    continue;
                request = std::move(*worker.request);
                worker.request.reset();
                worker.cancel = false;
            }
            SearchUpdate update;
            update.document = request.document;
            update.ticket = request.ticket;
            try {
                const auto pattern = compile_search(request.query, request.options);
                hr(com, "Initialize search thread");
                if (!renderer.write)
                    init_renderer(renderer);
                if (generation != request.document) {
                    cache.clear();
                    cache_size = 0;
                    clear_resources(renderer);
                    generation = 0;
                    open(document, request.path, utf8(request.password));
                    generation = request.document;
                }
                update.total = request.options.page < 0 ? int(document.pages.size()) : 1;
                bool has_text = false;
                for (int i = 0; i < update.total; ++i) {
                    check_cancel(document);
                    const int page = request.options.page < 0 ? i : request.options.page;
                    auto found = cache.find(page);
                    SearchText extracted;
                    if (found == cache.end())
                        extracted = search_text(render_page(renderer, document, size_t(page), 1, 0, true));
                    const auto& text = found == cache.end() ? extracted : found->second;
                    has_text = has_text || !text.text.empty();
                    update.has_text = has_text;
                    update.matches = find_matches(text, pattern, page, &worker.cancel, std::move(reusable));
                    if (found == cache.end()) {
                        const size_t cost =
                            extracted.text.size() * (sizeof(wchar_t) + sizeof(SearchPosition));
                        constexpr size_t limit = 32 * 1024 * 1024;
                        if (cost <= limit) {
                            if (cache_size + cost > limit) {
                                cache.clear();
                                cache_size = 0;
                            }
                            cache_size += cost;
                            cache.emplace(page, std::move(extracted));
                        }
                    }
                    update.checked = i + 1;
                    update.complete = update.checked == update.total;
                    publish(worker, std::move(update));
                    update = {request.document,
                              request.ticket,
                              i + 1,
                              request.options.page < 0 ? int(document.pages.size()) : 1,
                              false,
                              has_text,
                              {},
                              {}};
                }
            } catch (const std::regex_error& e) {
                if (!worker.cancel && !worker.stop) {
                    update.error = utf8(regex_error_message(e.code()));
                    update.complete = true;
                    publish(worker, std::move(update));
                }
            } catch (const std::exception& e) {
                if (!worker.cancel && !worker.stop) {
                    update.error = utf8(e.what());
                    update.complete = true;
                    publish(worker, std::move(update));
                }
            }
        }
    } catch (const std::exception& e) {
        SearchUpdate failed;
        {
            std::lock_guard lock(worker.mutex);
            if (worker.request) {
                failed.document = worker.request->document;
                failed.ticket = worker.request->ticket;
            }
        }
        failed.error = utf8(e.what());
        failed.complete = true;
        publish(worker, std::move(failed));
    }
    close_renderer(renderer);
    close(document);
    if (SUCCEEDED(com))
        CoUninitialize();
}
void submit_search(SearchWorker& worker, SearchRequest request) {
    {
        std::lock_guard lock(worker.mutex);
        worker.cancel = true;
        worker.notified = false;
        worker.request = std::move(request);
    }
    worker.ready.notify_one();
}
void cancel_search(SearchWorker& worker) {
    {
        std::lock_guard lock(worker.mutex);
        worker.cancel = true;
        worker.notified = false;
        worker.request.reset();
    }
    worker.ready.notify_one();
}
void retire_search_results(SearchWorker& worker, SearchResults& results) {
    if (!results.count)
        return;
    {
        std::lock_guard lock(worker.mutex);
        // One pending generation is enough: the worker drains it before it can
        // produce any matches for the next query. Empty clears do not enqueue.
        assert(!worker.retired.count);
        std::swap(worker.retired, results);
    }
    worker.ready.notify_one();
}
bool take_search_update(SearchWorker& worker, uint64_t document, uint64_t ticket, SearchUpdate& update) {
    {
        std::lock_guard lock(worker.mutex);
        if (worker.cancel || worker.updates.empty() || worker.updates.front().document != document ||
            worker.updates.front().ticket != ticket)
            return false;
        worker.queued_matches -= worker.updates.front().matches.size();
        update = std::move(worker.updates.front());
        worker.updates.pop_front();
    }
    worker.ready.notify_one();
    return true;
}
bool finish_search_updates(SearchWorker& worker) {
    std::lock_guard lock(worker.mutex);
    const bool more = !worker.cancel && !worker.updates.empty();
    if (!more)
        worker.notified = false;
    return more;
}
void stop_search_worker(SearchWorker& worker) {
    worker.stop = true;
    cancel_search(worker);
    worker.ready.notify_one();
    if (worker.thread.joinable())
        worker.thread.join();
}
} // namespace pdf
