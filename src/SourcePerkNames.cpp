// Perk Conditions Framework
// SilentlyGayming
// SourcePerkNames.cpp

#include "PCH.h"
#include "SourcePerkNames.h"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace SourceCore
{
	class ByteReader
	{
	public:
		// Releases a source-name file reader.
		virtual ~ByteReader() = default;
		[[nodiscard]] virtual std::uint64_t Size() const noexcept = 0;
		[[nodiscard]] virtual bool ReadAt(std::uint64_t a_offset, void* a_buffer, std::size_t a_size) const = 0;
	};

	struct PluginMetadata
	{
		bool localized{ false };
		std::uint32_t masterCount{ 0 };
		std::uint64_t recordsOffset{ 0 };
	};

	struct RecordRequest
	{
		std::uint32_t localFormID{ 0 };
		std::uintptr_t token{ 0 };
	};

	class StringTable
	{
	public:
		[[nodiscard]] static std::optional<StringTable> Load(const ByteReader& a_reader);
		[[nodiscard]] std::optional<std::string> Lookup(std::uint32_t a_id) const;

	private:
		std::unordered_map<std::uint32_t, std::uint32_t> _offsets;
		std::vector<std::byte> _data;
	};

	struct ScanResult
	{
		std::unordered_map<std::uintptr_t, std::string> names;
	};

	[[nodiscard]] std::optional<PluginMetadata> ReadPluginInfo(const ByteReader& a_reader);
	[[nodiscard]] ScanResult ReadPerkNames(const ByteReader& a_reader, const PluginMetadata& a_metadata,
		std::span<const RecordRequest> a_requests, const StringTable* a_strings);
}

namespace
{
	// Converts a four-letter record name into its numeric tag.
	constexpr std::uint32_t MakeRecordTag(char a_a, char a_b, char a_c, char a_d) noexcept
	{
		return static_cast<std::uint32_t>(static_cast<unsigned char>(a_a)) |
			(static_cast<std::uint32_t>(static_cast<unsigned char>(a_b)) << 8) |
			(static_cast<std::uint32_t>(static_cast<unsigned char>(a_c)) << 16) |
			(static_cast<std::uint32_t>(static_cast<unsigned char>(a_d)) << 24);
	}

	constexpr auto kTES4 = MakeRecordTag('T', 'E', 'S', '4');
	constexpr auto kGRUP = MakeRecordTag('G', 'R', 'U', 'P');
	constexpr auto kPERK = MakeRecordTag('P', 'E', 'R', 'K');
	constexpr auto kMAST = MakeRecordTag('M', 'A', 'S', 'T');
	constexpr auto kFULL = MakeRecordTag('F', 'U', 'L', 'L');
	constexpr auto kXXXX = MakeRecordTag('X', 'X', 'X', 'X');
	constexpr std::uint32_t kLocalizedFlag = 0x00000080;
	constexpr std::uint32_t kCompressedFlag = 0x00040000;
	constexpr std::uint64_t kRecordHeaderSize = 24;
	constexpr std::uint64_t kGroupHeaderSize = 24;
	constexpr std::uint64_t kMaxRecordPayload = 64ull * 1024ull * 1024ull;
	constexpr std::uint64_t kMaxStringTableSize = 256ull * 1024ull * 1024ull;

	struct RecordHeader
	{
		std::uint32_t type{ 0 };
		std::uint32_t dataSize{ 0 };
		std::uint32_t flags{ 0 };
		std::uint32_t formID{ 0 };
	};

	// Reads one simple value from a resource file.
	template <class T>
	bool ReadStreamValue(const SourceCore::ByteReader& a_reader, std::uint64_t a_offset, T& a_value)
	{
		static_assert(std::is_trivially_copyable_v<T>);
		return a_reader.ReadAt(a_offset, std::addressof(a_value), sizeof(T));
	}

	// Reads one simple value from memory.
	template <class T>
	bool ReadMemoryValue(std::span<const std::byte> a_data, std::size_t a_offset, T& a_value)
	{
		static_assert(std::is_trivially_copyable_v<T>);
		if (a_offset > a_data.size() || sizeof(T) > a_data.size() - a_offset) {
			return false;
		}
		std::memcpy(std::addressof(a_value), a_data.data() + a_offset, sizeof(T));
		return true;
	}

