// Perk Conditions Framework
// SilentlyGayming
// Localization.cpp

#include "PCH.h"
#include "Localization.h"

#include <memory>
#include <type_traits>
#include <vector>

namespace PCF::Localization
{
	// Gets the active Fallout language.
	std::string GetLanguage()
	{
		auto* settings = RE::INISettingCollection::GetSingleton();
		auto* language = settings ? settings->GetSetting("sLanguage:General") : nullptr;
		if (!language || language->GetType() != RE::Setting::SETTING_TYPE::kString) {
			return {};
		}
		const auto value = language->GetString();
		if constexpr (std::is_pointer_v<decltype(value)>) {
			return value ? std::string(value) : std::string{};
		} else {
			return std::string(value);
		}
	}

	// Loads a translation resource once through Fallout resource I/O.
	const Table* Load(Cache& a_cache, const std::string& a_path)
	{
		auto [entry, inserted] = a_cache.try_emplace(MakeKey(a_path));
		if (!inserted) {
			return entry->second ? std::addressof(*entry->second) : nullptr;
		}

		const auto stream = std::make_unique<RE::BSResourceNiBinaryStream>(a_path.c_str());
		const auto size = static_cast<bool>(*stream) && stream->stream ? static_cast<std::uint64_t>(stream->stream->totalSize) : 0;
		std::vector<std::byte> data;
		std::string_view failure;
		if (!static_cast<bool>(*stream) || !stream->stream) {
			failure = "not found";
		} else if (!size) {
			failure = "empty file";
		} else if (size > kMaxTableSize) {
			failure = "file exceeds 16 MiB";
		} else {
			data.resize(static_cast<std::size_t>(size));
			if (stream->BinaryRead(data.data(), data.size()) != data.size()) {
				failure = "read failure";
			} else if (!(entry->second = ParseTable(data))) {
				failure = "expected UTF-16 LE with a byte order mark";
			}
		}
		if (!failure.empty()) {
			spdlog::warn("Localization: {} unavailable ({})", a_path, failure);
			return nullptr;
		}
		if (entry->second->malformed || entry->second->duplicates) {
			spdlog::warn("Localization: {}: ignored {} malformed row(s) and {} duplicate key(s); the first entry for each key is used",
				a_path, entry->second->malformed, entry->second->duplicates);
		}
		spdlog::info("Localization: loaded {} key(s) from {}", entry->second->entries.size(), a_path);
		return std::addressof(*entry->second);
	}
}
