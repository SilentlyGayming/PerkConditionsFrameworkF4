// Perk Conditions Framework
// SilentlyGayming
// TextRewriter.h

#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace PCF::TextRewriter
{
	enum class MatchType : std::uint8_t
	{
		kPerk,
		kActorValue,
		kGlobalValue
	};

	struct MatchKey
	{
		MatchType type{ MatchType::kPerk };
		std::uintptr_t target{ 0 };
		std::uint8_t comparison{ 0 };
		std::uint32_t rank{ 0 };
		float threshold{ 0.0F };

		// Checks whether two replacement targets are exactly the same.
		[[nodiscard]] bool operator==(const MatchKey& a_right) const noexcept
		{
			if (type != a_right.type || target != a_right.target || comparison != a_right.comparison) {
				return false;
			}
			return type == MatchType::kPerk ? rank == a_right.rank : threshold == a_right.threshold;
		}
	};

	struct AliasEntry
	{
		std::uintptr_t source{ 0 };
		std::uint32_t sourceRank{ 0 };
		std::string phrase;
		std::string replacement;
		std::string genericReplacement;
		MatchKey matchKey;
	};

	struct RewriteContext
	{
		std::span<const std::uintptr_t> preferredSources{};
		bool genericBareReplacement{ false };
		bool restrictToPreferredSources{ false };
	};

	// Stores fixed text replacements grouped by their first character.
	class Dictionary
	{
	public:
		// Builds and sorts the text replacement list.
		void Build(std::vector<AliasEntry> a_entries)
		{
			_entries = std::move(a_entries);
			for (auto& bucket : _buckets) {
				bucket.clear();
			}
			for (auto& bucket : _protectedBuckets) {
				bucket.clear();
			}
			_protected.clear();
			for (std::size_t i = 0; i < _entries.size(); ++i) {
				const auto first = FirstLetter(_entries[i].phrase);
				if (first) {
					_buckets[*first].push_back(i);
				}
				const auto value = std::string_view(_entries[i].replacement);
				if (!value.empty() && !std::any_of(_protected.begin(), _protected.end(), [&](const std::string& a_existing) {
					return a_existing.size() == value.size() && std::equal(a_existing.begin(), a_existing.end(), value.begin(),
						[](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); });
				})) {
					_protected.emplace_back(value);
				}
			}
			for (std::size_t i = 0; i < _protected.size(); ++i) {
				if (const auto first = FirstLetter(_protected[i])) {
					_protectedBuckets[*first].push_back(i);
				}
			}
			for (auto& bucket : _buckets) {
				std::stable_sort(bucket.begin(), bucket.end(), [this](std::size_t a_left, std::size_t a_right) {
					const auto& left = _entries[a_left];
					const auto& right = _entries[a_right];
					if (left.phrase.size() != right.phrase.size()) {
						return left.phrase.size() > right.phrase.size();
					}
					return LessNoCase(left.phrase, right.phrase);
				});
			}
		}

		// Checks whether there are no text replacements.
		[[nodiscard]] bool Empty() const noexcept { return _entries.empty(); }
		// Returns all text replacements.
		[[nodiscard]] const std::vector<AliasEntry>& Entries() const noexcept { return _entries; }
		// Returns replacement choices for one first character.
		[[nodiscard]] const std::vector<std::size_t>& Bucket(unsigned char a_key) const noexcept { return _buckets[a_key]; }
		// Returns replacement text that should not be changed again.
		[[nodiscard]] const std::vector<std::string>& ProtectedPhrases() const noexcept { return _protected; }
		// Returns protected replacement text for one first character.
		[[nodiscard]] const std::vector<std::size_t>& ProtectedBucket(unsigned char a_key) const noexcept { return _protectedBuckets[a_key]; }

	private:
		// Finds the first usable character for lookup.
		[[nodiscard]] static std::optional<unsigned char> FirstLetter(std::string_view a_text) noexcept
		{
			for (const auto character : a_text) {
				const auto value = static_cast<unsigned char>(character);
				if (std::isalnum(value)) {
					return static_cast<unsigned char>(std::tolower(value));
				}
			}
			return std::nullopt;
		}

		// Sorts text without caring about letter case.
		[[nodiscard]] static bool LessNoCase(std::string_view a_left, std::string_view a_right) noexcept
		{
			const auto count = std::min(a_left.size(), a_right.size());
			for (std::size_t i = 0; i < count; ++i) {
				const auto left = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(a_left[i])));
				const auto right = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(a_right[i])));
				if (left != right) {
					return left < right;
				}
			}
			return a_left.size() < a_right.size();
		}

		std::vector<AliasEntry> _entries;
		std::array<std::vector<std::size_t>, 256> _buckets;
		std::vector<std::string> _protected;
		std::array<std::vector<std::size_t>, 256> _protectedBuckets;
	};

	namespace Detail
	{
		struct Match
		{
			std::size_t start{ 0 };
			std::size_t end{ 0 };
			std::size_t aliasIndex{ 0 };
			std::string_view replacement;
			MatchKey matchKey;
			bool preferred{ false };
			bool rejected{ false };
		};

		// Compares two characters without caring about letter case.
		[[nodiscard]] inline bool SameText(char a_left, char a_right) noexcept
		{
			return std::tolower(static_cast<unsigned char>(a_left)) ==
				std::tolower(static_cast<unsigned char>(a_right));
		}

		// Checks whether a character is part of a word.
		[[nodiscard]] inline bool IsWordChar(char a_character) noexcept
		{
			const auto value = static_cast<unsigned char>(a_character);
			return std::isalnum(value) || a_character == '_';
		}

		// Matches replacement text at word boundaries while allowing extra spaces.
		[[nodiscard]] inline bool MatchPhraseAt(std::string_view a_text, std::size_t a_start,
			std::string_view a_phrase, std::size_t& a_end, bool a_allowTerminalPunctuationOmission = false) noexcept
		{
			if (a_phrase.empty() || a_start >= a_text.size() || (a_start && IsWordChar(a_text[a_start - 1]))) {
				return false;
			}
			std::size_t text = a_start;
			std::size_t phrase = 0;
			while (phrase < a_phrase.size()) {
				if (std::isspace(static_cast<unsigned char>(a_phrase[phrase]))) {
					while (phrase < a_phrase.size() && std::isspace(static_cast<unsigned char>(a_phrase[phrase]))) {
						++phrase;
					}
					if (text >= a_text.size() || !std::isspace(static_cast<unsigned char>(a_text[text]))) {
						return false;
					}
					while (text < a_text.size() && std::isspace(static_cast<unsigned char>(a_text[text]))) {
						++text;
					}
					continue;
				}
				if (text >= a_text.size() || !SameText(a_text[text], a_phrase[phrase])) {
					const auto optionalTerminal = [](char a_character) noexcept {
						return a_character == '!' || a_character == '?' || a_character == '.';
					};
					const auto trailingOptional = std::all_of(a_phrase.begin() + static_cast<std::ptrdiff_t>(phrase), a_phrase.end(), optionalTerminal);
					const auto safeBoundary = text >= a_text.size() ||
						(!IsWordChar(a_text[text]) && a_text[text] != '-' && a_text[text] != '\'');
					if (!a_allowTerminalPunctuationOmission || !trailingOptional || !safeBoundary) {
						return false;
					}
					phrase = a_phrase.size();
					break;
				}
				++text;
				++phrase;
			}
			if (text < a_text.size() && IsWordChar(a_text[text])) {
				return false;
			}
			a_end = text;
			return true;
		}

		// Includes a nearby rank label when it belongs to the same source perk rank.
		[[nodiscard]] inline bool ExtendOwnedRankSuffix(std::string_view a_text, std::size_t a_nameEnd,
			std::uint32_t a_sourceRank, std::size_t& a_ownedEnd) noexcept
		{
			if (!a_sourceRank || a_nameEnd >= a_text.size() ||
				!std::isspace(static_cast<unsigned char>(a_text[a_nameEnd]))) {
				return false;
			}

			std::size_t cursor = a_nameEnd;
			while (cursor < a_text.size() && std::isspace(static_cast<unsigned char>(a_text[cursor]))) {
				++cursor;
			}
			if (cursor >= a_text.size() || !SameText(a_text[cursor], 'r')) {
				return false;
			}

			++cursor;
			if (cursor + 3 <= a_text.size() && SameText(a_text[cursor], 'a') &&
				SameText(a_text[cursor + 1], 'n') && SameText(a_text[cursor + 2], 'k')) {
				cursor += 3;
				if (cursor >= a_text.size() || !std::isspace(static_cast<unsigned char>(a_text[cursor]))) {
					return false;
				}
			}

			while (cursor < a_text.size() && std::isspace(static_cast<unsigned char>(a_text[cursor]))) {
				++cursor;
			}
			if (cursor >= a_text.size() || !std::isdigit(static_cast<unsigned char>(a_text[cursor]))) {
				return false;
			}

			std::uint32_t parsedRank = 0;
			const auto digitsStart = cursor;
			while (cursor < a_text.size() && std::isdigit(static_cast<unsigned char>(a_text[cursor]))) {
				const auto digit = static_cast<std::uint32_t>(a_text[cursor] - '0');
				if (parsedRank > 255u / 10u || parsedRank * 10u + digit > 255u) {
					return false;
				}
				parsedRank = parsedRank * 10u + digit;
				++cursor;
			}
			if (cursor == digitsStart || (cursor < a_text.size() && IsWordChar(a_text[cursor])) || parsedRank != a_sourceRank) {
				return false;
			}

			a_ownedEnd = cursor;
			return true;
		}

		// Checks whether a replacement comes from a preferred source.
		[[nodiscard]] inline bool IsPreferredSource(std::uintptr_t a_source, std::span<const std::uintptr_t> a_sources) noexcept
		{
			return std::find(a_sources.begin(), a_sources.end(), a_source) != a_sources.end();
		}

		// Chooses between multiple replacements that match the same text.
		inline void PickBestMatch(std::vector<Match>& a_matches)
		{
			std::vector<std::size_t> group;
			for (std::size_t i = 0; i < a_matches.size(); ++i) {
				if (a_matches[i].rejected) {
					continue;
				}
				group.clear();
				for (std::size_t j = i + 1; j < a_matches.size(); ++j) {
					if (!a_matches[j].rejected && a_matches[j].start == a_matches[i].start && a_matches[j].end == a_matches[i].end) {
						if (group.empty()) {
							group.push_back(i);
						}
						group.push_back(j);
					}
				}
				if (group.empty()) {
					continue;
				}
				const bool anyPreferred = std::any_of(group.begin(), group.end(), [&](std::size_t a_index) {
					return a_matches[a_index].preferred;
				});
				if (anyPreferred) {
					for (const auto index : group) {
						if (!a_matches[index].preferred) {
							a_matches[index].rejected = true;
						}
					}
				}
				std::optional<std::size_t> chosen;
				for (const auto index : group) {
					if (a_matches[index].rejected) {
						continue;
					}
					if (!chosen) {
						chosen = index;
						continue;
					}
					const auto& first = a_matches[*chosen];
					const auto& current = a_matches[index];
					if (first.matchKey == current.matchKey && first.replacement == current.replacement) {
						a_matches[index].rejected = true;
					} else {
						for (const auto reject : group) {
							a_matches[reject].rejected = true;
						}
						break;
					}
				}
			}
		}

		enum class ListStyle : std::uint8_t
		{
			kComma,
			kAnd,
			kAmpersand
		};

		// Rebuilds a merged requirement list while keeping its original separators.
		[[nodiscard]] inline std::string RebuildList(const std::vector<const Match*>& a_unique,
			std::size_t a_originalCount, const std::vector<std::string_view>& a_separators)
		{
			ListStyle style = ListStyle::kComma;
			if (std::any_of(a_separators.begin(), a_separators.end(), [](std::string_view a_value) { return a_value.find('&') != std::string_view::npos; })) {
				style = ListStyle::kAmpersand;
			} else if (std::any_of(a_separators.begin(), a_separators.end(), [](std::string_view a_value) { return a_value.find("and") != std::string_view::npos; })) {
				style = ListStyle::kAnd;
			}
			if (a_unique.empty()) {
				return {};
			}
			if (a_unique.size() == 1) {
				return std::string(a_unique.front()->replacement);
			}
			if (a_unique.size() == 2) {
				if (a_originalCount >= 3 || style == ListStyle::kComma) {
					return std::string(a_unique[0]->replacement) + ", " + std::string(a_unique[1]->replacement);
				}
				return std::string(a_unique[0]->replacement) + (style == ListStyle::kAmpersand ? " & " : " and ") + std::string(a_unique[1]->replacement);
			}
			std::string result;
			for (std::size_t i = 0; i < a_unique.size(); ++i) {
				if (i) {
					result += i + 1 == a_unique.size() ? (style == ListStyle::kAmpersand ? " & " : style == ListStyle::kAnd ? ", and " : ", ") : ", ";
				}
				result += a_unique[i]->replacement;
			}
			return result;
		}
	}

	// Rewrites visible text once without scanning the new text again.
	[[nodiscard]] inline std::optional<std::string> RewriteVisibleText(std::string_view a_original,
		const Dictionary& a_dictionary, RewriteContext a_context = {})
	{
		if (a_original.empty() || a_dictionary.Empty()) {
			return std::nullopt;
		}

		struct ProtectedSpan { std::size_t start; std::size_t end; };
		std::vector<ProtectedSpan> protectedSpans;
		for (std::size_t start = 0; start < a_original.size(); ++start) {
			const auto value = static_cast<unsigned char>(a_original[start]);
			if (!std::isalnum(value) || (start && Detail::IsWordChar(a_original[start - 1]))) {
				continue;
			}
			const auto key = static_cast<unsigned char>(std::tolower(value));
			for (const auto index : a_dictionary.ProtectedBucket(key)) {
				std::size_t end = start;
				if (Detail::MatchPhraseAt(a_original, start, a_dictionary.ProtectedPhrases()[index], end)) {
					protectedSpans.push_back({ start, end });
				}
			}
		}

		std::vector<Detail::Match> matches;
		for (std::size_t start = 0; start < a_original.size(); ++start) {
			const auto value = static_cast<unsigned char>(a_original[start]);
			if (!std::isalnum(value) || (start && Detail::IsWordChar(a_original[start - 1]))) {
				continue;
			}
			const auto key = static_cast<unsigned char>(std::tolower(value));
			const auto& bucket = a_dictionary.Bucket(key);
			if (bucket.empty()) {
				continue;
			}
			for (const auto index : bucket) {
				const auto& alias = a_dictionary.Entries()[index];
				const auto preferred = Detail::IsPreferredSource(alias.source, a_context.preferredSources);
				if (a_context.restrictToPreferredSources && !preferred) {
					continue;
				}
				std::size_t end = start;
				if (!Detail::MatchPhraseAt(a_original, start, alias.phrase, end, preferred)) {
					continue;
				}
				if (std::any_of(protectedSpans.begin(), protectedSpans.end(), [&](const ProtectedSpan& a_span) {
					return start >= a_span.start && end <= a_span.end;
				})) {
					continue;
				}
				const auto canonicalEnd = end;
				const auto ownsExplicitRank = Detail::ExtendOwnedRankSuffix(a_original, canonicalEnd, alias.sourceRank, end);
				const auto generic = a_context.genericBareReplacement && !ownsExplicitRank && !alias.genericReplacement.empty();
				const auto replacement = generic ? std::string_view(alias.genericReplacement) : std::string_view(alias.replacement);
				if (replacement.empty()) {
					continue;
				}
				auto matchKey = alias.matchKey;
				if (generic) {
					matchKey.rank = 0;
					matchKey.threshold = 0.0F;
				}
				matches.push_back({ start, end, index, replacement, matchKey, preferred, false });
			}
		}
		if (matches.empty()) {
			return std::nullopt;
		}

		Detail::PickBestMatch(matches);
		std::stable_sort(matches.begin(), matches.end(), [&](const Detail::Match& a_left, const Detail::Match& a_right) {
			const auto leftLength = a_left.end - a_left.start;
			const auto rightLength = a_right.end - a_right.start;
			if (leftLength != rightLength) {
				return leftLength > rightLength;
			}
			if (a_left.start != a_right.start) {
				return a_left.start < a_right.start;
			}
			return a_left.aliasIndex < a_right.aliasIndex;
		});

		std::vector<Detail::Match> accepted;
		for (const auto& match : matches) {
			if (match.rejected) {
				continue;
			}
			const auto overlaps = std::any_of(accepted.begin(), accepted.end(), [&](const Detail::Match& a_existing) {
				return match.start < a_existing.end && a_existing.start < match.end;
			});
			if (!overlaps) {
				accepted.push_back(match);
			}
		}
		if (accepted.empty()) {
			return std::nullopt;
		}

		std::sort(accepted.begin(), accepted.end(), [](const Detail::Match& a_left, const Detail::Match& a_right) {
			return a_left.start < a_right.start;
		});
		constexpr std::array<std::string_view, 5> separators{ ", ", ", and ", ", & ", " and ", " & " };
		std::vector<bool> consumed(accepted.size(), false);
		struct Operation { std::size_t start; std::size_t end; std::string replacement; };
		std::vector<Operation> operations;
		for (std::size_t first = 0; first < accepted.size();) {
			std::size_t last = first;
			std::vector<std::string_view> joins;
			while (last + 1 < accepted.size()) {
				const auto gap = a_original.substr(accepted[last].end, accepted[last + 1].start - accepted[last].end);
				if (std::find(separators.begin(), separators.end(), gap) == separators.end()) {
					break;
				}
				joins.push_back(gap);
				++last;
			}
			if (last > first) {
				const auto groupEnd = accepted[last].end;
				std::vector<const Detail::Match*> unique;
				for (std::size_t i = first; i <= last; ++i) {
					if (std::none_of(unique.begin(), unique.end(), [&](const Detail::Match* a_existing) { return a_existing->matchKey == accepted[i].matchKey; })) {
						unique.push_back(&accepted[i]);
					}
				}
				const auto count = last - first + 1;
				if (unique.size() < count) {
					operations.push_back({ accepted[first].start, groupEnd, Detail::RebuildList(unique, count, joins) });
					for (std::size_t i = first; i <= last; ++i) {
						consumed[i] = true;
					}
				}
			}
			first = last + 1;
		}
		for (std::size_t i = 0; i < accepted.size(); ++i) {
			if (!consumed[i]) {
				operations.push_back({ accepted[i].start, accepted[i].end, std::string(accepted[i].replacement) });
			}
		}
		if (operations.empty()) {
			return std::nullopt;
		}

		std::sort(operations.begin(), operations.end(), [](const Operation& a_left, const Operation& a_right) { return a_left.start < a_right.start; });
		std::size_t finalSize = a_original.size();
		for (const auto& operation : operations) {
			finalSize += operation.replacement.size();
			finalSize -= operation.end - operation.start;
		}
		std::string result;
		result.reserve(finalSize);
		std::size_t cursor = 0;
		for (const auto& operation : operations) {
			result.append(a_original.substr(cursor, operation.start - cursor));
			result += operation.replacement;
			cursor = operation.end;
		}
		result.append(a_original.substr(cursor));
		return result == a_original ? std::nullopt : std::optional<std::string>(std::move(result));
	}
}
