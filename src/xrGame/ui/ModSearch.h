// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#pragma once

// The Mod Browser's search (docs/dead-air/MOD_CATALOG.md 14): every word of the query must match
// the start of a word of the name, the author, the tags or the description; a word of five
// letters or more may carry one typo; case and ё/е do not matter; the name weighs most. Text is in
// the UI code page (Windows-1251), where the edit box types it.

namespace ModSearch
{
// Lower case, ё as е, everything that is not a letter or a digit a space.
xr_string Fold(std::string_view text);

// The words of a folded text.
xr_vector<std::string_view> Words(std::string_view folded);

struct Fields
{
    xr_string name, author, tags, description; // folded
};

// 0 when some word of the query matches nothing; otherwise the higher, the better the match.
float Score(const xr_vector<xr_string>& query, const Fields& fields);
} // namespace ModSearch