	// Reads one Bethesda record header from a plugin.
	bool ReadHeader(const SourceCore::ByteReader& a_reader, std::uint64_t a_offset, RecordHeader& a_header)
	{
		std::array<std::byte, kRecordHeaderSize> bytes{};
		if (!a_reader.ReadAt(a_offset, bytes.data(), bytes.size())) {
			return false;
		}
		return ReadMemoryValue(bytes, 0, a_header.type) && ReadMemoryValue(bytes, 4, a_header.dataSize) &&
			ReadMemoryValue(bytes, 8, a_header.flags) && ReadMemoryValue(bytes, 12, a_header.formID);
	}

	// Reads one plugin record and decompresses it when needed.
	std::optional<std::vector<std::byte>> ReadRecordPayload(const SourceCore::ByteReader& a_reader,
		std::uint64_t a_dataOffset, std::uint32_t a_dataSize, std::uint32_t a_flags)
	{
		if (a_dataSize > kMaxRecordPayload || a_dataOffset > a_reader.Size() || a_dataSize > a_reader.Size() - a_dataOffset) {
			return std::nullopt;
		}
		std::vector<std::byte> stored(a_dataSize);
		if (a_dataSize && !a_reader.ReadAt(a_dataOffset, stored.data(), stored.size())) {
			return std::nullopt;
		}
		if ((a_flags & kCompressedFlag) == 0) {
			return stored;
		}
		if (stored.size() < sizeof(std::uint32_t)) {
			return std::nullopt;
		}
		std::uint32_t uncompressedSize = 0;
		std::memcpy(std::addressof(uncompressedSize), stored.data(), sizeof(uncompressedSize));
		if (!uncompressedSize || uncompressedSize > kMaxRecordPayload) {
			return std::nullopt;
		}
		std::vector<std::byte> result(uncompressedSize);
		uLongf destinationSize = static_cast<uLongf>(result.size());
		const auto* source = reinterpret_cast<const Bytef*>(stored.data() + sizeof(std::uint32_t));
		const auto sourceSize = static_cast<uLong>(stored.size() - sizeof(std::uint32_t));
		const auto status = ::uncompress(reinterpret_cast<Bytef*>(result.data()), std::addressof(destinationSize), source, sourceSize);
		if (status != Z_OK || destinationSize != result.size()) {
			return std::nullopt;
		}
		return result;
	}

	// Reads a PERK FULL name from one plugin record.
	std::optional<std::string> ReadFullSubrecord(std::span<const std::byte> a_payload, bool a_localized,
		const SourceCore::StringTable* a_strings)
	{
		std::size_t cursor = 0;
		std::optional<std::uint32_t> extendedSize;
		while (cursor + 6 <= a_payload.size()) {
			std::uint32_t type = 0;
			std::uint16_t declaredSize = 0;
			if (!ReadMemoryValue(a_payload, cursor, type) || !ReadMemoryValue(a_payload, cursor + 4, declaredSize)) {
				return std::nullopt;
			}
			cursor += 6;
			std::uint32_t size = declaredSize;
			if (type == kXXXX) {
				if (declaredSize != sizeof(std::uint32_t) || cursor + sizeof(std::uint32_t) > a_payload.size()) {
					return std::nullopt;
				}
				std::uint32_t value = 0;
				std::memcpy(std::addressof(value), a_payload.data() + cursor, sizeof(value));
				extendedSize = value;
				cursor += declaredSize;
				continue;
			}
			if (extendedSize) {
				size = *extendedSize;
				extendedSize.reset();
			}
			if (cursor > a_payload.size() || size > a_payload.size() - cursor) {
				return std::nullopt;
			}
			if (type == kFULL) {
				if (a_localized) {
					if (size != sizeof(std::uint32_t) || !a_strings) {
						return std::nullopt;
					}
					std::uint32_t stringID = 0;
					std::memcpy(std::addressof(stringID), a_payload.data() + cursor, sizeof(stringID));
					return a_strings->Lookup(stringID);
				}
				const auto* text = reinterpret_cast<const char*>(a_payload.data() + cursor);
				std::size_t length = 0;
				while (length < size && text[length] != '\0') {
					++length;
				}
				if (!length) {
					return std::nullopt;
				}
				return std::string(text, length);
			}
			cursor += size;
		}
		return std::nullopt;
	}
}

