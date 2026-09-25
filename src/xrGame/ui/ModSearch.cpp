// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#include "StdAfx.h"
#include "ModSearch.h"

namespace
{
// Windows-1251: Latin and Cyrillic letters folded to lower case, ё (and Ё) to е, the Ukrainian and
// Belarusian letters to their lower case; 0 for what is not a letter or a digit.
u8 fold_char(u8 c)
{
    if (c >= '0' && c <= '9')
        return c;
    if (c >= 'a' && c <= 'z')
        return c;
    if (c >= 'A' && c <= 'Z')
        return static_cast<u8>(c + ('a' - 'A'));
    if (c >= 0xE0)
        return c;
    if (c >= 0xC0)
        return static_cast<u8>(c + 0x20);
    switch (c)
    {
    case 0xA8:
    case 0xB8: return 0xE5; // Ё ё -> е
    case 0xAA: return 0xBA; // Є
    case 0xBA: return 0xBA;
    case 0xAF: return 0xBF; // Ї
    case 0xBF: return 0xBF;
    case 0xB2: return 0xB3; // І
    case 0xB3: return 0xB3;
    case 0xA5: return 0xB4; // Ґ
    case 0xB4: return 0xB4;
    case 0xA1: return 0xA2; // Ў
    case 0xA2: return 0xA2;
    default: return 0;
    }
}

// At most one substitution, insertion, deletion or swap of neighbours apart.
bool within_one_edit(std::string_view a, std::string_view b)
{
    if (a.size() > b.size())
        std::swap(a, b);
    if (b.size() - a.size() > 1)
        return false;
    size_t i = 0;
    while (i < a.size() && a[i] == b[i])
        ++i;
    if (i == a.size())
        return true;
    if (a.size() == b.size())
    {
        if (a.substr(i + 1) == b.substr(i + 1))
            return true;
        return i + 1 < a.size() && a[i] == b[i + 1] && a[i + 1] == b[i] && a.substr(i + 2) == b.substr(i + 2);
    }
    return a.substr(i) == b.substr(i + 1);
}

// How well a query word starts a word: 1 exactly, 0.6 with one typo, 0 not at all.
float match_word(std::string_view query, std::string_view word)
{
    if (word.starts_with(query))
        return 1.f;
    if (query.size() < 5)
        return 0.f;
    for (size_t length = query.size() - 1; length <= query.size() + 1; ++length)
        if (length <= word.size() && within_one_edit(query, word.substr(0, length)))
            return 0.6f;
    return 0.f;
}

float match_field(std::string_view query, std::string_view field)
{
    float best = 0.f;
    for (const std::string_view word : ModSearch::Words(field))
    {
        best = std::max(best, match_word(query, word));
        if (best >= 1.f)
            break;
    }
    return best;
}
} // namespace

xr_string ModSearch::Fold(std::string_view text)
{
    xr_string out;
    out.reserve(text.size());
    for (const char c : text)
    {
        const u8 folded = fold_char(static_cast<u8>(c));
        out += folded ? static_cast<char>(folded) : ' ';
    }
    return out;
}

xr_vector<std::string_view> ModSearch::Words(std::string_view folded)
{
    xr_vector<std::string_view> words;
    size_t start = folded.find_first_not_of(' ');
    while (start != std::string_view::npos)
    {
        const size_t end = folded.find(' ', start);
        words.push_back(folded.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
        start = end == std::string_view::npos ? end : folded.find_first_not_of(' ', end);
    }
    return words;
}

float ModSearch::Score(const xr_vector<xr_string>& query, const Fields& fields)
{
    float total = 0.f;
    for (const xr_string& word : query)
    {
        const float best = std::max({8.f * match_field(word, fields.name), 4.f * match_field(word, fields.tags),
            3.f * match_field(word, fields.author), 1.f * match_field(word, fields.description)});
        if (best <= 0.f)
            return 0.f;
        total += best;
    }
    return total;
}
