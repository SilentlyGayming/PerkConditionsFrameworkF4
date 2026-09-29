// Perk Conditions Framework
// SilentlyGayming
// TextManager.cpp

#include "PCH.h"
#include "TextManager.h"
#include "PerkConditions.h"
#include "PerkDescriptions.h"
#include "SourcePerkNames.h"
#include "EngineIDs.h"
#include "NativeHooks.h"
#include "RuleRegistry.h"
#include "TextRewriter.h"

#include <Zydis/Zydis.h>

#ifndef NOMINMAX
#	define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{
	struct TextDictionaryState
	{
		PCF::TextRewriter::Dictionary dictionary;
		std::unordered_map<RE::BGSPerk*, PCF::TextManager::RequirementPresentation> presentations;
		std::unordered_map<RE::BGSPerk*, PCF::TextManager::RequirementPresentation> originalPresentations;
		bool built{ false };
	};

	TextDictionaryState g_textDictionary;

	// Builds a readable form name for logging.
	std::string DescribeForm(const RE::TESForm* a_form)
	{
		if (!a_form) {
			return "<unknown>";
		}
		const auto formID = a_form->GetFormID();
		const auto* files = a_form->sourceFiles.array;
		const auto* file = files && !files->empty() ? (*files)[0] : nullptr;
		if (file) {
			const auto localID = file->IsLight() ? formID & 0x00000FFF : formID & 0x00FFFFFF;
			return fmt::format("{}|{:08X}", file->filename.data(), localID);
		}
		return fmt::format("{:08X}", formID);
	}

	// Gets a display name with a safe fallback for replacement text.
	std::string GetFormName(const RE::TESForm* a_form, std::string_view a_fallback)
	{
		if (a_form) {
			const auto fullName = RE::TESFullName::GetFormFullName(a_form);
			const char* name = fullName ? fullName->data() : nullptr;
			if (name && name[0]) {
				return std::string(name);
			}
			const char* editorID = a_form->GetFormEditorID();
			if (editorID && editorID[0]) {
				return editorID;
			}
		}
		return std::string(a_fallback);
	}

	// Gets the real player-visible TESFullName used for text replacement.
	std::string GetVisibleSourceName(const RE::TESForm* a_form)
	{
		if (!a_form) {
			return {};
		}
		const auto fullName = RE::TESFullName::GetFormFullName(a_form);
		const char* name = fullName ? fullName->data() : nullptr;
		return name && name[0] ? std::string(name) : std::string{};
	}

	// Compares visible names without caring about letter case.
	bool SameText(std::string_view a_left, std::string_view a_right)
	{
		return a_left.size() == a_right.size() && std::equal(a_left.begin(), a_left.end(), a_right.begin(),
			[](char a, char b) {
				return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
			});
	}

	// Finds the main linked-family perk for a perk.
	RE::BGSPerk* GetPerkFamily(RE::BGSPerk* a_perk)
	{
		if (!a_perk) {
			return nullptr;
		}
		const auto rank = PCF::RuleRegistry::GetRank(a_perk);
		return rank.base ? rank.base : a_perk;
	}

	struct SourceTextIdentity
	{
		RE::BGSPerk* family{ nullptr };
		std::uint32_t rank{ 0 };
	};

	// Finds the source perk family and rank using the registry.
	SourceTextIdentity GetSourceTextIdentity(RE::BGSPerk* a_source)
	{
		if (!a_source) {
			return {};
		}
		const auto linked = PCF::RuleRegistry::GetRank(a_source);
		if (linked.base && linked.rank) {
			const auto resolved = PCF::RuleRegistry::FindPerkRank(a_source, linked.rank);
			if (resolved.perk == a_source && resolved.kind == PCF::PerkRankType::kLinked) {
				return { linked.base, linked.rank };
			}
		}

		std::uint32_t fallbackRank = 0;
		for (std::uint32_t rank = 255; rank > 1; --rank) {
			const auto resolved = PCF::RuleRegistry::FindPerkRank(a_source, rank);
			if (resolved.perk == a_source && resolved.kind == PCF::PerkRankType::kFallback) {
				fallbackRank = rank;
				break;
			}
		}
		if (!fallbackRank) {
			return { a_source, linked.rank ? linked.rank : 1u };
		}

		auto* family = a_source;
		for (auto* candidate : PCF::RuleRegistry::Sources()) {
			if (!candidate || candidate == a_source) {
				continue;
			}
			const auto first = PCF::RuleRegistry::FindPerkRank(candidate, 1);
			const auto resolved = PCF::RuleRegistry::FindPerkRank(candidate, fallbackRank);
			if (first.perk == candidate && first.kind == PCF::PerkRankType::kInternal &&
				resolved.perk == a_source && resolved.kind == PCF::PerkRankType::kFallback) {
				family = candidate;
				break;
			}
		}
		return { family, fallbackRank };
	}

	// Gets the visible symbol for a configured number comparison.
	std::string_view GetComparisonSymbol(PCF::ComparisonOp a_comparison)
	{
		switch (a_comparison) {
		case PCF::ComparisonOp::kEqual:
			return "=";
		case PCF::ComparisonOp::kGreater:
			return ">";
		case PCF::ComparisonOp::kLess:
			return "<";
		case PCF::ComparisonOp::kGreaterEqual:
			return ">=";
		case PCF::ComparisonOp::kLessEqual:
			return "<=";
		}
		return {};
	}

	// Builds the visible text for one numeric requirement.
	std::string BuildNumberRequirement(std::string_view a_label, PCF::ComparisonOp a_comparison, float a_required)
	{
		switch (a_comparison) {
		case PCF::ComparisonOp::kEqual:
			return fmt::format("{} = {:g}", a_label, a_required);
		case PCF::ComparisonOp::kGreater:
			return fmt::format("{} > {:g}", a_label, a_required);
		case PCF::ComparisonOp::kLess:
			return fmt::format("{} < {:g}", a_label, a_required);
		case PCF::ComparisonOp::kGreaterEqual:
			return fmt::format("{} {:g}+", a_label, a_required);
		case PCF::ComparisonOp::kLessEqual:
			return fmt::format("{} <= {:g}", a_label, a_required);
		}
		return {};
	}

	// Builds the value text used by numeric menu fields.
	std::string BuildNumberText(PCF::ComparisonOp a_comparison, float a_required)
	{
		switch (a_comparison) {
		case PCF::ComparisonOp::kEqual:
			return fmt::format("= {:g}", a_required);
		case PCF::ComparisonOp::kGreater:
			return fmt::format("> {:g}", a_required);
		case PCF::ComparisonOp::kLess:
			return fmt::format("< {:g}", a_required);
		case PCF::ComparisonOp::kGreaterEqual:
			return fmt::format("{:g}", a_required);
		case PCF::ComparisonOp::kLessEqual:
			return fmt::format("<= {:g}", a_required);
		}
		return {};
	}

	// Gets the replacement form used for player-visible display data.
	RE::TESForm* GetDisplayTarget(const PCF::PerkAlternative& a_display)
	{
		switch (a_display.type) {
		case PCF::AlternativeType::kPerk:
			return a_display.perk;
		case PCF::AlternativeType::kActorValue:
			return a_display.actorValue;
		case PCF::AlternativeType::kGlobalValue:
			return a_display.globalValue;
		}
		return nullptr;
	}

	// Gets the CUSTOM display name, giving [Names] entries priority.
	std::string GetCustomDisplayName(const PCF::PerkAlternative& a_display, bool a_familyFallback)
	{
		if (const auto* configured = PCF::RuleRegistry::GetDisplayName(GetDisplayTarget(a_display)); configured && !configured->empty()) {
			return *configured;
		}
		switch (a_display.type) {
		case PCF::AlternativeType::kPerk:
			if (!a_familyFallback && !a_display.name.empty()) {
				return a_display.name;
			}
			if (auto* family = GetPerkFamily(a_display.perk)) {
				return GetFormName(family, "Perk");
			}
			return a_display.perk ? GetFormName(a_display.perk, "Perk") : std::string{};
		case PCF::AlternativeType::kActorValue:
			return !a_display.name.empty() ? a_display.name :
				a_display.actorValue ? GetFormName(a_display.actorValue, "Skill") : std::string{};
		case PCF::AlternativeType::kGlobalValue:
			return !a_display.name.empty() ? a_display.name :
				a_display.globalValue ? std::string("GlobalValue") : std::string{};
		}
		return {};
	}

	// Builds the full visible requirement text for a replacement.
	std::string BuildDisplayLabel(const PCF::PerkAlternative& a_display)
	{
		const auto label = GetCustomDisplayName(a_display, false);
		if (label.empty()) {
			return {};
		}
		return a_display.type == PCF::AlternativeType::kPerk ? label :
			BuildNumberRequirement(label, a_display.comparison, a_display.requiredValue);
	}

	// Builds the shorter family name used in MESSAGE text.
	std::string BuildGenericLabel(const PCF::PerkAlternative& a_display)
	{
		return GetCustomDisplayName(a_display, true);
	}

	// Builds the identity used to compare one custom requirement.
	PCF::TextManager::RequirementIdentity BuildRequirementIdentity(const PCF::PerkAlternative& a_display)
	{
		PCF::TextManager::RequirementIdentity identity;
		identity.type = a_display.type;
		identity.comparison = a_display.comparison;
		switch (a_display.type) {
		case PCF::AlternativeType::kPerk:
			identity.target = GetPerkFamily(a_display.perk);
			break;
		case PCF::AlternativeType::kActorValue:
			identity.target = a_display.actorValue;
			break;
		case PCF::AlternativeType::kGlobalValue:
			identity.target = a_display.globalValue;
			break;
		}
		identity.equalityValue = a_display.requiredValue;
		return identity;
	}

	// Builds the fixed CUSTOM display data for one replacement.
	PCF::TextManager::RequirementPresentation BuildRequirement(const PCF::PerkAlternative& a_display)
	{
		PCF::TextManager::RequirementPresentation result;
		result.identity = BuildRequirementIdentity(a_display);
		result.label = GetCustomDisplayName(a_display, false);
		result.fullLabel = BuildDisplayLabel(a_display);
		result.rowLabel = result.label;
		if (a_display.type != PCF::AlternativeType::kPerk && a_display.comparison != PCF::ComparisonOp::kGreaterEqual && !result.rowLabel.empty()) {
			result.rowLabel = fmt::format("{} {}", result.rowLabel, GetComparisonSymbol(a_display.comparison));
		}
		switch (a_display.type) {
		case PCF::AlternativeType::kPerk:
			result.value = static_cast<double>(a_display.rank);
			result.strength = static_cast<double>(a_display.foundRank ? a_display.foundRank.rank : a_display.rank);
			result.valueText = fmt::format("{:g}", result.value);
			break;
		case PCF::AlternativeType::kActorValue:
		case PCF::AlternativeType::kGlobalValue:
			result.value = static_cast<double>(a_display.requiredValue);
			result.strength = a_display.comparison == PCF::ComparisonOp::kLess || a_display.comparison == PCF::ComparisonOp::kLessEqual ? -result.value : result.value;
			result.valueText = BuildNumberText(a_display.comparison, a_display.requiredValue);
			break;
		}
		return result;
	}


	// Builds ORIGINAL display data without changing the game text.
	PCF::TextManager::RequirementPresentation BuildOriginalRequirement(RE::BGSPerk* a_source)
	{
		PCF::TextManager::RequirementPresentation result;
		result.overrideNativeText = false;
		const auto sourceIdentity = GetSourceTextIdentity(a_source);
		auto* family = sourceIdentity.family ? sourceIdentity.family : a_source;
		result.identity.type = PCF::AlternativeType::kPerk;
		result.identity.target = family;
		result.label = GetVisibleSourceName(family);
		if (result.label.empty()) {
			result.label = GetVisibleSourceName(a_source);
		}
		result.fullLabel = result.label;
		result.rowLabel = result.label;
		result.value = static_cast<double>(sourceIdentity.rank);
		result.strength = result.value;
		if (sourceIdentity.rank) {
			result.valueText = fmt::format("{}", sourceIdentity.rank);
		}
		return result;
	}

	// Converts a registry replacement into the identity used to merge matching requirements.
	PCF::TextRewriter::MatchKey BuildMatchIdentity(const PCF::PerkAlternative& a_display)
	{
		PCF::TextRewriter::MatchKey identity;
		identity.comparison = static_cast<std::uint8_t>(a_display.comparison);
		switch (a_display.type) {
		case PCF::AlternativeType::kPerk:
			identity.type = PCF::TextRewriter::MatchType::kPerk;
			identity.target = reinterpret_cast<std::uintptr_t>(GetPerkFamily(a_display.perk));
			identity.rank = a_display.rank;
			break;
		case PCF::AlternativeType::kActorValue:
			identity.type = PCF::TextRewriter::MatchType::kActorValue;
			identity.target = reinterpret_cast<std::uintptr_t>(a_display.actorValue);
			identity.threshold = a_display.requiredValue;
			break;
		case PCF::AlternativeType::kGlobalValue:
			identity.type = PCF::TextRewriter::MatchType::kGlobalValue;
			identity.target = reinterpret_cast<std::uintptr_t>(a_display.globalValue);
			identity.threshold = a_display.requiredValue;
			break;
		}
		return identity;
	}

	// Adds one unique visible name to the replacement list.
	bool AddAlias(std::vector<PCF::TextRewriter::AliasEntry>& a_entries, RE::BGSPerk* a_source,
		std::uint32_t a_rank, std::string a_phrase, const std::string& a_replacement,
		const std::string& a_genericReplacement, const PCF::TextRewriter::MatchKey& a_semantic)
	{
		if (!a_source || a_phrase.empty() || a_replacement.empty()) {
			return false;
		}
		const auto duplicate = std::find_if(a_entries.begin(), a_entries.end(), [&](const PCF::TextRewriter::AliasEntry& a_existing) {
			return a_existing.source == reinterpret_cast<std::uintptr_t>(a_source) && SameText(a_existing.phrase, a_phrase);
		});
		if (duplicate != a_entries.end()) {
			return false;
		}
		a_entries.push_back({ reinterpret_cast<std::uintptr_t>(a_source), a_rank,
			std::move(a_phrase), a_replacement, a_genericReplacement, a_semantic });
		return true;
	}

	// Clears a prematurely built alias dictionary so startup can retry after localized data is ready.
	void ResetTextDictionary()
	{
		g_textDictionary.dictionary.Build({});
		g_textDictionary.presentations.clear();
		g_textDictionary.originalPresentations.clear();
		g_textDictionary.built = false;
	}

	// Builds the fixed text replacement list once after the registry loads.
	void BuildTextDictionary()
	{
		if (g_textDictionary.built) {
			return;
		}
		std::vector<PCF::TextRewriter::AliasEntry> entries;
		const auto& sources = PCF::RuleRegistry::Sources();
		entries.reserve(sources.size());
		g_textDictionary.presentations.clear();
		g_textDictionary.presentations.reserve(sources.size());
		g_textDictionary.originalPresentations.clear();
		g_textDictionary.originalPresentations.reserve(sources.size());
		const auto sourceNames = PCF::SourcePerkNames::LoadNames(sources);
		std::uint32_t runtimeNameFallbacks = 0;
		for (auto* source : sources) {
			const auto* rule = PCF::RuleRegistry::Find(source);
			if (!source || !rule) {
				continue;
			}
			const auto* display = PCF::RuleRegistry::GetDisplay(*rule);
			const auto original = g_textDictionary.originalPresentations.try_emplace(source, BuildOriginalRequirement(source)).first;
			if (rule->display == PCF::DisplayMode::kOriginal) {
				g_textDictionary.presentations.try_emplace(source, original->second);
				continue;
			}
			if (!display) {
				continue;
			}
			g_textDictionary.presentations.try_emplace(source, BuildRequirement(*display));
			const auto replacement = BuildDisplayLabel(*display);
			const auto genericReplacement = BuildGenericLabel(*display);
			if (replacement.empty()) {
				continue;
			}
			std::string sourceName;
			const auto name = sourceNames.names.find(source);
			if (name != sourceNames.names.end() && !name->second.empty()) {
				sourceName = name->second;
			} else {
				// The original-plugin scanner is preferred because it survives winning-override
				// renames. If resource/localization data is not ready yet, retain text swapping by
				// falling back to the already-loaded runtime FULL name for this source only.
				sourceName = GetVisibleSourceName(source);
				if (!sourceName.empty()) {
					++runtimeNameFallbacks;
				}
			}
			if (sourceName.empty()) {
				continue;
			}
			const auto sourceIdentity = GetSourceTextIdentity(source);
			const auto matchKey = BuildMatchIdentity(*display);
			AddAlias(entries, source, sourceIdentity.rank, std::move(sourceName), replacement, genericReplacement, matchKey);
		}
		if (runtimeNameFallbacks) {
			spdlog::warn("Text aliases: original source-name lookup missed {} perk(s); runtime FULL-name fallback used",
				runtimeNameFallbacks);
		}
		g_textDictionary.dictionary.Build(std::move(entries));
		g_textDictionary.built = true;
	}

	// Checks whether a condition is a positive HasPerk check used by MESSAGE text.
	bool IsMessagePerkCheck(RE::TESConditionItem* a_item)
	{
		if (!a_item || !PCF::PerkConditions::IsPositivePerkCheck(a_item)) {
			return false;
		}
		const auto function = static_cast<std::uint32_t>(a_item->data.functionData.function.get());
		const auto hasPerk = std::uint32_t{ 448 };
		return function == hasPerk || function == hasPerk - 0x1000;
	}

	// Adds unique source perks while keeping MESSAGE-body HasPerk rules when requested.
	void AddConditionPerks(RE::TESConditionItem* a_head, std::vector<std::uintptr_t>& a_sources, bool a_hasPerkOnly)
	{
		for (auto* item = a_head; item; item = item->next) {
			if (a_hasPerkOnly ? !IsMessagePerkCheck(item) : !PCF::PerkConditions::IsPositivePerkCheck(item)) {
				continue;
			}
			auto* source = PCF::PerkConditions::GetConditionPerk(item);
			const auto identity = reinterpret_cast<std::uintptr_t>(source);
			if (source && std::find(a_sources.begin(), a_sources.end(), identity) == a_sources.end()) {
				a_sources.push_back(identity);
			}
		}
	}