namespace SourceCore
{
	// Loads a STRINGS file into a lookup table.
	std::optional<StringTable> StringTable::Load(const ByteReader& a_reader)
	{
		if (a_reader.Size() < 8 || a_reader.Size() > kMaxStringTableSize) {
			return std::nullopt;
		}
		std::uint32_t count = 0;
		std::uint32_t dataSize = 0;
		if (!ReadStreamValue(a_reader, 0, count) || !ReadStreamValue(a_reader, 4, dataSize)) {
			return std::nullopt;
		}
		const auto indexSize = static_cast<std::uint64_t>(count) * 8ull;
		const auto dataOffset = 8ull + indexSize;
		if (dataOffset > a_reader.Size() || dataSize > a_reader.Size() - dataOffset) {
			return std::nullopt;
		}
		StringTable result;
		result._offsets.reserve(count);
		for (std::uint32_t i = 0; i < count; ++i) {
			std::uint32_t id = 0;
			std::uint32_t offset = 0;
			if (!ReadStreamValue(a_reader, 8ull + static_cast<std::uint64_t>(i) * 8ull, id) ||
				!ReadStreamValue(a_reader, 12ull + static_cast<std::uint64_t>(i) * 8ull, offset) || offset >= dataSize) {
				return std::nullopt;
			}
			result._offsets.try_emplace(id, offset);
		}
		result._data.resize(dataSize);
		if (dataSize && !a_reader.ReadAt(dataOffset, result._data.data(), result._data.size())) {
			return std::nullopt;
		}
		return result;
	}

	// Finds one localized string by its ID.
	std::optional<std::string> StringTable::Lookup(std::uint32_t a_id) const
	{
		const auto it = _offsets.find(a_id);
		if (it == _offsets.end() || it->second >= _data.size()) {
			return std::nullopt;
		}
		const auto start = static_cast<std::size_t>(it->second);
		std::size_t end = start;
		while (end < _data.size() && _data[end] != std::byte{}) {
			++end;
		}
		if (end == _data.size() || end == start) {
			return std::nullopt;
		}
		return std::string(reinterpret_cast<const char*>(_data.data() + start), end - start);
	}

	// Reads the TES4 flags and master list needed to identify local perks.
	std::optional<PluginMetadata> ReadPluginInfo(const ByteReader& a_reader)
	{
		if (a_reader.Size() < kRecordHeaderSize) {
			return std::nullopt;
		}
		RecordHeader header;
		if (!ReadHeader(a_reader, 0, header) || header.type != kTES4 ||
			header.dataSize > a_reader.Size() - kRecordHeaderSize) {
			return std::nullopt;
		}
		auto payload = ReadRecordPayload(a_reader, kRecordHeaderSize, header.dataSize, header.flags);
		if (!payload) {
			return std::nullopt;
		}
		std::uint32_t masterCount = 0;
		std::size_t cursor = 0;
		std::optional<std::uint32_t> extendedSize;
		while (cursor + 6 <= payload->size()) {
			std::uint32_t type = 0;
			std::uint16_t declaredSize = 0;
			if (!ReadMemoryValue(*payload, cursor, type) || !ReadMemoryValue(*payload, cursor + 4, declaredSize)) {
				return std::nullopt;
			}
			cursor += 6;
			std::uint32_t size = declaredSize;
			if (type == kXXXX) {
				if (declaredSize != 4 || cursor + 4 > payload->size()) {
					return std::nullopt;
				}
				std::uint32_t value = 0;
				std::memcpy(std::addressof(value), payload->data() + cursor, sizeof(value));
				extendedSize = value;
				cursor += declaredSize;
				continue;
			}
			if (extendedSize) {
				size = *extendedSize;
				extendedSize.reset();
			}
			if (cursor > payload->size() || size > payload->size() - cursor) {
				return std::nullopt;
			}
			if (type == kMAST) {
				++masterCount;
			}
			cursor += size;
		}
		return PluginMetadata{
			(header.flags & kLocalizedFlag) != 0,
			masterCount,
			kRecordHeaderSize + header.dataSize
		};
	}

