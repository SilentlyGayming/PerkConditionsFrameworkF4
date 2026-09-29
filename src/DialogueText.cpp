// Perk Conditions Framework
// SilentlyGayming
// DialogueText.cpp

#include "PCH.h"
#include "DialogueText.h"
#include "EngineIDs.h"
#include "NativeHooks.h"
#include "PerkConditions.h"
#include "RuleRegistry.h"
#include "TextManager.h"
#include "CustomConditions.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{
	using DialoguePromptGetter = const char* (*)(RE::TESTopicInfo*);
	using DialoguePromptSetter = void (*)(RE::TESTopicInfo*, const char*);
	using DialoguePromptFallback = RE::TESTopicInfo* (*)(RE::TESTopicInfo*);
	using LocalizedSubrecordLoadFunction = void (*)(RE::BGSLocalizedString*, RE::TESFile*);
	using ResponseTextLoadFunction = void (*)(RE::TESResponse*, RE::TESFile*);
	using RNAMInsertFunction = std::uint8_t (*)(void*, const void*, const RE::BGSLocalizedString*, void*);
	using InfoInitFunction = void (*)(RE::TESTopicInfo*);

	enum class InfoTextPhase : std::uint8_t
	{
		kCapturing,
		kStartup,
		kReady,
		kDisabled
	};

	struct OriginalInfoPrompt
	{
		bool hadDirectRNAM{ false };
		std::string originalPrompt;
		std::string originalDirectPrompt;
		std::string originalProvider;
	};

	struct TESResponseView
	{
		RE::BGSLocalizedString text;  // 00
		RE::TESResponse* next;        // 08
		std::uint16_t field10;        // 10
		std::uint8_t field12;         // 12
		std::uint8_t field13;         // 13
		std::uint16_t field14;        // 14
	};
	static_assert(offsetof(TESResponseView, text) == 0x00);
	static_assert(offsetof(TESResponseView, next) == 0x08);
	static_assert(sizeof(TESResponseView) == 0x18);

	struct PlayerDialogueActionView
	{
		std::array<std::uint8_t, 0x20> prefix;
		std::array<RE::TESTopic*, 4> playerTopics;
	};
	static_assert(offsetof(PlayerDialogueActionView, playerTopics) == 0x20);

	struct OriginalPromptText
	{
		std::string text;
		std::string providerPlugin;
	};

	struct PendingResponseRewrite
	{
		std::size_t index{ 0 };
		std::string original;
		std::string final;
		std::string providerPlugin;
	};

	struct PendingInfoRewrite
	{
		RE::TESTopicInfo* info{ nullptr };
		std::uint32_t formID{ 0 };
		bool customConditions{ false };
		bool hadDirectRNAM{ false };
		std::string originalDirectPrompt;
		std::string originalPrompt;
		std::string promptProviderPlugin;
		std::string finalPrompt;
		std::vector<std::string> originalResponses;
		std::vector<PendingResponseRewrite> responseRewrites;
	};

	struct InfoTextStats
	{
		std::uint32_t directChanged{ 0 };
		std::uint32_t fallbackChanged{ 0 };
		std::uint32_t responseChanged{ 0 };
	};

	DialoguePromptGetter g_promptGetter{ nullptr };
	DialoguePromptSetter g_promptSetter{ nullptr };
	DialoguePromptFallback g_promptFallback{ nullptr };
	LocalizedSubrecordLoadFunction g_rnamLocalizedLoadOriginal{ nullptr };
	ResponseTextLoadFunction g_responseTextLoadOriginal{ nullptr };
	RNAMInsertFunction g_rnamInsertOriginal{ nullptr };
	InfoInitFunction g_infoInitOriginal{ nullptr };
	bool g_infoEarlyHookAttempted{ false };
	std::atomic<InfoTextPhase> g_infoTextPhase{ InfoTextPhase::kCapturing };
	std::atomic<std::uint32_t> g_dialogueChoicesChanged{ 0 };
	std::atomic<std::uint32_t> g_infoSetterCalls{ 0 };
	std::atomic<std::uint32_t> g_infoDirectChanges{ 0 };
	std::atomic<std::uint32_t> g_infoFallbackChanges{ 0 };
	std::atomic<std::uint32_t> g_infoResponseWrites{ 0 };
	std::atomic<std::uint32_t> g_infoResponseChanges{ 0 };
	std::mutex g_infoPromptMutex;
	std::unordered_map<std::uint32_t, std::string> g_directPromptCaptures;
	std::unordered_map<std::uint32_t, std::string> g_directPromptProviders;
	std::unordered_map<const RE::TESResponse*, std::string> g_responseProviders;
	std::unordered_map<std::uint32_t, OriginalInfoPrompt> g_originalInfoPrompts;
	std::unordered_map<std::uint32_t, std::vector<std::string>> g_originalInfoResponses;
	std::unordered_set<std::uint32_t> g_changedInfoIDs;
	std::unordered_set<std::uint32_t> g_playerDialogueTopicIDs;
	std::unordered_set<std::uint32_t> g_startupPendingInfos;
	thread_local const RE::TESFile* g_pendingRNAMProviderFile{ nullptr };
	constexpr std::uintptr_t kResponseTextLoadCallOffset = 0x1C1;
	constexpr std::uintptr_t kRNAMLocalizedLoadCallOffset = 0x5BE;
	constexpr std::uintptr_t kRNAMInsertCallOffset = 0x5DA;
	constexpr std::size_t kDirectCallSize = 5;
	constexpr std::size_t kInfoInitVtableSlot = 0x16;
	constexpr std::size_t kDialogueRewriteLogCapacity = 4096;
	std::mutex g_dialogueRewriteLogMutex;
	std::array<std::uint64_t, kDialogueRewriteLogCapacity> g_dialogueRewriteLogKeys{};
	std::size_t g_dialogueRewriteLogCount{ 0 };

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
			return fmt::format("{}|{:08X}", file->GetFilename(), localID);
		}
		return fmt::format("{:08X}", formID);
	}

	// Builds INFO source details for logging when an override supplied the text.
	std::string DescribeInfo(const RE::TESTopicInfo* a_info, std::string_view a_providerPlugin)
	{
		if (!a_info) {
			return "<unknown>";
		}
		const auto formID = a_info->GetFormID();
		const auto* files = a_info->sourceFiles.array;
		const auto* origin = files && !files->empty() ? (*files)[0] : nullptr;
		if (!origin) {
			return DescribeForm(a_info);
		}
		const auto originName = origin->GetFilename();
		const auto localID = origin->IsLight() ? formID & 0x00000FFF : formID & 0x00FFFFFF;
		if (!a_providerPlugin.empty() && a_providerPlugin != originName) {
			return fmt::format("{}|{}|{:08X}", a_providerPlugin, originName, localID);
		}
		return fmt::format("{}|{:08X}", originName, localID);
	}

	// Adds unique configured perks from one condition list.
	void AddConditionPerks(RE::TESConditionItem* a_head, std::vector<std::uintptr_t>& a_sources)
	{
    		for (auto* item = a_head; item; item = item->next) {
        		auto* source = PCF::PerkConditions::GetConditionPerk(item);
        			if (!source) {
            				continue;
        			}

        			const auto identity = reinterpret_cast<std::uintptr_t>(source);
        			if (std::find(a_sources.begin(), a_sources.end(), identity) == a_sources.end()) {
           			 a_sources.push_back(identity);
        		}
    		}
	}

	// Removes perks that do not have an active PCF rule.
	void KeepConfiguredPerks(std::vector<std::uintptr_t>& a_sources)
	{
		a_sources.erase(std::remove_if(a_sources.begin(), a_sources.end(), [](std::uintptr_t a_source) {
			auto* perk = reinterpret_cast<RE::BGSPerk*>(a_source);
			return !perk || !PCF::RuleRegistry::Find(perk);
		}), a_sources.end());
	}


	// Trims ASCII whitespace without changing the owned dialogue string.
	std::string_view TrimRequirementWhitespace(std::string_view a_text)
	{
		while (!a_text.empty() && std::isspace(static_cast<unsigned char>(a_text.front()))) {
			a_text.remove_prefix(1);
		}
		while (!a_text.empty() && std::isspace(static_cast<unsigned char>(a_text.back()))) {
			a_text.remove_suffix(1);
		}
		return a_text;
	}

	// Compares requirement labels without caring about case.
	bool SameRequirementText(std::string_view a_left, std::string_view a_right)
	{
		a_left = TrimRequirementWhitespace(a_left);
		a_right = TrimRequirementWhitespace(a_right);
		return a_left.size() == a_right.size() && std::equal(a_left.begin(), a_left.end(), a_right.begin(),
			[](char a_leftCharacter, char a_rightCharacter) {
				return std::tolower(static_cast<unsigned char>(a_leftCharacter)) ==
					std::tolower(static_cast<unsigned char>(a_rightCharacter));
			});
	}

	// Checks whether a leading marker names one of the discarded native HasPerk requirements.
	bool IsDiscardedPerkMarker(const RE::TESTopicInfo* a_info, std::string_view a_marker)
	{
		if (!a_info) {
			return false;
		}
		for (auto* item = a_info->objConditions.head; item; item = item->next) {
   			auto* source = PCF::PerkConditions::GetConditionPerk(item);
    			if (!source) {
        			continue;
    		}
			const auto matches = [&](RE::BGSPerk* a_perk) {
				if (!a_perk) {
					return false;
				}
				const auto name = RE::TESFullName::GetFullName(*a_perk, false);
				return !name.empty() && SameRequirementText(a_marker, name);
			};
			if (matches(source)) {
				return true;
			}
			const auto rank = PCF::RuleRegistry::GetRank(source);
			if (rank.base && rank.base != source && matches(rank.base)) {
				return true;
			}
		}
		return false;
	}

	// Recognizes compact numeric requirement tags used by dialogue patches, such as (PER 2) or [Science 75+].
	bool IsNumericRequirementMarker(std::string_view a_marker)
	{
		a_marker = TrimRequirementWhitespace(a_marker);
		if (a_marker.empty()) {
			return false;
		}
		bool hasDigit = false;
		bool hasComparison = false;
		for (const char character : a_marker) {
			const auto value = static_cast<unsigned char>(character);
			if (std::isdigit(value)) {
				hasDigit = true;
				continue;
			}
			if (std::isalpha(value) || std::isspace(value) || character == '_' || character == '-') {
				continue;
			}
			if (character == '+' || character == '<' || character == '>' || character == '=') {
				hasComparison = true;
				continue;
			}
			if (character == '.') {
				continue;
			}
			return false;
		}
		if (!hasDigit) {
			return false;
		}
		if (hasComparison) {
			return true;
		}

		const auto separator = a_marker.find_first_of(" \t");
		if (separator == std::string_view::npos) {
			return false;
		}
		const auto name = TrimRequirementWhitespace(a_marker.substr(0, separator));
		constexpr std::array<std::string_view, 14> knownNumericRequirements{
			"STR", "PER", "END", "CHA", "INT", "AGI", "LCK",
			"Strength", "Perception", "Endurance", "Charisma", "Intelligence", "Agility", "Luck"
		};
		return std::any_of(knownNumericRequirements.begin(), knownNumericRequirements.end(),
			[&](std::string_view a_known) { return SameRequirementText(name, a_known); });
	}

	// Removes only leading legacy requirement decorations from an INFO explicitly owned by [Conditions].
	// The dialogue sentence itself remains the final winning/canonical text supplied by the load order.
	std::string RemoveDiscardedRequirementMarkers(const RE::TESTopicInfo* a_info, std::string_view a_text)
	{
		while (!a_text.empty() && std::isspace(static_cast<unsigned char>(a_text.front()))) {
			a_text.remove_prefix(1);
		}
		while (!a_text.empty() && (a_text.front() == '[' || a_text.front() == '(')) {
			const char close = a_text.front() == '[' ? ']' : ')';
			const auto end = a_text.find(close, 1);
			if (end == std::string_view::npos) {
				break;
			}
			const auto marker = TrimRequirementWhitespace(a_text.substr(1, end - 1));
			if (!IsDiscardedPerkMarker(a_info, marker) && !IsNumericRequirementMarker(marker)) {
				break;
			}
			a_text.remove_prefix(end + 1);
			while (!a_text.empty() && std::isspace(static_cast<unsigned char>(a_text.front()))) {
				a_text.remove_prefix(1);
			}
		}
		return std::string(a_text);
	}

	// Builds a short key so the same INFO change is only logged once.
	std::uint64_t DialogueRewriteKey(const RE::TESTopicInfo* a_info, std::string_view a_original, std::string_view a_final,
		std::uint32_t a_surface) noexcept
	{
		constexpr std::uint64_t offsetBasis = 1469598103934665603ULL;
		constexpr std::uint64_t prime = 1099511628211ULL;
		std::uint64_t hash = offsetBasis;
		const auto mixByte = [&](std::uint8_t a_value) {
			hash ^= a_value;
			hash *= prime;
		};
		const auto formID = a_info ? a_info->GetFormID() : 0u;
		for (std::size_t shift = 0; shift < sizeof(formID); ++shift) {
			mixByte(static_cast<std::uint8_t>((formID >> (shift * 8)) & 0xFFu));
		}
		for (std::size_t shift = 0; shift < sizeof(a_surface); ++shift) {
			mixByte(static_cast<std::uint8_t>((a_surface >> (shift * 8)) & 0xFFu));
		}
		for (const auto character : a_original) {
			mixByte(static_cast<std::uint8_t>(character));
		}
		mixByte(0xFFu);
		for (const auto character : a_final) {
			mixByte(static_cast<std::uint8_t>(character));
		}
		return hash ? hash : 1ULL;
	}

	// Logs each visible INFO change once and can delay startup logs until the result section.
	bool RecordDialogueRewrite(RE::TESTopicInfo* a_info, std::string_view a_original, std::string_view a_final,
		std::string_view a_providerPlugin, std::vector<std::string>* a_deferredLogs = nullptr, std::uint32_t a_surface = 0,
		bool a_customConditions = false)
	{
		const auto key = DialogueRewriteKey(a_info, a_original, a_final, a_surface);
		{
			std::scoped_lock lock(g_dialogueRewriteLogMutex);
			if (std::find(g_dialogueRewriteLogKeys.begin(), g_dialogueRewriteLogKeys.begin() + g_dialogueRewriteLogCount, key) !=
				g_dialogueRewriteLogKeys.begin() + g_dialogueRewriteLogCount) {
				return false;
			}
			if (g_dialogueRewriteLogCount >= g_dialogueRewriteLogKeys.size()) {
				return false;
			}
			g_dialogueRewriteLogKeys[g_dialogueRewriteLogCount++] = key;
		}
		try {
			const auto label = a_customConditions ? "INFO Custom Conditions" : "INFO";
			auto line = fmt::format("{} [{}]: \"{}\" -> \"{}\"",
				DescribeInfo(a_info, a_providerPlugin), label, a_original, a_final);
			if (a_deferredLogs) {
				a_deferredLogs->push_back(std::move(line));
			} else {
				spdlog::info("{}", line);
			}
		} catch (...) {
		}
		return true;
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

	// Finds the topics used by PlayerDialogue player choices.
	void FindPlayerDialogueTopics(RE::TESDataHandler* a_data)
	{
		std::unordered_set<std::uint32_t> topics;
		if (a_data) {
			REL::Relocation<std::uintptr_t> playerDialogueVtable{ RE::VTABLE::BGSSceneActionPlayerDialogue[0] };
			const auto expectedVtable = playerDialogueVtable.address();
			for (auto* scene : a_data->GetFormArray<RE::BGSScene>()) {
				if (!scene) {
					continue;
				}
				for (auto* action : scene->actions) {
					if (!action || *reinterpret_cast<const std::uintptr_t*>(action) != expectedVtable) {
						continue;
					}
					const auto* view = reinterpret_cast<const PlayerDialogueActionView*>(action);
					for (auto* storedTopic : view->playerTopics) {
						auto* topic = storedTopic;
						const auto raw = reinterpret_cast<std::uintptr_t>(storedTopic);
						if (raw && raw <= std::numeric_limits<std::uint32_t>::max()) {
							topic = RE::TESForm::GetFormByID<RE::TESTopic>(static_cast<std::uint32_t>(raw));
						}
						if (topic) {
							topics.insert(topic->GetFormID());
						}
					}
				}
			}
		}
		std::scoped_lock lock(g_infoPromptMutex);
		g_playerDialogueTopicIDs = std::move(topics);
	}

	// Checks whether an INFO belongs to a PlayerDialogue player choice.
	bool IsPlayerDialogue(const RE::TESTopicInfo* a_info)
	{
		if (!a_info || !a_info->parentTopic) {
			return false;
		}
		const auto topicID = a_info->parentTopic->GetFormID();
		std::scoped_lock lock(g_infoPromptMutex);
		return g_playerDialogueTopicIDs.contains(topicID);
	}

	// Saves the original response text before PCF changes it.
	std::vector<std::string> SaveOriginalResponses(RE::TESTopicInfo* a_info)
	{
		if (!a_info) {
			return {};
		}
		{
			std::scoped_lock lock(g_infoPromptMutex);
			if (const auto original = g_originalInfoResponses.find(a_info->GetFormID()); original != g_originalInfoResponses.end()) {
				return original->second;
			}
		}
		std::vector<std::string> responses;
		for (auto* response = a_info->responses.head; response;) {
			const auto* view = reinterpret_cast<const TESResponseView*>(response);
			responses.emplace_back(GetVisibleText(static_cast<std::string_view>(view->text)));
			response = view->next;
		}
		return responses;
	}

	// Saves which plugin supplied each response in the original order.
	std::vector<std::string> SaveResponseSources(RE::TESTopicInfo* a_info)
	{
		std::vector<std::string> providers;
		if (!a_info) {
			return providers;
		}
		std::scoped_lock lock(g_infoPromptMutex);
		for (auto* response = a_info->responses.head; response;) {
			const auto provider = g_responseProviders.find(response);
			providers.push_back(provider != g_responseProviders.end() ? provider->second : std::string{});
			response = reinterpret_cast<const TESResponseView*>(response)->next;
		}
		return providers;
	}

	// Counts an INFO once when any player-visible text changes.
	void MarkInfoChanged(std::uint32_t a_formID)
	{
		bool inserted = false;
		{
			std::scoped_lock lock(g_infoPromptMutex);
			inserted = g_changedInfoIDs.insert(a_formID).second;
		}
		if (inserted) {
			g_dialogueChoicesChanged.fetch_add(1, std::memory_order_relaxed);
		}
	}


	// Lets the RNAM load continue and remembers which plugin supplied the text.
	void RNAMLoadHook(RE::BGSLocalizedString* a_value, RE::TESFile* a_file)
	{
		g_pendingRNAMProviderFile = nullptr;
		if (g_rnamLocalizedLoadOriginal) {
			g_rnamLocalizedLoadOriginal(a_value, a_file);
		}
		if (g_infoTextPhase.load(std::memory_order_acquire) != InfoTextPhase::kDisabled) {
			g_pendingRNAMProviderFile = a_file;
		}
	}

	// Lets the response load continue and remembers which plugin supplied the text.
	void ResponseLoadHook(RE::TESResponse* a_response, RE::TESFile* a_file)
	{
		if (g_responseTextLoadOriginal) {
			g_responseTextLoadOriginal(a_response, a_file);
		}
		if (!a_response || !a_file ||
			g_infoTextPhase.load(std::memory_order_acquire) == InfoTextPhase::kDisabled) {
			return;
		}
		std::scoped_lock lock(g_infoPromptMutex);
		g_responseProviders[a_response] = std::string(a_file->GetFilename());
	}

	// Lets the RNAM insert continue and saves its source before the temporary text is released.
	std::uint8_t RNAMInsertHook(void* a_map, const void* a_key, const RE::BGSLocalizedString* a_value, void* a_result)
	{
		const auto* providerFile = g_pendingRNAMProviderFile;
		g_pendingRNAMProviderFile = nullptr;
		const std::uint8_t inserted = g_rnamInsertOriginal ? g_rnamInsertOriginal(a_map, a_key, a_value, a_result) : std::uint8_t{ 0 };
		if (!inserted || !a_key || !a_value ||
			g_infoTextPhase.load(std::memory_order_acquire) == InfoTextPhase::kDisabled) {
			return inserted;
		}
		auto* info = *reinterpret_cast<RE::TESTopicInfo* const*>(a_key);
		if (!info) {
			return inserted;
		}
		std::string direct(static_cast<std::string_view>(*a_value));
		{
			std::scoped_lock lock(g_infoPromptMutex);
			g_directPromptCaptures.try_emplace(info->GetFormID(), direct);
			if (providerFile) {
				g_directPromptProviders[info->GetFormID()] = std::string(providerFile->GetFilename());
			}
		}
		return inserted;
	}

	// Returns the saved source information for one INFO prompt.
	std::pair<bool, std::string> GetSavedPrompt(std::uint32_t a_formID)
	{
		std::scoped_lock lock(g_infoPromptMutex);
		if (const auto original = g_originalInfoPrompts.find(a_formID); original != g_originalInfoPrompts.end()) {
			return { original->second.hadDirectRNAM, original->second.originalDirectPrompt };
		}
		if (const auto captured = g_directPromptCaptures.find(a_formID); captured != g_directPromptCaptures.end()) {
			return { true, captured->second };
		}
		return {};
	}

	// Returns the plugin that supplied one RNAM field.
	std::string GetPromptSource(std::uint32_t a_formID)
	{
		std::scoped_lock lock(g_infoPromptMutex);
		if (const auto original = g_originalInfoPrompts.find(a_formID); original != g_originalInfoPrompts.end()) {
			return original->second.originalProvider;
		}
		if (const auto provider = g_directPromptProviders.find(a_formID); provider != g_directPromptProviders.end()) {
			return provider->second;
		}
		return {};
	}

	// Finds the original visible text and the plugin that supplied it.
	std::optional<OriginalPromptText> FindOriginalPrompt(RE::TESTopicInfo* a_info, std::unordered_set<std::uint32_t>& a_visited)
	{
		if (!a_info) {
			return std::nullopt;
		}
		const auto formID = a_info->GetFormID();
		if (!a_visited.insert(formID).second) {
			return std::nullopt;
		}
		{
			std::scoped_lock lock(g_infoPromptMutex);
			if (const auto original = g_originalInfoPrompts.find(formID); original != g_originalInfoPrompts.end()) {
				return OriginalPromptText{ original->second.originalPrompt, original->second.originalProvider };
			}
			if (const auto captured = g_directPromptCaptures.find(formID); captured != g_directPromptCaptures.end()) {
				const auto provider = g_directPromptProviders.find(formID);
				return OriginalPromptText{ std::string(GetVisibleText(captured->second)),
					provider != g_directPromptProviders.end() ? provider->second : std::string{} };
			}
		}
		if (g_promptFallback) {
			auto* fallback = g_promptFallback(a_info);
			if (fallback && fallback != a_info) {
				return FindOriginalPrompt(fallback, a_visited);
			}
		}
		const char* current = g_promptGetter ? g_promptGetter(a_info) : nullptr;
		return current && current[0] ?
			std::optional<OriginalPromptText>{ OriginalPromptText{ std::string(GetVisibleText(current)), {} } } : std::nullopt;
	}

	// Gets an INFO prompt from its saved original text instead of PCF-generated text.
	std::optional<OriginalPromptText> GetOriginalPrompt(RE::TESTopicInfo* a_info)
	{
		std::unordered_set<std::uint32_t> visited;
		return FindOriginalPrompt(a_info, visited);
	}

	// Builds the new player-visible INFO text without writing it yet.
	std::optional<PendingInfoRewrite> BuildInfoText(RE::TESTopicInfo* a_info)
	{
		if (!a_info) {
			return std::nullopt;
		}
		// Use the exact native condition-list ownership key here. This is the same key the gameplay
		// TESCondition::IsTrue hook consults, so presentation cannot disagree with gameplay even if a
		// nested INFO is represented by a different TESForm view elsewhere.
		const auto* customSet = PCF::CustomConditions::Find(&a_info->objConditions);
		const bool customOwned = customSet && customSet->owner == PCF::CustomConditions::OwnerKind::kDialogue;

		// Explicit [Conditions] ownership is authoritative for both gameplay and requirement presentation.
		// Never send the same INFO through the ordinary native/perk requirement rewrite path as well.
		if (customOwned) {
			if (customSet->dialoguePrefix.empty()) {
				return std::nullopt;
			}

			PendingInfoRewrite pending;
			pending.info = a_info;
			pending.formID = a_info->GetFormID();
			pending.customConditions = true;

			// RNAM is the short player-choice prompt used by the vanilla dialogue UI.
			if (const auto original = GetOriginalPrompt(a_info); original && !original->text.empty()) {
				pending.originalPrompt = original->text;
				pending.promptProviderPlugin = original->providerPlugin;

				std::string cleanPrompt = RemoveDiscardedRequirementMarkers(a_info, original->text);
				std::string finalPrompt;
				finalPrompt.reserve(customSet->dialoguePrefix.size() + cleanPrompt.size());
				finalPrompt.append(customSet->dialoguePrefix);
				finalPrompt.append(cleanPrompt);
				if (finalPrompt != original->text) {
					pending.finalPrompt = std::move(finalPrompt);
					const auto [hadDirectRNAM, originalDirectPrompt] = GetSavedPrompt(pending.formID);
					pending.hadDirectRNAM = hadDirectRNAM;
					pending.originalDirectPrompt = originalDirectPrompt;
					if (pending.promptProviderPlugin.empty() && hadDirectRNAM) {
						pending.promptProviderPlugin = GetPromptSource(pending.formID);
					}
				}
			}

			// XDI/full-dialogue interfaces display the player response text (NAM1 surface) instead of, or
			// alongside, the short RNAM prompt. Apply the same direct-ownership replacement to that already
			// established player-response path so stale native/modded requirements cannot survive there.
			if (IsPlayerDialogue(a_info)) {
				pending.originalResponses = SaveOriginalResponses(a_info);
				const auto responseProviders = SaveResponseSources(a_info);
				for (std::size_t index = 0; index < pending.originalResponses.size(); ++index) {
					const auto& original = pending.originalResponses[index];
					if (original.empty()) {
						continue;
					}
					std::string cleanResponse = RemoveDiscardedRequirementMarkers(a_info, original);
					std::string finalResponse;
					finalResponse.reserve(customSet->dialoguePrefix.size() + cleanResponse.size());
					finalResponse.append(customSet->dialoguePrefix);
					finalResponse.append(cleanResponse);
					if (finalResponse != original) {
						pending.responseRewrites.push_back({ index, original, std::move(finalResponse),
							index < responseProviders.size() ? responseProviders[index] : std::string{} });
					}
				}
			}

			if (pending.finalPrompt.empty() && pending.responseRewrites.empty()) {
				return std::nullopt;
			}
			return pending;
		}

		// Normal INFOs retain the established PCF native/perk replacement pipeline unchanged.
		std::vector<std::uintptr_t> preferredSources;
		AddConditionPerks(a_info->objConditions.head, preferredSources);
		KeepConfiguredPerks(preferredSources);
		if (preferredSources.empty()) {
			return std::nullopt;
		}

		PendingInfoRewrite pending;
		pending.info = a_info;
		pending.formID = a_info->GetFormID();
		if (const auto original = GetOriginalPrompt(a_info); original && !original->text.empty()) {
			std::string final = original->text;
			if (const auto rewritten = PCF::TextManager::RewriteText(final, preferredSources, false, true)) {
				final = *rewritten;
			}
			if (final != original->text) {
				const auto [hadDirectRNAM, originalDirectPrompt] = GetSavedPrompt(pending.formID);
				pending.hadDirectRNAM = hadDirectRNAM;
				pending.originalDirectPrompt = originalDirectPrompt;
				pending.originalPrompt = original->text;
				pending.promptProviderPlugin = original->providerPlugin;
				if (pending.promptProviderPlugin.empty() && hadDirectRNAM) {
					pending.promptProviderPlugin = GetPromptSource(pending.formID);
				}
				pending.finalPrompt = std::move(final);
			}
		}

		if (IsPlayerDialogue(a_info)) {
			pending.originalResponses = SaveOriginalResponses(a_info);
			const auto responseProviders = SaveResponseSources(a_info);
			for (std::size_t index = 0; index < pending.originalResponses.size(); ++index) {
				const auto& original = pending.originalResponses[index];
				if (original.empty()) {
					continue;
				}
				const auto rewritten = PCF::TextManager::RewriteText(original, preferredSources, false, true);
				if (rewritten && *rewritten != original) {
					pending.responseRewrites.push_back({ index, original, *rewritten,
						index < responseProviders.size() ? responseProviders[index] : std::string{} });
				}
			}
		}

		if (pending.finalPrompt.empty() && pending.responseRewrites.empty()) {
			return std::nullopt;
		}
		return pending;
	}

	// Writes the prepared INFO text back to the correct game-owned strings.
	bool ApplyInfoText(const PendingInfoRewrite& a_pending, std::vector<std::string>* a_deferredAuditLogs = nullptr)
	{
		if (!a_pending.info) {
			return false;
		}

		bool changed = false;
		if (!a_pending.finalPrompt.empty() && g_promptSetter && g_promptGetter) {
			const char* currentRaw = g_promptGetter(a_pending.info);
			const std::string current = currentRaw && currentRaw[0] ? std::string(GetVisibleText(currentRaw)) : std::string{};
			if (current != a_pending.finalPrompt) {
				g_promptSetter(a_pending.info, a_pending.finalPrompt.c_str());
				g_infoSetterCalls.fetch_add(1, std::memory_order_relaxed);
				const char* afterRaw = g_promptGetter(a_pending.info);
				const std::string after = afterRaw && afterRaw[0] ? std::string(GetVisibleText(afterRaw)) : std::string{};
				if (after != a_pending.finalPrompt) {
					spdlog::warn("INFO RNAM text check failed for {}", DescribeForm(a_pending.info));
				} else {
					{
						std::scoped_lock lock(g_infoPromptMutex);
						g_originalInfoPrompts.try_emplace(a_pending.formID,
							OriginalInfoPrompt{ a_pending.hadDirectRNAM, a_pending.originalPrompt,
								a_pending.originalDirectPrompt, a_pending.promptProviderPlugin });
					}
					if (a_pending.hadDirectRNAM) {
						g_infoDirectChanges.fetch_add(1, std::memory_order_relaxed);
					} else {
						g_infoFallbackChanges.fetch_add(1, std::memory_order_relaxed);
					}
					RecordDialogueRewrite(a_pending.info, a_pending.originalPrompt, a_pending.finalPrompt,
						a_pending.promptProviderPlugin, a_deferredAuditLogs, 0, a_pending.customConditions);
					changed = true;
				}
			}
		}

		if (!a_pending.responseRewrites.empty() && IsPlayerDialogue(a_pending.info)) {
			{
				std::scoped_lock lock(g_infoPromptMutex);
				g_originalInfoResponses.try_emplace(a_pending.formID, a_pending.originalResponses);
			}
			std::size_t index = 0;
			for (auto* response = a_pending.info->responses.head; response; ++index) {
				auto* view = reinterpret_cast<TESResponseView*>(response);
				const auto rewrite = std::find_if(a_pending.responseRewrites.begin(), a_pending.responseRewrites.end(),
					[index](const PendingResponseRewrite& a_entry) { return a_entry.index == index; });
				if (rewrite != a_pending.responseRewrites.end()) {
					const auto current = std::string(GetVisibleText(static_cast<std::string_view>(view->text)));
					if (current != rewrite->final) {
						view->text = std::string_view(rewrite->final);
						g_infoResponseWrites.fetch_add(1, std::memory_order_relaxed);
						const auto after = std::string(GetVisibleText(static_cast<std::string_view>(view->text)));
						if (after == rewrite->final) {
							g_infoResponseChanges.fetch_add(1, std::memory_order_relaxed);
							RecordDialogueRewrite(a_pending.info, rewrite->original, rewrite->final, rewrite->providerPlugin,
								a_deferredAuditLogs, static_cast<std::uint32_t>(index + 1), a_pending.customConditions);
							changed = true;
						} else {
							spdlog::warn("INFO player-response verification failed for {}", DescribeForm(a_pending.info));
						}
					}
				}
				response = view->next;
			}
		}

		if (changed) {
			MarkInfoChanged(a_pending.formID);
		}
		return changed;
	}

	// Clears saved prompt source data after the INFO is finished.
	void ClearPromptCapture(std::uint32_t a_formID)
	{
		std::scoped_lock lock(g_infoPromptMutex);
		g_directPromptCaptures.erase(a_formID);
	}

	// Clears saved response source data after the INFO is finished.
	void ClearResponseSources(RE::TESTopicInfo* a_info)
	{
		if (!a_info) {
			return;
		}
		std::scoped_lock lock(g_infoPromptMutex);
		for (auto* response = a_info->responses.head; response;) {
			g_responseProviders.erase(response);
			response = reinterpret_cast<const TESResponseView*>(response)->next;
		}
	}

	// Updates one INFO loaded after startup using its original text.
	void UpdateInfo(RE::TESTopicInfo* a_info, std::vector<std::string>* a_deferredAuditLogs = nullptr)
	{
		if (!a_info) {
			return;
		}
		const auto pending = BuildInfoText(a_info);
		if (pending) {
			ApplyInfoText(*pending, a_deferredAuditLogs);
		}
		ClearPromptCapture(a_info->GetFormID());
		ClearResponseSources(a_info);
	}

	// Runs the game load first, then updates INFO records loaded after PCF is ready.
	void InfoLoadHook(RE::TESTopicInfo* a_info)
	{
		if (g_infoInitOriginal) {
			g_infoInitOriginal(a_info);
		}
		if (!a_info) {
			return;
		}
		auto phase = g_infoTextPhase.load(std::memory_order_acquire);
		if (phase == InfoTextPhase::kReady) {
			UpdateInfo(a_info);
			return;
		}
		if (phase != InfoTextPhase::kStartup) {
			return;
		}
		bool processNow = false;
		{
			std::scoped_lock lock(g_infoPromptMutex);
			phase = g_infoTextPhase.load(std::memory_order_relaxed);
			if (phase == InfoTextPhase::kStartup) {
				g_startupPendingInfos.insert(a_info->GetFormID());
			} else if (phase == InfoTextPhase::kReady) {
				processNow = true;
			}
		}
		if (processNow) {
			UpdateInfo(a_info);
		}
	}

	// Visits loaded INFO records through their DIAL records because Fallout leaves formArrays[kINFO] empty.
	template <class Visitor>
	bool VisitLoadedInfo(RE::TESDataHandler* a_data, Visitor&& a_visitor)
	{
		if (!a_data) {
			return false;
		}
		std::unordered_set<std::uint32_t> seen;
		for (auto* topic : a_data->GetFormArray<RE::TESTopic>()) {
			if (!topic || !topic->topicInfos || !topic->numTopicInfos) {
				continue;
			}
			for (std::uint32_t index = 0; index < topic->numTopicInfos; ++index) {
				auto* info = topic->topicInfos[index];
				if (!info || !seen.insert(info->GetFormID()).second) {
					continue;
				}
				if (a_visitor(info)) {
					return true;
				}
			}
		}
		return false;
	}

	// Finds loaded INFO records through the shared DIAL traversal.
	std::vector<RE::TESTopicInfo*> CollectLoadedInfo(RE::TESDataHandler* a_data)
	{
		std::vector<RE::TESTopicInfo*> infos;
		VisitLoadedInfo(a_data, [&infos](RE::TESTopicInfo* a_info) {
			infos.push_back(a_info);
			return false;
		});
		return infos;
	}

	// Updates startup INFOs in two passes: build changes first, then apply them.
	InfoTextStats UpdateStartupInfo(std::vector<std::string>& a_infoLogLines,
		const std::vector<RE::TESTopicInfo*>* a_startupInfos)
	{
		InfoTextStats stats;
		if (g_infoTextPhase.load(std::memory_order_acquire) == InfoTextPhase::kDisabled ||
			!g_rnamInsertOriginal || !g_infoInitOriginal || !g_promptGetter || !g_promptSetter || !g_promptFallback) {
			a_infoLogLines.emplace_back("INFO text updates unavailable; game dialogue text left unchanged");
			return stats;
		}
		auto* data = RE::TESDataHandler::GetSingleton();
		if (!data) {
			return stats;
		}
		FindPlayerDialogueTopics(data);

		g_infoTextPhase.store(InfoTextPhase::kStartup, std::memory_order_release);
		const auto setterBefore = g_infoSetterCalls.load(std::memory_order_relaxed);
		const auto responseWritesBefore = g_infoResponseWrites.load(std::memory_order_relaxed);
		const auto responseChangesBefore = g_infoResponseChanges.load(std::memory_order_relaxed);
		const auto directBefore = g_infoDirectChanges.load(std::memory_order_relaxed);
		const auto fallbackBefore = g_infoFallbackChanges.load(std::memory_order_relaxed);
		std::vector<PendingInfoRewrite> pending;
		std::vector<std::uint32_t> enumeratedIDs;
		std::vector<RE::TESTopicInfo*> collectedInfos;
		if (!a_startupInfos) {
			collectedInfos = CollectLoadedInfo(data);
			a_startupInfos = std::addressof(collectedInfos);
		}
		const auto& infos = *a_startupInfos;
		pending.reserve(infos.size() / 8);
		enumeratedIDs.reserve(infos.size());

		for (auto* info : infos) {
			if (!info) {
				continue;
			}
			enumeratedIDs.push_back(info->GetFormID());
			if (auto rewrite = BuildInfoText(info)) {
				pending.push_back(std::move(*rewrite));
			}
		}

		const auto pass1SetterCalls = g_infoSetterCalls.load(std::memory_order_relaxed) - setterBefore;
		const auto pass1ResponseWrites = g_infoResponseWrites.load(std::memory_order_relaxed) - responseWritesBefore;
		if (pass1SetterCalls != 0 || pass1ResponseWrites != 0) {
			g_infoTextPhase.store(InfoTextPhase::kDisabled, std::memory_order_release);
			spdlog::error("INFO text updates disabled: startup build pass changed game text");
			return stats;
		}

		for (const auto& rewrite : pending) {
			ApplyInfoText(rewrite, std::addressof(a_infoLogLines));
		}
		{
			std::scoped_lock lock(g_infoPromptMutex);
			for (std::size_t index = 0; index < enumeratedIDs.size(); ++index) {
				g_directPromptCaptures.erase(enumeratedIDs[index]);
				if (index < infos.size() && infos[index]) {
					for (auto* response = infos[index]->responses.head; response;) {
						g_responseProviders.erase(response);
						response = reinterpret_cast<const TESResponseView*>(response)->next;
					}
				}
			}
		}

		for (;;) {
			std::vector<std::uint32_t> deferred;
			{
				std::scoped_lock lock(g_infoPromptMutex);
				if (g_startupPendingInfos.empty()) {
					g_infoTextPhase.store(InfoTextPhase::kReady, std::memory_order_release);
					break;
				}
				deferred.assign(g_startupPendingInfos.begin(), g_startupPendingInfos.end());
				g_startupPendingInfos.clear();
			}
			for (const auto formID : deferred) {
				if (auto* info = RE::TESForm::GetFormByID<RE::TESTopicInfo>(formID)) {
					UpdateInfo(info, std::addressof(a_infoLogLines));
				}
			}
		}

		stats.directChanged = g_infoDirectChanges.load(std::memory_order_relaxed) - directBefore;
		stats.fallbackChanged = g_infoFallbackChanges.load(std::memory_order_relaxed) - fallbackBefore;
		stats.responseChanged = g_infoResponseChanges.load(std::memory_order_relaxed) - responseChangesBefore;
		return stats;
	}

	// Disables INFO text changes if a required game hook cannot be used safely.
	bool DisableInfoHooks(std::string_view a_reason)
	{
		g_infoTextPhase.store(InfoTextPhase::kDisabled, std::memory_order_release);
		spdlog::error("INFO RNAM hooks unavailable: {}; INFO text updates disabled", a_reason);
		return false;
	}

	// Makes sure the shared hook space is large enough for INFO and text hooks.
	bool EnsureHookSpace(std::size_t a_required)
	{
		auto& trampoline = F4SE::GetTrampoline();
		if (trampoline.empty()) {
			constexpr std::size_t trampolineSize = 512;
			const auto* api = F4SE::GetTrampolineInterface();
			auto* memory = api ? api->AllocateFromBranchPool(trampolineSize) : nullptr;
			if (!memory) {
				return false;
			}
			trampoline.set_trampoline(memory, trampolineSize);
		}
		return trampoline.free_size() >= a_required;
	}

	// Installs the INFO text hooks after checking the required game functions.
	bool InstallInfoHooks()
	{
		if (g_infoEarlyHookAttempted) {
			return g_infoTextPhase.load(std::memory_order_acquire) != InfoTextPhase::kDisabled;
		}
		g_infoEarlyHookAttempted = true;
		try {
			const auto load = REL::IDDatabase::get().resolve(PCF::EngineIDs::TESTopicInfoLoad);
			const auto init = REL::IDDatabase::get().resolve(PCF::EngineIDs::TESTopicInfoInitItemImpl);
			const auto localizedLoad = REL::IDDatabase::get().resolve(PCF::EngineIDs::LocalizedSubrecordLoad);
			const auto responseTextLoad = REL::IDDatabase::get().resolve(PCF::EngineIDs::TESResponseTextLoad);
			const auto fallback = REL::IDDatabase::get().resolve(PCF::EngineIDs::DialoguePromptFallback);
			const auto getter = REL::IDDatabase::get().resolve(PCF::EngineIDs::DialoguePromptGetter);
			const auto setter = REL::IDDatabase::get().resolve(PCF::EngineIDs::DialoguePromptSetter);
			const auto insert = REL::IDDatabase::get().resolve(PCF::EngineIDs::DialoguePromptInsert);
			if (!load || !init || !localizedLoad || !responseTextLoad || !fallback || !getter || !setter || !insert) {
				return DisableInfoHooks("required relocation ID unavailable");
			}

			const auto base = REL::Module::get().base();
			const auto responseLoadCallsite = base + *load.rva + kResponseTextLoadCallOffset;
			const auto rnamLoadCallsite = base + *load.rva + kRNAMLocalizedLoadCallOffset;
			const auto captureCallsite = base + *load.rva + kRNAMInsertCallOffset;
			const auto expectedResponseTextLoad = base + *responseTextLoad.rva;
			const auto expectedLocalizedLoad = base + *localizedLoad.rva;
			const auto expectedInsert = base + *insert.rva;

			const auto validateCall = [](std::uintptr_t a_callsite, std::uintptr_t a_expected) {
				if (*reinterpret_cast<const std::uint8_t*>(a_callsite) != 0xE8) {
					return false;
				}
				std::int32_t relative = 0;
				std::memcpy(std::addressof(relative), reinterpret_cast<const void*>(a_callsite + 1), sizeof(relative));
				const auto actual = static_cast<std::uintptr_t>(
					static_cast<std::intptr_t>(a_callsite + kDirectCallSize) + static_cast<std::intptr_t>(relative));
				return actual == a_expected;
			};
			if (!validateCall(responseLoadCallsite, expectedResponseTextLoad)) {
				return DisableInfoHooks("response-text load site does not match TESResponseTextLoad");
			}
			if (!validateCall(rnamLoadCallsite, expectedLocalizedLoad)) {
				return DisableInfoHooks("RNAM localized-load site does not match LocalizedSubrecordLoad");
			}
			if (!validateCall(captureCallsite, expectedInsert)) {
				return DisableInfoHooks("RNAM insertion site does not match DialoguePromptInsert");
			}

			REL::Relocation<std::uintptr_t> vtable{ RE::TESTopicInfo::VTABLE[0] };
			const auto vtableAddress = vtable.address();
			const auto initSlot = vtableAddress + (sizeof(void*) * kInfoInitVtableSlot);
			const auto expectedInit = base + *init.rva;
			if (*reinterpret_cast<const std::uintptr_t*>(initSlot) != expectedInit) {
				return DisableInfoHooks("TESTopicInfo InitItemImpl vtable slot does not match the expected target");
			}
			if (!EnsureHookSpace(64)) {
				return DisableInfoHooks("branch-pool allocation failed");
			}

			g_promptFallback = reinterpret_cast<DialoguePromptFallback>(base + *fallback.rva);
			g_promptGetter = reinterpret_cast<DialoguePromptGetter>(base + *getter.rva);
			g_promptSetter = reinterpret_cast<DialoguePromptSetter>(base + *setter.rva);
			g_rnamLocalizedLoadOriginal = reinterpret_cast<LocalizedSubrecordLoadFunction>(expectedLocalizedLoad);
			g_responseTextLoadOriginal = reinterpret_cast<ResponseTextLoadFunction>(expectedResponseTextLoad);
			g_rnamInsertOriginal = reinterpret_cast<RNAMInsertFunction>(expectedInsert);
			g_infoInitOriginal = reinterpret_cast<InfoInitFunction>(expectedInit);

			auto& trampoline = F4SE::GetTrampoline();
			const auto restoreCallIfOwned = [&trampoline](std::uintptr_t a_callsite, std::uintptr_t a_replacement,
				std::uintptr_t a_original, std::string_view a_name) {
				if (!a_original || !PCF::NativeHooks::IsRelativeCallTo(a_callsite, a_replacement)) {
					return;
				}
				trampoline.write_call<5>(a_callsite, a_original);
				if (!PCF::NativeHooks::IsRelativeCallTo(a_callsite, a_original)) {
					spdlog::warn("INFO {} hook rollback could not be verified; PCF INFO behavior remains inactive", a_name);
				}
			};
			const auto restoreInitIfOwned = [&vtable, vtableAddress, initSlot](std::uintptr_t a_replacement, std::uintptr_t a_original) {
				if (!a_original || !PCF::NativeHooks::IsVtableSlotSet(vtableAddress, kInfoInitVtableSlot, a_replacement)) {
					return;
				}
				vtable.write_vfunc(kInfoInitVtableSlot, a_original);
				if (*reinterpret_cast<const std::uintptr_t*>(initSlot) != a_original) {
					spdlog::warn("INFO InitItemImpl hook rollback could not be verified; PCF INFO behavior remains inactive");
				}
			};

			const auto rnamReplacement = reinterpret_cast<std::uintptr_t>(&RNAMLoadHook);
			const auto responseReplacement = reinterpret_cast<std::uintptr_t>(&ResponseLoadHook);
			const auto insertReplacement = reinterpret_cast<std::uintptr_t>(&RNAMInsertHook);
			const auto initReplacement = reinterpret_cast<std::uintptr_t>(&InfoLoadHook);

			const auto previousRNAMLoad = trampoline.write_call<5>(rnamLoadCallsite, &RNAMLoadHook);
			if (previousRNAMLoad) {
				g_rnamLocalizedLoadOriginal = reinterpret_cast<LocalizedSubrecordLoadFunction>(previousRNAMLoad);
			}
			if (previousRNAMLoad != expectedLocalizedLoad ||
				!PCF::NativeHooks::IsRelativeCallTo(rnamLoadCallsite, rnamReplacement)) {
				restoreCallIfOwned(rnamLoadCallsite, rnamReplacement, previousRNAMLoad, "RNAM localized-load");
				return DisableInfoHooks("RNAM localized-load hook write could not be verified");
			}

			const auto previousResponseLoad = trampoline.write_call<5>(responseLoadCallsite, &ResponseLoadHook);
			if (previousResponseLoad) {
				g_responseTextLoadOriginal = reinterpret_cast<ResponseTextLoadFunction>(previousResponseLoad);
			}
			if (previousResponseLoad != expectedResponseTextLoad ||
				!PCF::NativeHooks::IsRelativeCallTo(responseLoadCallsite, responseReplacement)) {
				restoreCallIfOwned(responseLoadCallsite, responseReplacement, previousResponseLoad, "response-text load");
				restoreCallIfOwned(rnamLoadCallsite, rnamReplacement, previousRNAMLoad, "RNAM localized-load");
				return DisableInfoHooks("response-text load hook write could not be verified");
			}

			const auto previousInsert = trampoline.write_call<5>(captureCallsite, &RNAMInsertHook);
			if (previousInsert) {
				g_rnamInsertOriginal = reinterpret_cast<RNAMInsertFunction>(previousInsert);
			}
			if (previousInsert != expectedInsert ||
				!PCF::NativeHooks::IsRelativeCallTo(captureCallsite, insertReplacement)) {
				restoreCallIfOwned(captureCallsite, insertReplacement, previousInsert, "RNAM insertion");
				restoreCallIfOwned(responseLoadCallsite, responseReplacement, previousResponseLoad, "response-text load");
				restoreCallIfOwned(rnamLoadCallsite, rnamReplacement, previousRNAMLoad, "RNAM localized-load");
				return DisableInfoHooks("RNAM insertion hook write could not be verified");
			}

			const auto previousInit = vtable.write_vfunc(kInfoInitVtableSlot, &InfoLoadHook);
			if (previousInit) {
				g_infoInitOriginal = reinterpret_cast<InfoInitFunction>(previousInit);
			}
			if (previousInit != expectedInit ||
				!PCF::NativeHooks::IsVtableSlotSet(vtableAddress, kInfoInitVtableSlot, initReplacement)) {
				restoreInitIfOwned(initReplacement, previousInit);
				restoreCallIfOwned(captureCallsite, insertReplacement, previousInsert, "RNAM insertion");
				restoreCallIfOwned(responseLoadCallsite, responseReplacement, previousResponseLoad, "response-text load");
				restoreCallIfOwned(rnamLoadCallsite, rnamReplacement, previousRNAMLoad, "RNAM localized-load");
				return DisableInfoHooks("TESTopicInfo InitItemImpl hook write could not be verified");
			}
			g_infoTextPhase.store(InfoTextPhase::kCapturing, std::memory_order_release);
			return true;
		} catch (...) {
			return DisableInfoHooks("unexpected installation failure");
		}
	}

}