// Removes the string ID prefix from visible text.
	std::string_view GetVisibleText(std::string_view a_text)
	{
		if (!a_text.starts_with("<ID=")) {
			return a_text;
		}
		const auto end = a_text.find('>');
		return end == std::string_view::npos ? a_text : a_text.substr(end + 1);
	}

	// Updates MESSAGE text after the game finishes building it.
	bool UpdateMessageBody(RE::BGSMessage* a_message, RE::BSFixedString& a_result)
	{
		if (!a_message || !g_textDictionary.built || g_textDictionary.dictionary.Empty()) {
			return false;
		}
		const char* raw = a_result.c_str();
		if (!raw || !raw[0]) {
			return false;
		}
		bool hasButton = false;
		std::vector<std::uintptr_t> sources;
		for (auto* button : a_message->buttonList) {
			if (!button) {
				continue;
			}
			hasButton = true;
			AddConditionPerks(button->conditions.head, sources, true);
		}
		if (!hasButton || sources.empty()) {
			return false;
		}
		const std::string_view original(raw);
		const auto rewritten = PCF::TextRewriter::RewriteVisibleText(original, g_textDictionary.dictionary,
			{ sources, true });
		if (!rewritten) {
			return false;
		}
		// Keeps the original text alive until logging is finished.
		const RE::BSFixedString originalOwner(a_result);
		a_result = rewritten->c_str();
		try {
			const auto line = fmt::format("{} [MESSAGE]: \"{}\" -> \"{}\"", DescribeForm(a_message), original, *rewritten);
			static std::mutex logMutex;
			static std::unordered_set<std::string> loggedRewrites;
			bool firstRewrite = false;
			{
				std::scoped_lock lock(logMutex);
				firstRewrite = loggedRewrites.insert(line).second;
			}
			if (firstRewrite) {
				spdlog::info("{}", line);
			}
		} catch (...) {
		}
		return true;
	}

	using MessageBodyFunction = std::uint32_t (*)(RE::BGSMessage*, RE::BSFixedString&);
	MessageBodyFunction g_messageBodyOriginal{ nullptr };
	bool g_messageBodyHookAttempted{ false };
	bool g_messageBodyBehaviorActive{ false };
	constexpr std::size_t kMessageBodyPatchSize = 5;
	constexpr std::size_t kMessageBodyDecodeLimit = 32;
	constexpr std::size_t kAbsoluteJumpSize = 14;
	constexpr std::size_t kMessageBodyTrampolineSize = 128;

	// Marks MESSAGE text replacement unavailable when its hook cannot be installed.
	bool DisableMessageTextHook(bool a_preserveOriginal = false)
	{
		g_messageBodyBehaviorActive = false;
		if (!a_preserveOriginal) {
			g_messageBodyOriginal = nullptr;
		}
		spdlog::warn("MESSAGE body text hook unavailable; body rewriting disabled");
		return false;
	}

	// Checks for instructions that cannot be copied safely into hook code.
	bool HasUnsafeOperand(const ZydisDecodedInstruction& a_instruction, const ZydisDecodedOperand* a_operands)
	{
		for (std::uint8_t i = 0; i < a_instruction.operand_count; ++i) {
			const auto& operand = a_operands[i];
			if (operand.type == ZYDIS_OPERAND_TYPE_MEMORY &&
				(operand.mem.base == ZYDIS_REGISTER_RIP || operand.mem.base == ZYDIS_REGISTER_EIP)) {
				return true;
			}
			if (operand.type == ZYDIS_OPERAND_TYPE_IMMEDIATE && operand.imm.is_relative) {
				return true;
			}
		}
		return false;
	}

	// Finds how many bytes can be safely replaced by the MESSAGE hook.
	std::size_t GetMessageHookSize(std::uintptr_t a_address)
	{
		ZydisDecoder decoder;
		if (!ZYAN_SUCCESS(ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64))) {
			return 0;
		}
		const auto* code = reinterpret_cast<const ZyanU8*>(a_address);
		std::size_t length = 0;
		while (length < kMessageBodyPatchSize) {
			if (length >= kMessageBodyDecodeLimit) {
				return 0;
			}
			ZydisDecodedInstruction instruction;
			ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
			if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, code + length, kMessageBodyDecodeLimit - length,
				std::addressof(instruction), operands)) || !instruction.length || HasUnsafeOperand(instruction, operands)) {
				return 0;
			}
			length += instruction.length;
		}
		return length <= kMessageBodyDecodeLimit ? length : 0;
	}

	// Writes an x64 jump for the hook.
	void WriteJump(void* a_destination, std::uintptr_t a_target)
	{
		std::array<std::uint8_t, kAbsoluteJumpSize> jump{};
		jump[0] = 0xFF;
		jump[1] = 0x25;
		std::memcpy(jump.data() + 6, std::addressof(a_target), sizeof(a_target));
		std::memcpy(a_destination, jump.data(), jump.size());
	}

	// Updates MESSAGE text after the game formatter returns.
	std::uint32_t MessageTextHook(RE::BGSMessage* a_message, RE::BSFixedString& a_result)
	{
		if (!g_messageBodyOriginal) {
			return 0;
		}
		const auto nativeResult = g_messageBodyOriginal(a_message, a_result);
		if (g_messageBodyBehaviorActive) {
			UpdateMessageBody(a_message, a_result);
		}
		return nativeResult;
	}

	// Installs the MESSAGE text hook.
	bool InstallMessageTextHook()
	{
		if (g_messageBodyHookAttempted) {
			return g_messageBodyBehaviorActive;
		}
		g_messageBodyHookAttempted = true;
		try {
			const auto source = REL::Relocation<std::uintptr_t>{
				PCF::EngineIDs::BGSMessageGetConvertedDescription
			}.GetAddress();
			const auto prologueLength = GetMessageHookSize(source);
			if (prologueLength < kMessageBodyPatchSize) {
				return DisableMessageTextHook();
			}

			auto& trampoline = **REL::GetTrampoline();
			if (trampoline.IsEmpty()) {
				const auto trampolineInterface = F4SE::GetTrampolineInterface();
				auto* memory = static_cast<std::byte*>(trampolineInterface->AllocateFromBranchPool(kMessageBodyTrampolineSize));
				if (!memory) {
					return DisableMessageTextHook();
				}
				trampoline.Init(memory, kMessageBodyTrampolineSize);
			}
			if (trampoline.GetFreeSize() < prologueLength + (kAbsoluteJumpSize * 2)) {
				return DisableMessageTextHook();
			}

			auto* original = reinterpret_cast<std::uint8_t*>(trampoline.Allocate(prologueLength + kAbsoluteJumpSize));
			std::memcpy(original, reinterpret_cast<const void*>(source), prologueLength);
			WriteJump(original + prologueLength, source + prologueLength);
			g_messageBodyOriginal = reinterpret_cast<MessageBodyFunction>(original);

			auto* relay = reinterpret_cast<std::uint8_t*>(trampoline.Allocate(kAbsoluteJumpSize));
			WriteJump(relay, reinterpret_cast<std::uintptr_t>(&MessageTextHook));
			const auto nextInstruction = source + kMessageBodyPatchSize;
			const auto displacement = reinterpret_cast<std::intptr_t>(relay) - static_cast<std::intptr_t>(nextInstruction);
			if (displacement < std::numeric_limits<std::int32_t>::min() || displacement > std::numeric_limits<std::int32_t>::max()) {
				return DisableMessageTextHook();
			}

			std::array<std::uint8_t, kMessageBodyDecodeLimit> patch{};
			patch.fill(REL::NOP);
			patch[0] = 0xE9;
			const auto relative = static_cast<std::int32_t>(displacement);
			std::memcpy(patch.data() + 1, std::addressof(relative), sizeof(relative));
			if (!PCF::NativeHooks::WriteVerified(source,
				std::span<const std::uint8_t>{ patch.data(), prologueLength })) {
				return DisableMessageTextHook(true);
			}
			g_messageBodyBehaviorActive = true;
			return true;
		} catch (...) {
			return DisableMessageTextHook();
		}
	}


	inline constexpr std::uintptr_t kPRKFGetRequirementsRVA = 0x13DF0;
	inline constexpr std::uintptr_t kPRKFStringAssignRVA = 0x79A0;
	inline constexpr std::uint32_t kPRKFImageTimestamp = 0x6A9D7778;
	inline constexpr std::uint32_t kPRKFImageSize = 0x0007C000;
	inline constexpr std::string_view kPRKFFailedOpen = "<font color='#fa8e47'>";
	inline constexpr std::string_view kPRKFFailedClose = "</font>";
	inline constexpr std::string_view kPRKFAnd = ", ";
	inline constexpr std::string_view kPRKFOr = " $PRKF_or ";
	inline constexpr std::string_view kPRKFOrOutput = "$PRKF_or ";

	struct PRKFString
	{
		union { char local[16]; char* heap; };
		std::size_t size;
		std::size_t capacity;
		[[nodiscard]] const char* data() const noexcept { return capacity > 15 ? heap : local; }
		[[nodiscard]] std::string_view view() const noexcept { return data() ? std::string_view(data(), size) : std::string_view{}; }
	};
	static_assert(sizeof(PRKFString) == 0x20);

	struct PRKFSegment
	{
		std::string_view text;
		std::string_view separator;
		const RE::TESForm* form{ nullptr };
		const std::string* label{ nullptr };
		bool failed{ false };
	};

	using PRKFGetRequirements = void* (*)(void*, RE::BGSPerk*);
	using PRKFStringAssign = PRKFString* (*)(PRKFString*, const char*, std::size_t);
	PRKFGetRequirements g_prkfGetRequirements{ nullptr };
	PRKFStringAssign g_prkfStringAssign{ nullptr };
	bool g_prkfBridgeAttempted{ false };
	bool g_prkfBridgeActive{ false };
	std::mutex g_prkfLogLock;
	std::unordered_set<std::uint64_t> g_prkfLoggedMatches;
	std::unordered_set<std::uint32_t> g_prkfLoggedFailures;

	// Gets the ActorValue form from one PRKF-rendered numeric condition.
	RE::TESForm* GetPRKFConditionForm(RE::TESConditionItem* a_item)
	{
		if (!a_item || a_item->data.aliasParams || a_item->data.packDataParams) {
			return nullptr;
		}
		const auto function = static_cast<std::uint32_t>(a_item->data.functionData.function.get());
		const auto matches = [function](RE::SCRIPT_OUTPUT a_expected) {
			const auto value = static_cast<std::uint32_t>(a_expected);
			return function == value || (value >= 0x1000 && function == value - 0x1000);
		};
		if (!matches(static_cast<RE::SCRIPT_OUTPUT>(14)) &&
			!matches(static_cast<RE::SCRIPT_OUTPUT>(277)) &&
			!matches(static_cast<RE::SCRIPT_OUTPUT>(494))) {
			return nullptr;
		}
		auto* form = static_cast<RE::TESForm*>(a_item->data.functionData.param[0]);
		return form && form->As<RE::ActorValueInfo>() ? form : nullptr;
	}

	// Rewrites configured segments from PRKF's already-rendered requirement string.
	std::optional<std::string> RewritePRKFRequirements(RE::BGSPerk* a_source, std::string_view a_original)
	{
		if (!a_source || !PCF::RuleRegistry::HasRequirementLabels(a_source) || !a_original.starts_with(kPRKFAnd)) {
			return std::nullopt;
		}

		std::vector<PRKFSegment> segments;
		std::size_t cursor = kPRKFAnd.size();
		for (auto* item = a_source->perkConditions.head; item; item = item->next) {
			const auto separator = item->next ? (item->data.compareOr ? kPRKFOr : kPRKFAnd) : std::string_view{};
			const auto end = separator.empty() ? a_original.size() : a_original.find(separator, cursor);
			if (end == std::string_view::npos || end < cursor) {
				return std::nullopt;
			}
			const auto text = a_original.substr(cursor, end - cursor);
			auto* form = GetPRKFConditionForm(item);
			const auto* label = form ? PCF::RuleRegistry::FindRequirementLabel(a_source, form) : nullptr;
			const bool failed = text.starts_with(kPRKFFailedOpen) && text.ends_with(kPRKFFailedClose);
			segments.push_back({ text, separator, form, label, failed });
			cursor = separator.empty() ? end : end + separator.size();
		}
		if (cursor != a_original.size()) {
			return std::nullopt;
		}

		bool matched = false;
		std::string output(kPRKFAnd);
		for (std::size_t i = 0; i < segments.size();) {
			const auto& first = segments[i];
			if (!first.form || !first.label) {
				output.append(first.text);
				if (first.separator == kPRKFOr) {
					output.append(kPRKFOrOutput);
				} else {
					output.append(first.separator);
				}
				++i;
				continue;
			}

			matched = true;
			bool failed = first.failed;
			std::size_t last = i;
			while (last + 1 < segments.size() && segments[last + 1].form == first.form &&
				segments[last + 1].label && *segments[last + 1].label == *first.label) {
				failed = segments[last].separator == kPRKFOr ? failed && segments[last + 1].failed : failed || segments[last + 1].failed;
				++last;
			}
			if (failed) {
				output.append(kPRKFFailedOpen);
			}
			output.append(*first.label);
			if (failed) {
				output.append(kPRKFFailedClose);
			}
			if (segments[last].separator == kPRKFOr) {
				output.append(kPRKFOrOutput);
			} else {
				output.append(segments[last].separator);
			}

			const auto key = (static_cast<std::uint64_t>(a_source->GetFormID()) << 32) | first.form->GetFormID();
			std::scoped_lock lock(g_prkfLogLock);
			if (g_prkfLoggedMatches.insert(key).second) {
				spdlog::info("RequirementLabels: source={} condition={} \"{}\" -> \"{}\"",
					DescribeForm(a_source), DescribeForm(first.form), first.text, *first.label);
			}
			i = last + 1;
		}
		return matched ? std::optional<std::string>(std::move(output)) : std::nullopt;
	}

	// Calls PRKF's formatter first and substitutes only its returned reqs string.
	void* PRKFRequirementHook(void* a_result, RE::BGSPerk* a_source)
	{
		auto* returned = g_prkfGetRequirements ? g_prkfGetRequirements(a_result, a_source) : a_result;
		if (!a_result || !a_source || !PCF::RuleRegistry::HasRequirementLabels(a_source)) {
			return returned;
		}
		auto* reqs = reinterpret_cast<PRKFString*>(reinterpret_cast<std::byte*>(a_result) + 0x10);
		if (const auto rewritten = RewritePRKFRequirements(a_source, reqs->view()); rewritten && g_prkfStringAssign) {
			g_prkfStringAssign(reqs, rewritten->data(), rewritten->size());
			return returned;
		}

		std::scoped_lock lock(g_prkfLogLock);
		if (g_prkfLoggedFailures.insert(a_source->GetFormID()).second) {
			spdlog::warn("RequirementLabels: source={} reached PRKF but no configured condition matched; original text retained",
				DescribeForm(a_source));
		}
		return returned;
	}

	// Installs one version-verified detour on PRKF's getperkreqs_internal formatter.
	bool InstallPRKFRequirementBridge()
	{
		if (g_prkfBridgeActive) {
			return true;
		}

		// PRKF can become available after PCF receives its first game-data notification.
		// Do not permanently consume the install attempt until the module actually exists.
		auto* module = ::GetModuleHandleW(L"PRKF.dll");
		if (!module) {
			spdlog::debug("RequirementLabels: PRKF.dll is not loaded yet; bridge installation deferred");
			return false;
		}
		if (g_prkfBridgeAttempted) {
			return false;
		}
		g_prkfBridgeAttempted = true;
		const auto base = reinterpret_cast<std::uintptr_t>(module);
		const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
		const auto* nt = dos->e_magic == IMAGE_DOS_SIGNATURE ?
			reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew) : nullptr;
		if (!nt || nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.TimeDateStamp != kPRKFImageTimestamp ||
			nt->OptionalHeader.SizeOfImage != kPRKFImageSize) {
			spdlog::warn("RequirementLabels: unsupported PRKF.dll build; original code left untouched");
			return false;
		}

		const auto source = base + kPRKFGetRequirementsRVA;
		const auto stringAssign = base + kPRKFStringAssignRVA;
		constexpr std::array<std::uint8_t, PCF::NativeHooks::kAbsoluteJumpSize> prologue{
			0x48, 0x89, 0x5C, 0x24, 0x18,
			0x55, 0x56, 0x57,
			0x41, 0x54, 0x41, 0x55, 0x41, 0x56
		};
		constexpr std::array<std::uint8_t, 4> reqsField{ 0x4C, 0x8D, 0x71, 0x10 };
		constexpr std::array<std::uint8_t, 5> assignPrologue{ 0x48, 0x89, 0x5C, 0x24, 0x10 };
		if (std::memcmp(reinterpret_cast<const void*>(source), prologue.data(), prologue.size()) != 0 ||
			std::memcmp(reinterpret_cast<const void*>(source + 0x4E), reqsField.data(), reqsField.size()) != 0 ||
			std::memcmp(reinterpret_cast<const void*>(stringAssign), assignPrologue.data(), assignPrologue.size()) != 0 ||
			PCF::NativeHooks::FindSafeOverwriteLength(source, prologue.size()) != prologue.size()) {
			spdlog::warn("RequirementLabels: PRKF formatter verification failed; original code left untouched");
			return false;
		}

		constexpr std::size_t required = prologue.size() + PCF::NativeHooks::kAbsoluteJumpSize;
		auto* original = static_cast<std::uint8_t*>(::VirtualAlloc(
			nullptr, required, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
		if (!original) {
			spdlog::warn("RequirementLabels: dedicated PRKF stub allocation failed; original code left untouched");
			return false;
		}
		std::memcpy(original, reinterpret_cast<const void*>(source), prologue.size());
		PCF::NativeHooks::WriteAbsoluteJump(original + prologue.size(), source + prologue.size());
		::FlushInstructionCache(::GetCurrentProcess(), original, required);

		std::array<std::uint8_t, PCF::NativeHooks::kAbsoluteJumpSize> patch{};
		patch[0] = 0xFF;
		patch[1] = 0x25;
		const auto hookAddress = reinterpret_cast<std::uintptr_t>(&PRKFRequirementHook);
		std::memcpy(patch.data() + 6, std::addressof(hookAddress), sizeof(hookAddress));

		g_prkfGetRequirements = reinterpret_cast<PRKFGetRequirements>(original);
		g_prkfStringAssign = reinterpret_cast<PRKFStringAssign>(stringAssign);
		if (!PCF::NativeHooks::WriteVerified(source, patch)) {
			g_prkfGetRequirements = nullptr;
			g_prkfStringAssign = nullptr;
			PCF::NativeHooks::WriteVerified(source, prologue);
			::VirtualFree(original, 0, MEM_RELEASE);
			spdlog::warn("RequirementLabels: PRKF absolute detour write failed; original code restored");
			return false;
		}
		g_prkfBridgeActive = true;
		spdlog::info("RequirementLabels: PRKF requirement-text bridge active");
		return true;
	}



}

