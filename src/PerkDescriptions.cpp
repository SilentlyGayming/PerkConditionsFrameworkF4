// Perk Conditions Framework
// SilentlyGayming
// PerkDescriptions.cpp

#include "PCH.h"
#include "PerkDescriptions.h"
#include "EngineIDs.h"
#include "NativeHooks.h"
#include "PerkConditions.h"
#include "RuleRegistry.h"
#include "UICommon.h"

#ifndef NOMINMAX
#	define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>

namespace
{
	using DescriptionFunction = void (*)(RE::TESDescription*, RE::BSStringT<char>&, const RE::TESForm*);
	using PipboyUpdateFunction = void (*)(RE::PipboyPerksMenu*);
	using Value = RE::Scaleform::GFx::Value;

	DescriptionFunction g_descriptionOriginal{ nullptr };
	PipboyUpdateFunction g_pipboyUpdateOriginal{ nullptr };
	std::uintptr_t g_pipboyVtableAddress{ 0 };
	bool g_descriptionHookAttempted{ false };
	bool g_pipboyHookAttempted{ false };
	bool g_descriptionHookInstalled{ false };
	bool g_pipboyHookInstalled{ false };
	bool g_descriptionBehaviorActive{ false };
	constexpr std::size_t kTrampolineSize = 128;
	constexpr std::size_t kPipboyUpdateSlot = 2;

	inline constexpr std::uint32_t kPRKFImageTimestamp = 0x6A9D7778;
	inline constexpr std::uint32_t kPRKFImageSize = 0x0007C000;
	inline constexpr std::uintptr_t kPRKFDescriptionResolverRVA = 0x14D34;
	inline constexpr std::uintptr_t kPRKFDescriptionFormArgRVA = 0x14D49;
	bool g_prkfDescriptionBridgeAttempted{ false };
	bool g_prkfDescriptionBridgeInstalled{ false };
	std::unordered_set<std::uint32_t> g_prkfDescriptionLogged;
	std::unordered_set<std::uint32_t> g_prkfDescriptionMissLogged;

	struct BSStringStorage
	{
		char* data{ nullptr };
		std::uint16_t size{ 0 };
		std::uint16_t capacity{ 0 };
	};

	static_assert(sizeof(BSStringStorage) == sizeof(RE::BSStringT<char>));

	// Replaces a game string while keeping Bethesda memory ownership intact.
	bool SetDescription(RE::BSStringT<char>& a_output, std::string_view a_description)
	{
		if (a_description.size() >= (std::numeric_limits<std::uint16_t>::max)()) {
			return false;
		}
		auto* replacement = RE::calloc<char>(a_description.size() + 1);
		if (!replacement) {
			return false;
		}
		std::memcpy(replacement, a_description.data(), a_description.size());

		BSStringStorage storage;
		std::memcpy(std::addressof(storage), std::addressof(a_output), sizeof(storage));
		RE::free(storage.data);
		storage.data = replacement;
		storage.size = static_cast<std::uint16_t>(a_description.size());
		storage.capacity = static_cast<std::uint16_t>(a_description.size() + 1);
		std::memcpy(std::addressof(a_output), std::addressof(storage), sizeof(storage));
		return true;
	}