	// Finds the original FULL names for requested perks in a plugin.
	ScanResult ReadPerkNames(const ByteReader& a_reader, const PluginMetadata& a_metadata,
		std::span<const RecordRequest> a_requests, const StringTable* a_strings)
	{
		ScanResult result;
		if (a_metadata.localized && !a_strings) {
			return result;
		}
		std::unordered_map<std::uint32_t, std::vector<std::uintptr_t>> requested;
		requested.reserve(a_requests.size());
		for (const auto& request : a_requests) {
			requested[request.localFormID & 0x00FFFFFF].push_back(request.token);
		}
		std::unordered_set<std::uint32_t> resolvedIDs;
		std::uint64_t cursor = a_metadata.recordsOffset;
		while (cursor + 4 <= a_reader.Size() && resolvedIDs.size() < requested.size()) {
			std::uint32_t type = 0;
			if (!ReadStreamValue(a_reader, cursor, type)) {
				break;
			}
			if (type == kGRUP) {
				if (cursor + kGroupHeaderSize > a_reader.Size()) {
					break;
				}
				std::uint32_t groupSize = 0;
				if (!ReadStreamValue(a_reader, cursor + 4, groupSize) || groupSize < kGroupHeaderSize || groupSize > a_reader.Size() - cursor) {
					break;
				}
				cursor += kGroupHeaderSize;
				continue;
			}
			if (cursor + kRecordHeaderSize > a_reader.Size()) {
				break;
			}
			RecordHeader header;
			if (!ReadHeader(a_reader, cursor, header) || header.dataSize > a_reader.Size() - cursor - kRecordHeaderSize) {
				break;
			}
			const auto dataOffset = cursor + kRecordHeaderSize;
			if (header.type == kPERK && (header.formID >> 24) == a_metadata.masterCount) {
				const auto localID = header.formID & 0x00FFFFFF;
				if (const auto it = requested.find(localID); it != requested.end()) {
					if (auto payload = ReadRecordPayload(a_reader, dataOffset, header.dataSize, header.flags)) {
						if (auto name = ReadFullSubrecord(*payload, a_metadata.localized, a_strings); name && !name->empty()) {
							for (const auto token : it->second) {
								result.names.try_emplace(token, *name);
							}
							resolvedIDs.insert(localID);
						}
					}
				}
			}
			cursor = dataOffset + header.dataSize;
		}
		return result;
	}
}


namespace
{
	class ResourceByteReader final : public SourceCore::ByteReader
	{
	public:
		// Opens a plugin or STRINGS file through Fallout resource I/O.
		explicit ResourceByteReader(const std::string& a_path) :
			_stream(std::make_unique<RE::BSResourceNiBinaryStream>(a_path))
		{
			if (_stream && static_cast<bool>(*_stream) && _stream->stream) {
				_size = _stream->stream->totalSize;
			}
		}

		// Checks whether the resource file is open and readable.
		[[nodiscard]] bool Good() const noexcept { return _stream && static_cast<bool>(*_stream) && _stream->stream && _size; }
		// Gets the readable resource size.
		[[nodiscard]] std::uint64_t Size() const noexcept override { return _size; }

