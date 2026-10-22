#include "search.h"
#include <iostream>

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
int main() {
    try {
        RenderedPage page;
        page.text = {span(L"warranty warranty Warranty warrantyholder", 0, 0)};
        const auto text = search_text(page);
        auto matches = search_matches(text, L"warranty", {});
        check(matches.size() == 4, "Every occurrence in one text run is found");
        check(matches[0].offset == 0 && matches[1].offset == 9, "Matches retain distinct offsets");
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
        std::cout << "Search text, offsets, geometry, word boundaries, and normalization passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