namespace PCF::DialogueText
{
	// Captures the authoritative dialogue-ready INFO snapshot used by both deferred binding and text startup.
	std::vector<RE::TESTopicInfo*> CollectLoadedInfoForStartup()
	{
		return CollectLoadedInfo(RE::TESDataHandler::GetSingleton());
	}

	// Updates TESTopic and DIAL text through the shared text system.
	Result UpdateTopics(std::vector<std::string>& a_topicLogLines)
	{
		Result result;
		auto* data = RE::TESDataHandler::GetSingleton();
		if (!data) {
			return result;
		}
		for (auto* topic : data->GetFormArray<RE::TESTopic>()) {
			if (!topic) {
				continue;
			}
			auto* fullName = static_cast<RE::TESFullName*>(topic);
			const auto original = GetVisibleText(static_cast<std::string_view>(fullName->fullName));
			const auto rewritten = PCF::TextManager::RewriteText(original);
			if (!rewritten) {
				continue;
			}
			// The log view still refers to this localized string.
			const RE::BGSLocalizedString originalOwner(fullName->fullName);
			fullName->fullName = std::string_view(*rewritten);
			++result.topicNamesChanged;
			try {
				a_topicLogLines.push_back(fmt::format("{} [DIAL]: \"{}\" -> \"{}\"", DescribeForm(topic), original, *rewritten));
			} catch (...) {
			}
		}
		result.dialogueChoicesChanged = g_dialogueChoicesChanged.load(std::memory_order_relaxed);
		return result;
	}

