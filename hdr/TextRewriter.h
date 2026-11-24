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

	namespace Detail
	{
		// Reads one UTF-8 character and keeps invalid bytes as single legacy characters.
		[[nodiscard]] inline char32_t ReadCodePoint(std::string_view a_text, std::size_t a_index, std::size_t& a_length) noexcept
		{
			const auto lead = static_cast<unsigned char>(a_text[a_index]);
			a_length = 1;
			if (lead < 0x80) {
				return lead;
			}
			const std::size_t count = lead >= 0xC2 && lead <= 0xDF ? 2 : lead >= 0xE0 && lead <= 0xEF ? 3 : lead >= 0xF0 && lead <= 0xF4 ? 4 : 0;
			const char32_t invalid = 0x110000u + lead;
			if (!count || a_index + count > a_text.size()) {
				return invalid;
			}
			char32_t value = lead & (0x7Fu >> count);
			for (std::size_t i = 1; i < count; ++i) {
				const auto next = static_cast<unsigned char>(a_text[a_index + i]);
				if ((next & 0xC0) != 0x80) {
					return invalid;
				}
				value = (value << 6) | (next & 0x3Fu);
			}
			if ((count == 3 && (value < 0x800 || (value >= 0xD800 && value <= 0xDFFF))) || (count == 4 && (value < 0x10000 || value > 0x10FFFF))) {
				return invalid;
			}
			a_length = count;
			return value;
		}

		// Folds the letter case of Latin, Greek, and Cyrillic characters, ignoring Greek accents.
		[[nodiscard]] inline char32_t FoldCase(char32_t a_value) noexcept
		{
			if (a_value < 0x80) {
				return a_value >= 'A' && a_value <= 'Z' ? a_value + 0x20 : a_value;
			}
			switch (a_value) {
			case 0x178:
				return 0xFF;
			case 0x386:
			case 0x3AC:
				return 0x3B1;
			case 0x388:
			case 0x3AD:
				return 0x3B5;
			case 0x389:
			case 0x3AE:
				return 0x3B7;
			case 0x38A:
			case 0x3AF:
				return 0x3B9;
			case 0x38C:
			case 0x3CC:
				return 0x3BF;
			case 0x38E:
			case 0x3CD:
				return 0x3C5;
			case 0x38F:
			case 0x3CE:
				return 0x3C9;
			case 0x3C2:
				return 0x3C3;
			case 0x390:
				return 0x3CA;
			case 0x3B0:
				return 0x3CB;
			case 0x4C0:
				return 0x4CF;
			}
			if ((a_value >= 0xC0 && a_value <= 0xDE && a_value != 0xD7) || (a_value >= 0x391 && a_value <= 0x3AB && a_value != 0x3A2) ||
				(a_value >= 0x410 && a_value <= 0x42F)) {
				return a_value + 0x20;
			}
			if (a_value >= 0x400 && a_value <= 0x40F) {
				return a_value + 0x50;
			}
			if (a_value >= 0x100 && a_value <= 0x17E && a_value != 0x130 && a_value != 0x131 && a_value != 0x138 && a_value != 0x149) {
				const bool oddUpper = (a_value >= 0x139 && a_value <= 0x148) || a_value >= 0x179;
				return ((a_value & 1) != 0) == oddUpper ? a_value + 1 : a_value;
			}
			if ((a_value >= 0x460 && a_value <= 0x481) || (a_value >= 0x48A && a_value <= 0x4BF) || (a_value >= 0x4D0 && a_value <= 0x52F) ||
				(a_value >= 0x1E00 && a_value <= 0x1E95) || (a_value >= 0x1EA0 && a_value <= 0x1EFF)) {
				return a_value | 1;
			}
			return a_value >= 0x4C1 && a_value <= 0x4CE && (a_value & 1) != 0 ? a_value + 1 : a_value;
		}

		enum class CharClass : std::uint8_t
		{
			kOther,
			kWord,
			kUnspaced,
			kMark
		};

		// Gets how a character joins neighboring text into words.
		[[nodiscard]] inline CharClass GetCharClass(char32_t a_value) noexcept
		{
			if (a_value < 0x80) {
				return std::isalnum(static_cast<unsigned char>(a_value)) || a_value == '_' ? CharClass::kWord : CharClass::kOther;
			}
			if ((a_value >= 0x300 && a_value <= 0x36F) || (a_value >= 0x1AB0 && a_value <= 0x1AFF) || (a_value >= 0x1DC0 && a_value <= 0x1DFF) ||
				(a_value >= 0x20D0 && a_value <= 0x20FF) || (a_value >= 0xFE20 && a_value <= 0xFE2F) || a_value == 0x3099 || a_value == 0x309A) {
				return CharClass::kMark;
			}
			if (a_value > 0x10FFFF || a_value == 0xAA || a_value == 0xB5 || a_value == 0xBA ||
				(a_value >= 0xC0 && a_value <= 0x2AF && a_value != 0xD7 && a_value != 0xF7) ||
				(a_value >= 0x370 && a_value <= 0x52F && a_value != 0x37E && a_value != 0x387) || (a_value >= 0x1E00 && a_value <= 0x1FFF)) {
				return CharClass::kWord;
			}
			if ((a_value >= 0x1100 && a_value <= 0x11FF) || (a_value >= 0x3040 && a_value <= 0x30FF) || (a_value >= 0x3130 && a_value <= 0x318F) ||
				(a_value >= 0x3400 && a_value <= 0x4DBF) || (a_value >= 0x4E00 && a_value <= 0x9FFF) || (a_value >= 0xAC00 && a_value <= 0xD7AF) ||
				(a_value >= 0xF900 && a_value <= 0xFAFF) || (a_value >= 0xFF10 && a_value <= 0xFF19) || (a_value >= 0xFF21 && a_value <= 0xFF3A) ||
				(a_value >= 0xFF41 && a_value <= 0xFF5A) || (a_value >= 0xFF66 && a_value <= 0xFFDC) || (a_value >= 0x20000 && a_value <= 0x3FFFF)) {
				return CharClass::kUnspaced;
			}
			return CharClass::kOther;
		}

		// Checks whether a neighboring character continues the word at the edge of a match.
		[[nodiscard]] inline bool Continues(CharClass a_neighbour, CharClass a_edge) noexcept
		{
			return a_neighbour == CharClass::kWord || a_neighbour == CharClass::kMark ||
				(a_neighbour == CharClass::kUnspaced && a_edge == CharClass::kUnspaced);
		}

		// Gets the lookup bucket for a character that can begin replacement text.
		[[nodiscard]] inline std::optional<unsigned char> GetMatchKey(char32_t a_value) noexcept
		{
			if (a_value < 0x80) {
				return std::isalnum(static_cast<unsigned char>(a_value)) ?
					std::optional<unsigned char>(static_cast<unsigned char>(FoldCase(a_value))) : std::nullopt;
			}
			const auto type = GetCharClass(a_value);
			if (type != CharClass::kWord && type != CharClass::kUnspaced) {
				return std::nullopt;
			}
			return static_cast<unsigned char>(0x80 | (FoldCase(a_value) & 0x7F));
		}
	}

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
			for (std::size_t i = 0, length = 1; i < a_text.size(); i += length) {
				if (const auto key = Detail::GetMatchKey(Detail::ReadCodePoint(a_text, i, length))) {
					return key;
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

		// Gets the word class of the character at a position.
		[[nodiscard]] inline CharClass ClassAt(std::string_view a_text, std::size_t a_index) noexcept
		{
			std::size_t length = 1;
			return a_index < a_text.size() ? GetCharClass(ReadCodePoint(a_text, a_index, length)) : CharClass::kOther;
		}

		// Gets the word class of the character before a position.
		[[nodiscard]] inline CharClass ClassBefore(std::string_view a_text, std::size_t a_index) noexcept
		{
			if (!a_index || a_index > a_text.size()) {
				return CharClass::kOther;
			}
			auto start = a_index - 1;
			while (start && a_index - start < 4 && (static_cast<unsigned char>(a_text[start]) & 0xC0) == 0x80) {
				--start;
			}
			std::size_t length = 1;
			const auto value = ReadCodePoint(a_text, start, length);
			return GetCharClass(start + length == a_index ? value : 0x110000u);
		}

		// Gets the byte length of a space character at a position.
		[[nodiscard]] inline std::size_t SpaceLength(std::string_view a_text, std::size_t a_index) noexcept
		{
			if (a_index >= a_text.size()) {
				return 0;
			}
			const auto value = static_cast<unsigned char>(a_text[a_index]);
			if (value < 0x80) {
				return std::isspace(value) ? 1 : 0;
			}
			std::size_t length = 1;
			const auto codePoint = ReadCodePoint(a_text, a_index, length);
			const bool space = codePoint == 0xA0 || codePoint == 0x1680 || (codePoint >= 0x2000 && codePoint <= 0x200A) ||
				codePoint == 0x202F || codePoint == 0x205F || codePoint == 0x3000;
			return space ? length : 0;
		}

		// Skips space characters from a position.
		[[nodiscard]] inline std::size_t SkipSpaces(std::string_view a_text, std::size_t a_index) noexcept
		{
			while (const auto length = SpaceLength(a_text, a_index)) {
				a_index += length;
			}
			return a_index;
		}

		// Compares one character from each text without caring about letter case.
		[[nodiscard]] inline bool SameCharacter(std::string_view a_left, std::size_t a_leftIndex, std::size_t& a_leftLength,
			std::string_view a_right, std::size_t a_rightIndex, std::size_t& a_rightLength) noexcept
		{
			if (static_cast<unsigned char>(a_left[a_leftIndex]) < 0x80 && static_cast<unsigned char>(a_right[a_rightIndex]) < 0x80) {
				a_leftLength = 1;
				a_rightLength = 1;
				return SameText(a_left[a_leftIndex], a_right[a_rightIndex]);
			}
			return FoldCase(ReadCodePoint(a_left, a_leftIndex, a_leftLength)) == FoldCase(ReadCodePoint(a_right, a_rightIndex, a_rightLength));
		}

		// Matches replacement text at word boundaries while allowing extra spaces.
		[[nodiscard]] inline bool MatchPhraseAt(std::string_view a_text, std::size_t a_start,
			std::string_view a_phrase, std::size_t& a_end, bool a_allowTerminalPunctuationOmission = false) noexcept
		{
			if (a_phrase.empty() || a_start >= a_text.size() || Continues(ClassBefore(a_text, a_start), ClassAt(a_phrase, 0))) {
				return false;
			}
			std::size_t text = a_start;
			std::size_t phrase = 0;
			auto edge = CharClass::kOther;
			while (phrase < a_phrase.size()) {
				if (SpaceLength(a_phrase, phrase)) {
					phrase = SkipSpaces(a_phrase, phrase);
					if (!SpaceLength(a_text, text)) {
						return false;
					}
					text = SkipSpaces(a_text, text);
					edge = CharClass::kOther;
					continue;
				}
				std::size_t textLength = 1;
				std::size_t phraseLength = 1;
				if (text >= a_text.size() || !SameCharacter(a_text, text, textLength, a_phrase, phrase, phraseLength)) {
					const auto optionalTerminal = [](char a_character) noexcept {
						return a_character == '!' || a_character == '?' || a_character == '.';
					};
					const auto trailingOptional = std::all_of(a_phrase.begin() + static_cast<std::ptrdiff_t>(phrase), a_phrase.end(), optionalTerminal);
					const auto safeBoundary = text >= a_text.size() ||
						(!Continues(ClassAt(a_text, text), edge) && a_text[text] != '-' && a_text[text] != '\'');
					if (!a_allowTerminalPunctuationOmission || !trailingOptional || !safeBoundary) {
						return false;
					}
					phrase = a_phrase.size();
					break;
				}
				edge = ClassAt(a_text, text);
				text += textLength;
				phrase += phraseLength;
			}
			if (Continues(ClassAt(a_text, text), edge)) {
				return false;
			}
			a_end = text;
			return true;
		}

		// Includes a nearby rank label when it belongs to the same source perk rank.
		[[nodiscard]] inline bool ExtendOwnedRankSuffix(std::string_view a_text, std::size_t a_nameEnd,
			std::uint32_t a_sourceRank, std::size_t& a_ownedEnd) noexcept
		{
			if (!a_sourceRank || !SpaceLength(a_text, a_nameEnd)) {
				return false;
			}

			std::size_t cursor = SkipSpaces(a_text, a_nameEnd);
			if (cursor < a_text.size() && SameText(a_text[cursor], 'r')) {
				++cursor;
				if (cursor + 3 <= a_text.size() && SameText(a_text[cursor], 'a') &&
					SameText(a_text[cursor + 1], 'n') && SameText(a_text[cursor + 2], 'k')) {
					cursor += 3;
					if (!SpaceLength(a_text, cursor)) {
						return false;
					}
				}

				cursor = SkipSpaces(a_text, cursor);
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
			if (cursor == digitsStart || Continues(ClassAt(a_text, cursor), CharClass::kWord) || parsedRank != a_sourceRank) {
				return false;
			}

			a_ownedEnd = cursor;
			return true;
		}

		// Checks whether a number, optionally after one word, follows a matched name.
		[[nodiscard]] inline bool FollowedByNumber(std::string_view a_text, std::size_t a_end) noexcept
		{
			if (!SpaceLength(a_text, a_end)) {
				return false;
			}
			auto cursor = SkipSpaces(a_text, a_end);
			if (cursor < a_text.size() && !std::isdigit(static_cast<unsigned char>(a_text[cursor]))) {
				std::size_t length = 1;
				while (cursor < a_text.size() && GetCharClass(ReadCodePoint(a_text, cursor, length)) == CharClass::kWord) {
					cursor += length;
				}
				if (!SpaceLength(a_text, cursor)) {
					return false;
				}
				cursor = SkipSpaces(a_text, cursor);
			}
			return cursor < a_text.size() && std::isdigit(static_cast<unsigned char>(a_text[cursor]));
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
		auto previousClass = Detail::CharClass::kOther;
		for (std::size_t start = 0, length = 1; start < a_original.size(); start += length) {
			const auto value = Detail::ReadCodePoint(a_original, start, length);
			const auto key = Detail::GetMatchKey(value);
			const auto type = Detail::GetCharClass(value);
			const auto previous = std::exchange(previousClass, type);
			if (!key || Detail::Continues(previous, type)) {
				continue;
			}
			for (const auto index : a_dictionary.ProtectedBucket(*key)) {
				std::size_t end = start;
				if (Detail::MatchPhraseAt(a_original, start, a_dictionary.ProtectedPhrases()[index], end)) {
					protectedSpans.push_back({ start, end });
				}
			}
		}

		std::vector<Detail::Match> matches;
		previousClass = Detail::CharClass::kOther;
		for (std::size_t start = 0, length = 1; start < a_original.size(); start += length) {
			const auto value = Detail::ReadCodePoint(a_original, start, length);
			const auto key = Detail::GetMatchKey(value);
			const auto type = Detail::GetCharClass(value);
			const auto previous = std::exchange(previousClass, type);
			if (!key || Detail::Continues(previous, type)) {
				continue;
			}
			const auto& bucket = a_dictionary.Bucket(*key);
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
				if (alias.sourceRank && !ownsExplicitRank && Detail::FollowedByNumber(a_original, canonicalEnd)) {
					continue;
				}
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