namespace PCF::TextManager
{
	// Builds player-facing display data for a pre-resolved requirement.
	RequirementPresentation BuildRequirementPresentation(const PerkAlternative& a_display)
	{
		return BuildRequirement(a_display);
	}

	// Replaces one already-structured display node with a target-specific label.
	bool ApplyRequirementLabel(const RE::TESForm* a_target, const RE::TESForm* a_conditionForm, RequirementPresentation& a_presentation)
	{
		const auto* label = PCF::RuleRegistry::FindRequirementLabel(a_target, a_conditionForm);
		if (!label) {
			return false;
		}
		a_presentation.label = *label;
		a_presentation.fullLabel = *label;
		a_presentation.rowLabel = *label;
		a_presentation.valueText.clear();
		return true;
	}

	// Gets configured display data without depending on whether the player currently passes it.
	const RequirementPresentation* GetConfiguredRequirement(RE::BGSPerk* a_source)
	{
		if (!a_source) {
			return nullptr;
		}
		BuildTextDictionary();
		const auto presentation = g_textDictionary.presentations.find(a_source);
		return presentation != g_textDictionary.presentations.end() ? std::addressof(presentation->second) : nullptr;
	}


	// Rewrites one visible string using the shared PCF text replacement list.
	std::optional<std::string> RewriteText(std::string_view a_original, std::span<const std::uintptr_t> a_preferredSources,
		bool a_genericBareReplacement, bool a_restrictToPreferred)
	{
		BuildTextDictionary();
		return TextRewriter::RewriteVisibleText(a_original, g_textDictionary.dictionary,
			{ a_preferredSources, a_genericBareReplacement, a_restrictToPreferred });
	}