	// Finishes startup INFO updates and records their final counts.
	void FinishInfoStartup(Result& a_result, std::vector<std::string>& a_infoLogLines,
		const std::vector<RE::TESTopicInfo*>* a_infos)
	{
		const auto stats = UpdateStartupInfo(a_infoLogLines, a_infos);
		a_result.dialogueChoicesChanged = g_dialogueChoicesChanged.load(std::memory_order_relaxed);
		a_result.infoDirectChanges = stats.directChanged;
		a_result.infoFallbackChanges = stats.fallbackChanged;
		a_result.infoResponseChanges = stats.responseChanged;
	}

	// Installs early INFO source tracking before game data finishes loading.
	void InstallEarlyHooks()
	{
		InstallInfoHooks();
	}

	// Clears early dialogue source data when no rules need INFO text changes.
	void DisableInfoText()
	{
		g_infoTextPhase.store(InfoTextPhase::kDisabled, std::memory_order_release);
		std::scoped_lock lock(g_infoPromptMutex);
		g_directPromptCaptures.clear();
		g_directPromptProviders.clear();
		g_responseProviders.clear();
		g_originalInfoPrompts.clear();
		g_originalInfoResponses.clear();
		g_changedInfoIDs.clear();
		g_playerDialogueTopicIDs.clear();
		g_startupPendingInfos.clear();
	}
}
