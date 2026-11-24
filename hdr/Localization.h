// Perk Conditions Framework
// SilentlyGayming
// Localization.h

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

namespace PCF::Localization
{
	inline constexpr std::size_t kMaxTableSize = 16u * 1024u * 1024u;

	struct Table
	{
		std::unordered_map<std::string, std::string> entries;
		std::size_t malformed{ 0 };
		std::size_t duplicates{ 0 };
	};

	using Cache = std::unordered_map<std::string, std::optional<Table>>;

	// Converts translation key letters to lowercase for lookup.
	[[nodiscard]] inline std::string MakeKey(std::string_view a_key)
	{
		std::string key(a_key);
		for (auto& character : key) {
			if (character >= 'A' && character <= 'Z') {
				character = static_cast<char>(character + ('a' - 'A'));
			}
		}
		return key;
	}

	// Checks whether configured text names a translation key.
	[[nodiscard]] inline bool IsKey(std::string_view a_text) noexcept
	{
		return a_text.size() > 1 && a_text[0] == '$' && a_text[1] != '$';
	}

	// Checks a game or configured language suffix.
	[[nodiscard]] inline bool IsLanguage(std::string_view a_language) noexcept
	{
		if (a_language.empty() || a_language.size() > 16) {
			return false;
		}
		for (const auto character : a_language) {
			if (!(character >= 'a' && character <= 'z') && !(character >= 'A' && character <= 'Z') &&
				!(character >= '0' && character <= '9') && character != '_' && character != '-') {
				return false;
			}
		}
		return true;
	}

	// Builds a Data-relative translation resource stem without its language suffix.
	[[nodiscard]] inline std::optional<std::string> CleanResourcePath(std::string_view a_path)
	{
		while (!a_path.empty() && (a_path.front() == ' ' || a_path.front() == '\t')) {
			a_path.remove_prefix(1);
		}
		while (!a_path.empty() && (a_path.back() == ' ' || a_path.back() == '\t')) {
			a_path.remove_suffix(1);
		}
		std::string path(a_path);
		for (auto& character : path) {
			if (character == '/') {
				character = '\\';
			}
		}
		const auto startsWith = [&path](std::string_view a_prefix) {
			return path.size() >= a_prefix.size() && MakeKey(std::string_view(path).substr(0, a_prefix.size())) == a_prefix;
		};
		if (startsWith("data\\")) {
			path.erase(0, 5);
		}
		if (path.empty() || path.size() > 200 || startsWith("\\") || path.back() == '\\' ||
			(path.size() >= 4 && MakeKey(std::string_view(path).substr(path.size() - 4)) == ".txt")) {
			return std::nullopt;
		}
		std::string_view rest(path);
		while (!rest.empty()) {
			const auto separator = rest.find('\\');
			const auto part = rest.substr(0, separator);
			if (part.empty() || part == "." || part == "..") {
				return std::nullopt;
			}
			rest = separator == std::string_view::npos ? std::string_view{} : rest.substr(separator + 1);
		}
		for (const auto character : path) {
			const auto value = static_cast<unsigned char>(character);
			if (value < 0x20 || character == ':' || character == '*' || character == '?' || character == '"' ||
				character == '<' || character == '>' || character == '|') {
				return std::nullopt;
			}
		}
		return path;
	}

	// Builds the translation resource path for one language.
	[[nodiscard]] inline std::string MakePath(std::string_view a_stem, std::string_view a_language)
	{
		std::string path(a_stem);
		path.push_back('_');
		path.append(a_language);
		path.append(".txt");
		return path;
	}

	// Converts UTF-16 text to UTF-8 and rejects text that cannot be stored safely.
	[[nodiscard]] inline bool ConvertText(std::u16string_view a_text, std::string& a_result)
	{
		a_result.clear();
		a_result.reserve(a_text.size());
		for (std::size_t i = 0; i < a_text.size(); ++i) {
			char32_t value = a_text[i];
			if (value == 0 || (value >= 0xDC00 && value <= 0xDFFF)) {
				return false;
			}
			if (value >= 0xD800 && value <= 0xDBFF) {
				if (i + 1 >= a_text.size() || a_text[i + 1] < 0xDC00 || a_text[i + 1] > 0xDFFF) {
					return false;
				}
				value = 0x10000 + ((value - 0xD800) << 10) + (a_text[++i] - 0xDC00);
			}
			if (value < 0x80) {
				a_result.push_back(static_cast<char>(value));
			} else if (value < 0x800) {
				a_result.push_back(static_cast<char>(0xC0 | (value >> 6)));
				a_result.push_back(static_cast<char>(0x80 | (value & 0x3F)));
			} else if (value < 0x10000) {
				a_result.push_back(static_cast<char>(0xE0 | (value >> 12)));
				a_result.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3F)));
				a_result.push_back(static_cast<char>(0x80 | (value & 0x3F)));
			} else {
				a_result.push_back(static_cast<char>(0xF0 | (value >> 18)));
				a_result.push_back(static_cast<char>(0x80 | ((value >> 12) & 0x3F)));
				a_result.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3F)));
				a_result.push_back(static_cast<char>(0x80 | (value & 0x3F)));
			}
		}
		return true;
	}

	// Reads a UTF-16 LE interface translation file using the same row rules as F4SE.
	[[nodiscard]] inline std::optional<Table> ParseTable(std::span<const std::byte> a_data)
	{
		if (a_data.size() < 2 || a_data.size() > kMaxTableSize || a_data[0] != std::byte{ 0xFF } || a_data[1] != std::byte{ 0xFE }) {
			return std::nullopt;
		}
		std::u16string units((a_data.size() - 2) / 2, u'\0');
		for (std::size_t i = 0; i < units.size(); ++i) {
			units[i] = static_cast<char16_t>(std::to_integer<std::uint16_t>(a_data[2 + i * 2]) |
				(std::to_integer<std::uint16_t>(a_data[3 + i * 2]) << 8));
		}

		Table table;
		table.malformed = a_data.size() % 2;
		std::u16string_view text(units);
		std::string key;
		std::string value;
		while (!text.empty()) {
			const auto newline = text.find(u'\n');
			auto line = text.substr(0, newline);
			text = newline == std::u16string_view::npos ? std::u16string_view{} : text.substr(newline + 1);
			if (!line.empty() && line.back() == u'\r') {
				line.remove_suffix(1);
			}
			if (line.empty() || line.front() != u'$') {
				continue;
			}
			const auto tab = line.rfind(u'\t');
			if (tab == std::u16string_view::npos || tab < 2 || !ConvertText(line.substr(0, tab), key) ||
				!ConvertText(line.substr(tab + 1), value)) {
				++table.malformed;
				continue;
			}
			if (!table.entries.try_emplace(MakeKey(key), value).second) {
				++table.duplicates;
			}
		}
		return table;
	}

	[[nodiscard]] std::string GetLanguage();
	[[nodiscard]] const Table* Load(Cache& a_cache, const std::string& a_path);
}