	// Checks whether one description rule is currently active.
	bool IsDescriptionRuleActive(const PCF::DescriptionRule& a_rule)
	{
		switch (a_rule.type) {
		case PCF::AlternativeType::kGlobalValue: {
			if (!a_rule.globalValue) {
				return false;
			}
			const auto value = a_rule.globalValue->GetValue();
			return std::isfinite(value) && PCF::CheckComparison(value, a_rule.comparison, a_rule.requiredValue);
		}
		case PCF::AlternativeType::kActorValue: {
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player || !a_rule.actorValue) {
				return false;
			}
			const auto value = player->GetActorValue(*a_rule.actorValue);
			return std::isfinite(value) && PCF::CheckComparison(value, a_rule.comparison, a_rule.requiredValue);
		}
		case PCF::AlternativeType::kPerk: {
			auto* player = RE::PlayerCharacter::GetSingleton();
			const auto value = PCF::PerkConditions::GetPlayerPerkRank(player, a_rule.perk);
			return PCF::CheckComparison(value, a_rule.comparison, a_rule.requiredValue);
		}
		}
		return false;
	}

	// Finds the first active custom description for a perk.
	const std::string* FindActiveDescription(RE::BGSPerk* a_perk)
	{
		const auto* rules = a_perk ? PCF::RuleRegistry::GetDescriptions(a_perk) : nullptr;
		if (!rules) {
			return nullptr;
		}
		for (const auto& rule : *rules) {
			if (IsDescriptionRuleActive(rule)) {
				return std::addressof(rule.description);
			}
		}
		return nullptr;
	}

	// Compares two text keys without caring about letter case.
	bool SameText(std::string_view a_left, std::string_view a_right)
	{
		return a_left.size() == a_right.size() && std::ranges::equal(a_left, a_right, [](char a_leftChar, char a_rightChar) {
			const auto left = a_leftChar >= 'A' && a_leftChar <= 'Z' ? static_cast<char>(a_leftChar + ('a' - 'A')) : a_leftChar;
			const auto right = a_rightChar >= 'A' && a_rightChar <= 'Z' ? static_cast<char>(a_rightChar + ('a' - 'A')) : a_rightChar;
			return left == right;
		});
	}

	// Builds the text key for one Scaleform array item.
	bool MakeArrayKey(std::uint32_t a_index, std::array<char, 11>& a_key)
	{
		const auto [end, error] = std::to_chars(a_key.data(), a_key.data() + a_key.size() - 1, a_index);
		if (error != std::errc{}) {
			return false;
		}
		*end = '\0';
		return true;
	}

	// Gets the size of a Scaleform array.
	bool GetArrayLength(const Value& a_array, std::uint32_t& a_length)
	{
		return a_array.IsArray() && PCF::UICommon::ReadIndex(a_array, "length", a_length);
	}

	// Gets one item from a Scaleform array.
	bool GetArrayElement(const Value& a_array, std::uint32_t a_index, Value& a_value)
	{
		std::array<char, 11> key{};
		return a_array.IsArray() && MakeArrayKey(a_index, key) && a_array.GetMember(key.data(), std::addressof(a_value));
	}

	// Replaces one Pip-Boy description whether it is stored as text or as an object.
	bool SetDescriptionEntry(Value& a_array, std::uint32_t a_index, const std::string& a_description)
	{
		std::array<char, 11> key{};
		Value entry;
		if (!a_array.IsArray() || !MakeArrayKey(a_index, key) || !a_array.GetMember(key.data(), std::addressof(entry))) {
			return false;
		}
		if (entry.IsString()) {
			const char* existing = entry.GetString();
			if (existing && a_description == existing) {
				return true;
			}
			return a_array.SetMember(key.data(), Value(a_description.c_str()));
		}
		if (entry.IsObject()) {
			Value text;
			if (entry.GetMember("text", std::addressof(text)) && text.IsString()) {
				const char* existing = text.GetString();
				if (existing && a_description == existing) {
					return true;
				}
			}
			return entry.SetMember("text", Value(a_description.c_str()));
		}
		return false;
	}

	// Reads the text from one Pip-Boy description entry.
	bool ReadDescriptionEntry(const Value& a_array, std::uint32_t a_index, std::string& a_text)
	{
		Value entry;
		if (!GetArrayElement(a_array, a_index, entry)) {
			return false;
		}
		if (entry.IsString()) {
			const char* text = entry.GetString();
			if (!text) {
				return false;
			}
			a_text = text;
			return true;
		}
		return entry.IsObject() && PCF::UICommon::ReadText(entry, "text", a_text);
	}

	// Gets the original description used to match the exact perk rank in the Pip-Boy.
	std::string GetOriginalDescription(RE::BGSPerk* a_perk)
	{
		if (!g_descriptionOriginal || !a_perk) {
			return {};
		}
		RE::BSStringT<char> native;
		g_descriptionOriginal(static_cast<RE::TESDescription*>(a_perk), native, a_perk);
		return native.c_str() ? std::string(native.c_str(), native.size()) : std::string{};
	}

	// Checks whether a Pip-Boy row belongs to the configured perk family.
	bool RowMatchesPerk(const Value& a_row, RE::BGSPerk* a_perk)
	{
		if (!a_perk || !a_row.IsObject()) {
			return false;
		}
		const char* nativeSWF = a_perk->swfFile.c_str();
		const char* nativeClip = a_perk->textureName.c_str();
		bool compared = false;
		std::string value;
		if (nativeSWF && *nativeSWF) {
			compared = true;
			if (!PCF::UICommon::ReadText(a_row, "SWFFile", value) || !SameText(value, nativeSWF)) {
				return false;
			}
		}
		if (nativeClip && *nativeClip) {
			compared = true;
			if (!PCF::UICommon::ReadText(a_row, "clipName", value) || !SameText(value, nativeClip)) {
				return false;
			}
		}
		return compared;
	}

	// Finds the matching perk-rank entry in the Pip-Boy description list.
	std::optional<std::uint32_t> FindDescriptionEntry(const Value& a_descriptions, RE::BGSPerk* a_source)
	{
		std::uint32_t count = 0;
		if (!a_source || !GetArrayLength(a_descriptions, count) || count == 0) {
			return std::nullopt;
		}
		const auto native = GetOriginalDescription(a_source);
		if (!native.empty()) {
			std::string candidate;
			for (std::uint32_t index = 0; index < count; ++index) {
				if (ReadDescriptionEntry(a_descriptions, index, candidate) && candidate == native) {
					return index;
				}
			}
		}
		const auto rank = PCF::RuleRegistry::GetRank(a_source);
		if (rank.rank > 0 && rank.rank <= count) {
			return rank.rank - 1;
		}
		return count == 1 ? std::optional<std::uint32_t>{ 0 } : std::nullopt;
	}

	// Updates the configured rank description in one Pip-Boy perk row.
	bool UpdatePipboyRow(Value& a_row, RE::BGSPerk* a_source, const std::string& a_description)
	{
		Value descriptions;
		std::uint32_t count = 0;
		if (!a_row.IsObject() || !a_row.GetMember("descriptions", std::addressof(descriptions)) || !GetArrayLength(descriptions, count) || count == 0) {
			return false;
		}
		const auto rank = PCF::RuleRegistry::GetRank(a_source);
		const auto* family = rank.base ? rank.base : a_source;
		if (family == a_source && !a_source->nextPerk && a_source->data.numRanks > 1) {
			bool changed = false;
			for (std::uint32_t index = 0; index < count; ++index) {
				changed = SetDescriptionEntry(descriptions, index, a_description) || changed;
			}
			return changed;
		}
		const auto index = FindDescriptionEntry(descriptions, a_source);
		return index && SetDescriptionEntry(descriptions, *index, a_description);
	}

	// Applies active custom descriptions after the game builds the Pip-Boy perk list.
	void UpdatePipboyDescriptions(RE::PipboyPerksMenu* a_menu)
	{
		if (!a_menu || !a_menu->dataObj.IsObject()) {
			return;
		}
		Value perks;
		std::uint32_t rowCount = 0;
		if (!a_menu->dataObj.GetMember("PerksList", std::addressof(perks)) || !GetArrayLength(perks, rowCount)) {
			return;
		}
		for (auto* source : PCF::RuleRegistry::GetDescriptionSources()) {
			const auto* description = FindActiveDescription(source);
			if (!description) {
				continue;
			}
			const auto rank = PCF::RuleRegistry::GetRank(source);
			auto* family = rank.base ? rank.base : source;
			bool rewritten = false;
			for (std::uint32_t rowIndex = 0; rowIndex < rowCount; ++rowIndex) {
				Value row;
				if (!PCF::UICommon::ReadElement(perks, rowIndex, row) || (!RowMatchesPerk(row, source) && !RowMatchesPerk(row, family))) {
					continue;
				}
				if (UpdatePipboyRow(row, source, *description)) {
					rewritten = true;
					break;
				}
			}
			if (rewritten) {
				continue;
			}
			const auto native = GetOriginalDescription(source);
			if (native.empty()) {
				continue;
			}
			std::string candidate;
			for (std::uint32_t rowIndex = 0; rowIndex < rowCount && !rewritten; ++rowIndex) {
				Value row;
				Value descriptions;
				std::uint32_t descriptionCount = 0;
				if (!PCF::UICommon::ReadElement(perks, rowIndex, row) || !row.GetMember("descriptions", std::addressof(descriptions)) ||
					!GetArrayLength(descriptions, descriptionCount)) {
					continue;
				}
				for (std::uint32_t index = 0; index < descriptionCount; ++index) {
					if (ReadDescriptionEntry(descriptions, index, candidate) && candidate == native) {
						rewritten = SetDescriptionEntry(descriptions, index, *description);
						break;
					}
				}
			}
		}
	}

	// Applies a configured replacement after native description resolution.
	bool ApplyConfiguredDescription(RE::TESDescription* a_description, RE::BSStringT<char>& a_output, RE::BGSPerk* a_prkfContext)
	{
		if (!g_descriptionBehaviorActive) {
			return false;
		}
		if (const auto* actorValueDescription = PCF::RuleRegistry::FindActorValueDescription(a_description)) {
			return SetDescription(a_output, *actorValueDescription);
		}

		auto* perk = PCF::RuleRegistry::FindDescriptionPerk(a_description);
		if (!perk && a_prkfContext && PCF::RuleRegistry::GetDescriptions(a_prkfContext)) {
			perk = a_prkfContext;
		}
		const auto* replacement = FindActiveDescription(perk);
		if (!replacement || !SetDescription(a_output, *replacement)) {
			return false;
		}

		if (a_prkfContext && perk == a_prkfContext && g_prkfDescriptionLogged.insert(perk->GetFormID()).second) {
			spdlog::info("Descriptions: PRKF source={:08X} custom description applied", perk->GetFormID());
		}
		return true;
	}

	// Replaces configured text through the shared native TESDescription path.
	void DescriptionHook(RE::TESDescription* a_description, RE::BSStringT<char>& a_output, const RE::TESForm* a_form)
	{
		if (!g_descriptionOriginal) {
			return;
		}

		g_descriptionOriginal(a_description, a_output, a_form);
		ApplyConfiguredDescription(a_description, a_output, nullptr);
	}

	// Handles PRKF's main LevelUpMenu description call using the source perk PRKF already keeps in RDI.
	void PRKFDescriptionHook(RE::TESDescription* a_description, RE::BSStringT<char>& a_output, const RE::TESForm* a_form)
	{
		if (!g_descriptionOriginal) {
			return;
		}

		g_descriptionOriginal(a_description, a_output, a_form);
		auto* context = a_form ? const_cast<RE::BGSPerk*>(a_form->As<RE::BGSPerk>()) : nullptr;
		if (ApplyConfiguredDescription(a_description, a_output, context) || !context) {
			return;
		}

		const auto* rules = PCF::RuleRegistry::GetDescriptions(context);
		if (!rules || rules->empty() || !g_prkfDescriptionMissLogged.insert(context->GetFormID()).second) {
			return;
		}

		for (const auto& rule : *rules) {
			float currentValue = 0.0F;
			const char* type = "unknown";
			switch (rule.type) {
			case PCF::AlternativeType::kGlobalValue:
				type = "GlobalValue";
				currentValue = rule.globalValue ? rule.globalValue->GetValue() : 0.0F;
				break;
			case PCF::AlternativeType::kActorValue: {
				type = "ActorValue";
				auto* player = RE::PlayerCharacter::GetSingleton();
				currentValue = player && rule.actorValue ? player->GetActorValue(*rule.actorValue) : 0.0F;
				break;
			}
			case PCF::AlternativeType::kPerk: {
				type = "Perk";
				auto* player = RE::PlayerCharacter::GetSingleton();
				currentValue = static_cast<float>(PCF::PerkConditions::GetPlayerPerkRank(player, rule.perk));
				break;
			}
			}
			spdlog::warn(
				"Descriptions: PRKF source={:08X} configured rule inactive (type={}, current={}, required={})",
				context->GetFormID(), type, currentValue, rule.requiredValue);
		}
	}

	// Routes PRKF's main LevelUpMenu description call through PCF and preserves the source perk as TESDescription context.
	bool InstallPRKFDescriptionBridge()
	{
		if (g_prkfDescriptionBridgeInstalled) {
			return true;
		}

		auto* module = ::GetModuleHandleW(L"PRKF.dll");
		if (!module) {
			return true;
		}
		if (g_prkfDescriptionBridgeAttempted) {
			return false;
		}
		g_prkfDescriptionBridgeAttempted = true;

		const auto base = reinterpret_cast<std::uintptr_t>(module);
		const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
		const auto* nt = dos->e_magic == IMAGE_DOS_SIGNATURE ?
			reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew) : nullptr;
		if (!nt || nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.TimeDateStamp != kPRKFImageTimestamp ||
			nt->OptionalHeader.SizeOfImage != kPRKFImageSize) {
			spdlog::warn("Descriptions: unsupported PRKF.dll build; LevelUpMenu descriptions will use PRKF's original text");
			return false;
		}

		const auto resolverAddress = base + kPRKFDescriptionResolverRVA;
		const auto formArgAddress = base + kPRKFDescriptionFormArgRVA;
		const auto* resolverBytes = reinterpret_cast<const std::uint8_t*>(resolverAddress);
		const auto* formArgBytes = reinterpret_cast<const std::uint8_t*>(formArgAddress);
		constexpr std::array<std::uint8_t, 14> expectedResolver{
			0x4C, 0x8B, 0x0D, 0x35, 0xC8, 0x05, 0x00,
			0x49, 0x81, 0xC1, 0x10, 0xEA, 0x30, 0x00
		};
		constexpr std::array<std::uint8_t, 3> expectedFormArg{ 0x45, 0x33, 0xC0 };  // xor r8d,r8d
		if (std::memcmp(resolverBytes, expectedResolver.data(), expectedResolver.size()) != 0 ||
			std::memcmp(formArgBytes, expectedFormArg.data(), expectedFormArg.size()) != 0) {
			spdlog::warn("Descriptions: PRKF PopulatePerkEntry description site verification failed; original code left untouched");
			return false;
		}

		std::array<std::uint8_t, 14> resolverBackup{};
		std::array<std::uint8_t, 3> formArgBackup{};
		std::memcpy(resolverBackup.data(), resolverBytes, resolverBackup.size());
		std::memcpy(formArgBackup.data(), formArgBytes, formArgBackup.size());

		std::array<std::uint8_t, 14> resolverPatch{};
		resolverPatch.fill(0x90);
		resolverPatch[0] = 0x49;
		resolverPatch[1] = 0xB9;  // mov r9, imm64
		const auto hookAddress = reinterpret_cast<std::uintptr_t>(&PRKFDescriptionHook);
		std::memcpy(resolverPatch.data() + 2, std::addressof(hookAddress), sizeof(hookAddress));
		constexpr std::array<std::uint8_t, 3> formArgPatch{ 0x4C, 0x8B, 0xC7 };  // mov r8,rdi

		if (!PCF::NativeHooks::WriteVerified(resolverAddress, resolverPatch)) {
			spdlog::warn("Descriptions: PRKF description target redirect failed; original code left untouched");
			return false;
		}
		if (!PCF::NativeHooks::WriteVerified(formArgAddress, formArgPatch)) {
			PCF::NativeHooks::WriteVerified(resolverAddress, resolverBackup);
			spdlog::warn("Descriptions: PRKF source-perk argument patch failed; description target redirect restored");
			return false;
		}

		g_prkfDescriptionBridgeInstalled = true;
		spdlog::info("Descriptions: PRKF LevelUpMenu description bridge active (direct source perk context)");
		return true;
	}

	// Updates Pip-Boy perk descriptions after the game fills its list.
	void PipboyHook(RE::PipboyPerksMenu* a_menu)
	{
		if (!g_pipboyUpdateOriginal) {
			return;
		}
		g_pipboyUpdateOriginal(a_menu);
		if (g_descriptionBehaviorActive) {
			UpdatePipboyDescriptions(a_menu);
		}
	}

	// Installs the TESDescription hook used for perk descriptions.
	bool InstallDescriptionHook()
	{
		if (g_descriptionHookAttempted) {
			return g_descriptionHookInstalled;
		}
		g_descriptionHookAttempted = true;
		try {
			const auto resolved = REL::IDDatabase::get().resolve(PCF::EngineIDs::TESDescriptionGetDescription);
			if (!resolved) {
				spdlog::error("Descriptions: TESDescription::GetDescription relocation unavailable");
				return false;
			}
			const auto source = REL::Module::get().base() + *resolved.rva;
			const auto prologueLength = PCF::NativeHooks::FindSafeOverwriteLength(source);
			if (prologueLength < PCF::NativeHooks::kRelativeJumpSize) {
				spdlog::error("Descriptions: TESDescription::GetDescription prologue is not safe to patch");
				return false;
			}

			auto& trampoline = F4SE::GetTrampoline();
			if (trampoline.empty()) {
				const auto* trampolineInterface = F4SE::GetTrampolineInterface();
				void* memory = trampolineInterface ? trampolineInterface->AllocateFromBranchPool(kTrampolineSize) : nullptr;
				if (!memory) {
					spdlog::error("Descriptions: trampoline allocation failed");
					return false;
				}
				trampoline.set_trampoline(memory, kTrampolineSize);
			}
			if (trampoline.free_size() < prologueLength + (PCF::NativeHooks::kAbsoluteJumpSize * 2)) {
				spdlog::error("Descriptions: trampoline does not have enough free space");
				return false;
			}

			std::vector<std::uint8_t> originalBytes(prologueLength);
			std::memcpy(originalBytes.data(), reinterpret_cast<const void*>(source), prologueLength);

			auto* original = static_cast<std::uint8_t*>(trampoline.allocate(prologueLength + PCF::NativeHooks::kAbsoluteJumpSize));
			std::memcpy(original, originalBytes.data(), prologueLength);
			PCF::NativeHooks::WriteAbsoluteJump(original + prologueLength, source + prologueLength);
			g_descriptionOriginal = reinterpret_cast<DescriptionFunction>(original);

			auto* relay = static_cast<std::uint8_t*>(trampoline.allocate(PCF::NativeHooks::kAbsoluteJumpSize));
			PCF::NativeHooks::WriteAbsoluteJump(relay, reinterpret_cast<std::uintptr_t>(&DescriptionHook));
			std::vector<std::uint8_t> patch;
			if (!PCF::NativeHooks::MakeRelativeJumpPatch(source, reinterpret_cast<std::uintptr_t>(relay), prologueLength, patch)) {
				g_descriptionOriginal = nullptr;
				spdlog::error("Descriptions: hook relay is outside the relative branch range");
				return false;
			}

			if (std::memcmp(reinterpret_cast<const void*>(source), originalBytes.data(), originalBytes.size()) != 0) {
				g_descriptionOriginal = nullptr;
				spdlog::error("Descriptions: TESDescription hook target changed before installation");
				return false;
			}
			if (!PCF::NativeHooks::WriteVerified(source, patch)) {
				const bool restored = PCF::NativeHooks::WriteVerified(source, originalBytes);
				if (restored) {
					g_descriptionOriginal = nullptr;
				}
				spdlog::error("Descriptions: TESDescription hook write verification failed{}",
					restored ? " and the original bytes were restored" : " and the original bytes could not be verified");
				return false;
			}
			g_descriptionHookInstalled = true;
			return true;
		} catch (...) {
			return false;
		}
	}

	// Restores the Pip-Boy callback without overwriting a hook installed after PCF.
	bool RestorePipboyHook()
	{
		if (!g_pipboyUpdateOriginal || !g_pipboyVtableAddress) {
			return true;
		}
		try {
			const auto original = reinterpret_cast<std::uintptr_t>(g_pipboyUpdateOriginal);
			const auto hook = reinterpret_cast<std::uintptr_t>(&PipboyHook);
			const auto* table = reinterpret_cast<const std::uintptr_t*>(g_pipboyVtableAddress);
			const auto current = table[kPipboyUpdateSlot];
			if (current == original) {
				g_pipboyHookInstalled = false;
				g_pipboyUpdateOriginal = nullptr;
				g_pipboyVtableAddress = 0;
				return true;
			}
			if (current != hook) {
				// Another hook may now chain through PCF. Keep our original callback alive and
				// leave the current slot untouched so an inactive PCF callback can still forward safely.
				return false;
			}

			REL::Relocation<std::uintptr_t> vtable{ g_pipboyVtableAddress };
			vtable.write_vfunc(kPipboyUpdateSlot, g_pipboyUpdateOriginal);
			if (table[kPipboyUpdateSlot] != original) {
				return false;
			}
			g_pipboyHookInstalled = false;
			g_pipboyUpdateOriginal = nullptr;
			g_pipboyVtableAddress = 0;
			return true;
		} catch (...) {
			return false;
		}
	}

	// Installs and verifies the Pip-Boy hook used to update its perk description list.
	bool InstallPipboyHook()
	{
		if (g_pipboyHookAttempted) {
			return g_pipboyHookInstalled;
		}
		g_pipboyHookAttempted = true;
		try {
			const auto updateLookup = REL::IDDatabase::get().resolve(PCF::EngineIDs::PipboyPerksMenuUpdateData);
			const auto vtableLookup = REL::IDDatabase::get().resolve(RE::VTABLE::PipboyPerksMenu[0]);
			if (!updateLookup || !vtableLookup) {
				spdlog::error("Descriptions: Pip-Boy update relocation or vtable unavailable");
				return false;
			}
			const auto base = REL::Module::get().base();
			const auto vtableAddress = base + *vtableLookup.rva;
			const auto expectedUpdate = base + *updateLookup.rva;
			const auto* table = reinterpret_cast<const std::uintptr_t*>(vtableAddress);
			if (table[kPipboyUpdateSlot] != expectedUpdate) {
				spdlog::error("Descriptions: Pip-Boy update vtable slot does not match the proven runtime contract");
				return false;
			}

			g_pipboyVtableAddress = vtableAddress;
			g_pipboyUpdateOriginal = reinterpret_cast<PipboyUpdateFunction>(expectedUpdate);
			REL::Relocation<std::uintptr_t> vtable{ vtableAddress };
			const auto previous = vtable.write_vfunc(kPipboyUpdateSlot, &PipboyHook);
			if (previous != expectedUpdate) {
				if (previous) {
					g_pipboyUpdateOriginal = reinterpret_cast<PipboyUpdateFunction>(previous);
				}
				const bool restored = RestorePipboyHook();
				spdlog::error("Descriptions: Pip-Boy vtable changed during installation{}",
					restored ? "; original callback restored" : "; rollback could not be verified");
				return false;
			}
			if (table[kPipboyUpdateSlot] != reinterpret_cast<std::uintptr_t>(&PipboyHook)) {
				const bool restored = RestorePipboyHook();
				spdlog::error("Descriptions: Pip-Boy vtable hook write verification failed{}",
					restored ? "; original callback restored" : "; rollback could not be verified");
				return false;
			}
			g_pipboyHookInstalled = true;
			return true;
		} catch (...) {
			const bool restored = RestorePipboyHook();
			if (!restored && g_pipboyUpdateOriginal) {
				spdlog::error("Descriptions: Pip-Boy installation failed and rollback could not be verified; preserved callback remains native-forwarding only");
			}
			return false;
		}
	}


}

namespace PCF::PerkDescriptions
{
	// Installs configured descriptions through the shared native path and adds Pip-Boy support only for perk rules.
	bool Install()
	{
		g_descriptionBehaviorActive = false;
		if (!PCF::RuleRegistry::HasDescriptionRules()) {
			return true;
		}
		const bool perkDescriptionsActive = PCF::RuleRegistry::HasPerkDescriptionRules();
		if (perkDescriptionsActive && !InstallPipboyHook()) {
			return false;
		}
		if (InstallDescriptionHook()) {
			g_descriptionBehaviorActive = true;
			InstallPRKFDescriptionBridge();
			return true;
		}
		if (perkDescriptionsActive && !RestorePipboyHook()) {
			spdlog::error("Description hooks could not be rolled back completely after installation failed; preserved callbacks remain native-forwarding only");
		}
		return false;
	}
}