	// Updates MESSAGE buttons when game data is ready.
	Result UpdateMessages(std::vector<std::string>& a_messageLogLines)
	{
		Result result;
		auto* data = RE::TESDataHandler::GetSingleton();
		if (!data) {
			return result;
		}
		// A menu/UI consumer can ask for presentation data during GameDataReady. If that
		// happened before localized source names were readable, retry the alias build now
		// that the new/load-game state is established instead of preserving an empty cache.
		if (g_textDictionary.built && g_textDictionary.dictionary.Empty() && !PCF::RuleRegistry::Sources().empty()) {
			ResetTextDictionary();
		}
		BuildTextDictionary();
		result.aliases = static_cast<std::uint32_t>(g_textDictionary.dictionary.Entries().size());

		for (auto* message : data->GetFormArray<RE::BGSMessage>()) {
			if (!message) {
				continue;
			}
			std::uint32_t buttonIndex = 0;
			for (auto* button : message->buttonList) {
				const auto currentButton = buttonIndex++;
				if (!button) {
					continue;
				}
				const auto original = GetVisibleText(static_cast<std::string_view>(button->text));
				std::vector<std::uintptr_t> preferredSources;
				AddConditionPerks(button->conditions.head, preferredSources, false);
				const auto rewritten = TextRewriter::RewriteVisibleText(original, g_textDictionary.dictionary, { preferredSources, false });
				if (!rewritten) {
					continue;
				}
				// The log view still refers to this localized string.
				const RE::BGSLocalizedString originalOwner(button->text);
				button->text = std::string_view(*rewritten);
				++result.messageButtonsChanged;
				try {
					a_messageLogLines.push_back(fmt::format("{} [MESSAGE button {}]: \"{}\" -> \"{}\"",
						DescribeForm(message), currentButton, original, *rewritten));
				} catch (...) {
				}
			}
		}

		return result;
	}

	// Installs the MESSAGE text hook; INFO text is handled separately through game-owned data.
	bool Install()
	{
		return InstallMessageTextHook();
	}

	// Installs the single verified PRKF bridge used by target-specific RequirementLabels.
	bool InstallRequirementLabels()
	{
		return !PCF::RuleRegistry::HasRequirementLabels() || InstallPRKFRequirementBridge();
	}
}
