#include "search.h"
#include <cwctype>

namespace pdf {
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
std::vector<SearchMatch> search_matches(const SearchText& text, std::wstring_view query,
                                        const SearchOptions& options, int page) {
    SearchText normalized;
    for (auto c : query)
        append(normalized, c, {});
    while (!normalized.text.empty() && normalized.text.back() == L' ')
        normalized.text.pop_back();
    std::vector<SearchMatch> matches;
    if (normalized.text.empty())
        return matches;
    auto needle = options.match_case ? normalized.text : fold(normalized.text);
    auto haystack = options.match_case ? text.text : fold(text.text);
    for (size_t at = 0; (at = haystack.find(needle, at)) != std::wstring::npos; at += needle.size()) {
        const size_t end = at + needle.size();
        if (options.whole_words &&
            ((at && word(haystack[at - 1])) || (end < haystack.size() && word(haystack[end]))))
            continue;
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
    {
        std::lock_guard lock(worker.mutex);
        if (worker.cancel || worker.stop)
            return;
        worker.updates.push_back(std::move(update));
    }
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
    try {
        while (!worker.stop) {
            SearchRequest request;
            {
                std::unique_lock lock(worker.mutex);
                worker.ready.wait(lock, [&] { return worker.stop || worker.request.has_value(); });
                if (worker.stop)
                    break;
                request = std::move(*worker.request);
                worker.request.reset();
                worker.cancel = false;
            }
            SearchUpdate update;
            update.document = request.document;
            update.ticket = request.ticket;
            try {
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
                    update.matches = search_matches(text, request.query, request.options, page);
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
        worker.request = std::move(request);
        worker.updates.clear();
    }
    worker.ready.notify_one();
}
void cancel_search(SearchWorker& worker) {
    std::lock_guard lock(worker.mutex);
    worker.cancel = true;
    worker.request.reset();
    worker.updates.clear();
}
void stop_search_worker(SearchWorker& worker) {
    worker.stop = true;
    cancel_search(worker);
    worker.ready.notify_one();
    if (worker.thread.joinable())
        worker.thread.join();
}
} // namespace pdf
