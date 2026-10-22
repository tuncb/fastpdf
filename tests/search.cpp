#include "search.h"
#include <iostream>
#include <regex>
#include <chrono>

using namespace pdf;
static void check(bool value, const char* description) {
    if (!value)
        throw std::runtime_error(description);
}
static TextSpan span(std::wstring text, double x, double y) {
    TextSpan result{std::move(text), {x, y, x, y + 10}, {}};
    for (size_t i = 0; i < result.text.size(); ++i)
        result.characters.push_back({x + i * 5, y, x + (i + 1) * 5, y + 10});
    result.bounds.x1 = x + result.text.size() * 5;
    return result;
}
static Rect mapped_box(Matrix matrix, Rect box) {
    const auto a = transform(matrix, {box.x0, box.y0});
    const auto b = transform(matrix, {box.x1, box.y1});
    return {std::min(a.x, b.x), std::min(a.y, b.y), std::max(a.x, b.x), std::max(a.y, b.y)};
}
int main() {
    try {
        RenderedPage page;
        page.text = {span(L"warranty warranty Warranty warrantyholder", 0, 0)};
        const auto text = search_text(page);
        auto matches = search_matches(text, L"warranty", {});
        check(matches.size() == 4, "Every occurrence in one text run is found");
        check(matches[0].offset == 0 && matches[1].offset == 9, "Matches retain distinct offsets");
        SearchResults results;
        append_search_results(results, matches);
        append_search_results(results, {});
        append_search_results(results, search_matches(text, L"warranty", {}, 3));
        check(results.count == 8 && results.batches.size() == 2, "Empty pages do not add result rows");
        check(search_result(results, 3).page == 0 && search_result(results, 4).page == 3 &&
                  search_result(results, 7).offset == matches[3].offset,
              "Row lookup preserves order across page batch boundaries");
        check(search_result_page(results, 0)->first == 0 && search_result_page(results, 3)->first == 4 &&
                  !search_result_page(results, 1) && !search_result_page(results, 4),
              "Highlights use only the requested page's batch");
        auto boxes = search_rects(page, matches[1]);
        check(boxes.size() == 1 && boxes[0].x0 == 45 && boxes[0].x1 == 85,
              "Only the matching characters are highlighted");
        SearchOptions options;
        options.whole_words = true;
        check(search_matches(text, L"warranty", options).size() == 3, "Whole words exclude a suffix");
        options.match_case = true;
        check(search_matches(text, L"warranty", options).size() == 2, "Case and word options combine");
        page.text = {span(L"serial", 0, 0), span(L"number", 0, 20)};
        matches = search_matches(search_text(page), L"serial   number", {});
        check(matches.size() == 1 && matches[0].pieces.size() == 2, "A phrase crosses text runs and lines");
        boxes = search_rects(page, matches[0]);
        check(boxes.size() == 2 && boxes[0].y0 == 0 && boxes[1].y0 == 20,
              "Cross-line highlights stay on each line");
        // Ableton's manual splits this word into A, b, l, e, and ton, with small
        // kerning gaps. Every fragment used to get its own overlapping outline.
        const std::vector<TextSpan> fragments{span(L"A", 10, 20), span(L"b", 15.08, 20),
                                              span(L"l", 20.09, 20), span(L"e", 25.10, 20),
                                              span(L"ton", 30.12, 20)};
        const Matrix rotations[] = {
            {1, 0, 0, -1, 0, 100}, {0, 1, 1, 0, 0, 0}, {-1, 0, 0, 1, 100, 0}, {0, -1, -1, 0, 100, 100}};
        for (double scale : {0.75, 2.0})
            for (auto rotation : rotations) {
                page.page_matrix = multiply({scale, 0, 0, scale, 0, 0}, rotation);
                page.text = fragments;
                for (auto& fragment : page.text) {
                    fragment.bounds = mapped_box(page.page_matrix, fragment.bounds);
                    for (auto& character : fragment.characters)
                        character = mapped_box(page.page_matrix, character);
                }
                matches = search_matches(search_text(page), L"Ableton", {});
                check(matches.size() == 1, "Kerning fragments remain one match at every rotation");
                boxes = search_rects(page, matches[0]);
                const auto expected = mapped_box(page.page_matrix, {10, 20, 45.12, 30});
                check(boxes.size() == 1 && std::abs(boxes[0].x0 - expected.x0) < 1e-9 &&
                          std::abs(boxes[0].y0 - expected.y0) < 1e-9 &&
                          std::abs(boxes[0].x1 - expected.x1) < 1e-9 &&
                          std::abs(boxes[0].y1 - expected.y1) < 1e-9,
                      "Split words have one highlight at every zoom and page rotation");
                page.text = {span(L"serial", 0, 0), span(L"number", 0, 20)};
                for (auto& fragment : page.text) {
                    fragment.bounds = mapped_box(page.page_matrix, fragment.bounds);
                    for (auto& character : fragment.characters)
                        character = mapped_box(page.page_matrix, character);
                }
                matches = search_matches(search_text(page), L"serial number", {});
                check(matches.size() == 1 && search_rects(page, matches[0]).size() == 2,
                      "Rotated cross-line matches retain separate highlights");
            }
        page.page_matrix = {};
        page.text = {span(L"left", 0, 0), span(L"right", 100, 0)};
        matches = search_matches(search_text(page), L"left right", {});
        check(matches.size() == 1 && search_rects(page, matches[0]).size() == 2,
              "Highlights do not fill a gap between distant columns");
        page.text = {span(L"war", 0, 0), span(L"ranty", 15, 0)};
        check(search_matches(search_text(page), L"warranty", {}).size() == 1,
              "Adjacent PDF text fragments form one word");
        page.page_matrix = {0, 1, -1, 0, 100, 0};
        page.text[0].bounds = {90, 0, 100, 15};
        page.text[1].bounds = {90, 15, 100, 40};
        check(search_matches(search_text(page), L"warranty", {}).size() == 1,
              "Page rotation does not insert a space inside a word");
        page.page_matrix = {};
        page.text = {span(L"o\ufb03ce war\u00adranty", 0, 0)};
        matches = search_matches(search_text(page), L"office", {});
        check(matches.size() == 1 && matches[0].pieces[0].count == 4,
              "Expanded ligatures map back to the original glyphs");
        check(search_matches(search_text(page), L"warranty", {}).size() == 1,
              "Soft hyphens do not interrupt a word");
        page.text = {span(L"\u00c4PFEL \u00e4pfel", 0, 0)};
        check(search_matches(search_text(page), L"\u00e4pfel", {}).size() == 2,
              "Case matching works outside ASCII");
        check(search_matches(text, L" \t ", {}).empty(), "A blank query returns no matches");
        check(search_matches(text, L"absent", {}).empty(), "An absent query returns no matches");
        options = {};
        options.regex = true;
        check(search_matches(text, L"warr[a-z]+", options).size() == 4,
              "Regex is case insensitive by default");
        options.match_case = true;
        check(search_matches(text, L"warranty", options).size() == 3, "Regex respects case");
        options.whole_words = true;
        check(search_matches(text, L"war|warranty", options).size() == 2,
              "Whole-word regex tries the complete alternative");
        options.match_case = false;
        check(search_matches(text, L"war|warranty", options).size() == 3, "Regex options combine");
        options.whole_words = false;
        check(search_matches(text, L"^warranty", options).size() == 1,
              "Anchors do not restart at each match");
        check(search_matches(text, L" warranty ", options).size() == 1, "Regex preserves significant spaces");
        check(search_matches(text, L"\\b", options).empty(),
              "Zero-length matches do not loop or add empty results");
        page.text = {span(L"Code 123 codes 45", 0, 0)};
        matches = search_matches(search_text(page), L"\\d+", options);
        check(matches.size() == 2 && matches[0].pieces[0].count == 3 && matches[1].pieces[0].count == 2,
              "Regex highlights variable-length matches");
        check(search_matches(search_text(page), L"\\D+", options).size() == 2,
              "Case-insensitive mode does not change regex escape syntax");
        check(search_matches(search_text(page), L".*", {}).empty(),
              "Regex metacharacters remain literal when disabled");
        page.text = {span(L"serial", 0, 0), span(L"number", 0, 20)};
        matches = search_matches(search_text(page), L"serial\\s+number", options);
        check(matches.size() == 1 && matches[0].pieces.size() == 2, "Regex spans normalized text runs");
        page.text = {span(L"abab xabab ABAB", 0, 0)};
        options.whole_words = true;
        check(search_matches(search_text(page), L"(ab)\\1", options).size() == 2,
              "Whole-word mode preserves capture group numbers");
        page.text = {span(L"\u00c4PFEL \u00e4pfel \u00e4pfelbaum", 0, 0)};
        check(search_matches(search_text(page), L"\u00e4pfel", options).size() == 2,
              "Regex case and whole-word matching handle Unicode");
        bool invalid = false;
        try {
            search_matches(text, L"[", options);
        } catch (const std::regex_error&) {
            invalid = true;
        }
        check(invalid, "Invalid regex reports an error");
        page.text = {span(std::wstring(3000, L'a'), 0, 0)};
        options.whole_words = false;
        check(search_matches(search_text(page), L"a", options).size() == 3000,
              "Many simple regex matches do not repeatedly scan the whole page");
        page.text = {span(std::wstring(5000, L'a') + L'!', 0, 0)};
        const auto started = std::chrono::steady_clock::now();
        bool bounded = false;
        try {
            search_matches(search_text(page), L"(a+)+$", options);
        } catch (const std::exception&) {
            bounded = true;
        }
        check(bounded && std::chrono::steady_clock::now() - started < std::chrono::seconds(2),
              "Pathological regex stops within a bounded time");
        std::cout << "Search text, geometry, regex, case, word boundaries, and execution limits passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