		// Reads an exact range of bytes from the resource file.
		[[nodiscard]] bool ReadAt(std::uint64_t a_offset, void* a_buffer, std::size_t a_size) const override
		{
			if (!Good() || !a_buffer || a_offset > _size || a_size > _size - a_offset) {
				return false;
			}

			const auto current = static_cast<std::uint64_t>(_stream->GetPosition());
			if (a_offset != current) {
				constexpr auto maxDelta = static_cast<std::uint64_t>(std::numeric_limits<std::ptrdiff_t>::max());
				if (a_offset > current) {
					const auto distance = a_offset - current;
					if (distance > maxDelta) {
						return false;
					}
					_stream->Seek(static_cast<std::ptrdiff_t>(distance));
				} else {
					const auto distance = current - a_offset;
					if (distance > maxDelta) {
						return false;
					}
					_stream->Seek(-static_cast<std::ptrdiff_t>(distance));
				}
				if (_stream->GetPosition() != a_offset) {
					return false;
				}
			}

			return _stream->binary_read(a_buffer, a_size) == a_size;
		}

	private:
		std::unique_ptr<RE::BSResourceNiBinaryStream> _stream;
		std::uint64_t _size{ 0 };
	};

	// Converts a plugin name to lowercase for lookup.
	std::string ToLower(std::string_view a_value)
	{
		std::string result(a_value);
		std::transform(result.begin(), result.end(), result.begin(), [](char a_ch) {
			return static_cast<char>(std::tolower(static_cast<unsigned char>(a_ch)));
		});
		return result;
	}

	// Gets the active Fallout language.
	std::string GetLanguage()
	{
		auto* settings = RE::INISettingCollection::GetSingleton();
		auto* language = settings ? settings->GetSetting("sLanguage:General") : nullptr;
		if (!language || language->GetType() != RE::Setting::SETTING_TYPE::kString) {
			return {};
		}
		return std::string(language->GetString());
	}

	// Builds the STRINGS file path for a plugin.
	std::string GetStringsPath(std::string_view a_plugin, std::string_view a_language)
	{
		if (a_plugin.empty() || a_language.empty()) {
			return {};
		}
		const auto separator = a_plugin.find_last_of("/\\");
		const auto filename = separator == std::string_view::npos ? a_plugin : a_plugin.substr(separator + 1);
		const auto dot = filename.find_last_of('.');
		const auto stem = dot == std::string_view::npos ? filename : filename.substr(0, dot);
		return fmt::format("Strings\\{}_{}.STRINGS", stem, a_language);
	}

	struct PluginRequestGroup
	{
		std::string filename;
		std::vector<SourceCore::RecordRequest> requests;
	};
}

namespace PCF::SourcePerkNames
{
	// Loads the original FULL names for configured source perks.
	Result LoadNames(const std::vector<RE::BGSPerk*>& a_sources)
	{
		Result result;
		std::unordered_map<std::string, PluginRequestGroup> groups;
		groups.reserve(a_sources.size());
		for (auto* source : a_sources) {
			if (!source) {
				continue;
			}
			auto* file = source->GetFile(0);
			if (!file || file->GetFilename().empty()) {
				continue;
			}
			const auto filename = std::string(file->GetFilename());
			auto& group = groups[ToLower(filename)];
			if (group.filename.empty()) {
				group.filename = filename;
			}
			group.requests.push_back({ source->GetLocalFormID(), reinterpret_cast<std::uintptr_t>(source) });
		}
		const auto language = GetLanguage();
		for (auto& [key, group] : groups) {
			(void)key;
			ResourceByteReader plugin(group.filename);
			if (!plugin.Good()) {
				continue;
			}
			const auto metadata = SourceCore::ReadPluginInfo(plugin);
			if (!metadata) {
				continue;
			}
			std::optional<SourceCore::StringTable> strings;
			if (metadata->localized) {
				const auto path = GetStringsPath(group.filename, language);
				if (path.empty()) {
					continue;
				}
				ResourceByteReader stringsFile(path);
				if (!stringsFile.Good()) {
					continue;
				}
				strings = SourceCore::StringTable::Load(stringsFile);
				if (!strings) {
					continue;
				}
			}
			const auto scan = SourceCore::ReadPerkNames(plugin, *metadata, group.requests,
				strings ? std::addressof(*strings) : nullptr);
			for (const auto& [token, name] : scan.names) {
				auto* source = reinterpret_cast<RE::BGSPerk*>(token);
				if (source && !name.empty()) {
					result.names.try_emplace(source, name);
				}
			}
		}
		return result;
	}
}
