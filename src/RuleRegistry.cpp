// Perk Conditions Framework
// SilentlyGayming
// RuleRegistry.cpp

#include "PCH.h"
#include "RuleRegistry.h"
#include "Config.h"
#include "CustomConditions.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <memory>
#include <system_error>
#include <unordered_map>
#include <unordered_set>

namespace
{
	std::unordered_map<RE::BGSPerk*, PCF::PerkRule> g_rules;
	std::unordered_map<RE::BGSPerk*, PCF::PerkRank> g_ranks;
	std::unordered_map<RE::BGSPerk*, std::uint32_t> g_linkedRankCounts;
	std::vector<RE::BGSPerk*> g_sources;

	struct RankKey
	{
		RE::BGSPerk* base;
		std::uint32_t rank;
		// Compares two fallback rank keys.
		bool operator==(const RankKey&) const = default;
	};

	struct RankKeyHash
	{
		// Builds a hash for a fallback rank key.
		std::size_t operator()(const RankKey& a_key) const noexcept
		{
			const auto pointer = std::hash<RE::BGSPerk*>{}(a_key.base);
			const auto rank = std::hash<std::uint32_t>{}(a_key.rank);
			return pointer ^ (rank + 0x9E3779B9u + (pointer << 6) + (pointer >> 2));
		}
	};

	std::unordered_map<RankKey, RE::BGSPerk*, RankKeyHash> g_fallbackRanks;
	std::unordered_map<RE::TESForm*, std::string> g_displayNames;
	std::unordered_map<RE::TESForm*, std::string> g_swfPaths;
	std::unordered_map<RE::BGSPerk*, std::vector<PCF::DescriptionRule>> g_descriptionRules;
	std::vector<RE::BGSPerk*> g_descriptionSources;
	std::unordered_map<const RE::TESDescription*, RE::BGSPerk*> g_descriptionOwners;
	std::unordered_map<const RE::TESDescription*, std::string> g_actorValueDescriptions;
	std::unordered_map<const RE::TESForm*, std::unordered_map<const RE::TESForm*, std::string>> g_requirementLabels;
	using FormCache = std::unordered_map<std::string, RE::TESForm*>;
	using Diagnostics = std::vector<std::string>;

	struct PendingCustomCondition
	{
		std::filesystem::path file;
		std::size_t line{ 0 };
		std::string targetPlugin;
		std::string targetReference;
		std::uint32_t configuredFormID{ 0 };
		std::vector<PCF::CustomConditions::Condition> conditions;
	};

	std::vector<PendingCustomCondition> g_pendingCustomConditions;
	bool g_customConditionsConfigured{ false };
	bool g_customConditionsFinalized{ true };
	bool g_customConditionSummaryLogged{ false };

	struct LookupInfo
	{
		const std::filesystem::path* file{ nullptr };
		std::size_t line{ 0 };
		std::string_view kind;
	};
	// Builds a plugin and form key without caring about letter case.
	std::string MakeReferenceKey(std::string_view a_plugin, std::string_view a_reference)
	{
		std::string key;
		key.reserve(a_plugin.size() + a_reference.size() + 1);
		for (const auto ch : a_plugin) {
			key.push_back(ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch + ('a' - 'A')) : ch);
		}
		key.push_back('\x1F');
		for (const auto ch : a_reference) {
			key.push_back(ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch + ('a' - 'A')) : ch);
		}
		return key;
	}
	// Reads a hexadecimal form ID.
	bool ReadFormID(std::string_view a_text, std::uint32_t& a_value)
	{
		if (a_text.starts_with("0x") || a_text.starts_with("0X")) {
			a_text.remove_prefix(2);
		} else if (a_text.size() != 8) {
			return false;
		}
		if (a_text.empty() || a_text.size() > 8) {
			return false;
		}
		for (const auto character : a_text) {
			if (!std::isxdigit(static_cast<unsigned char>(character))) {
				return false;
			}
		}
		const auto [end, error] = std::from_chars(a_text.data(), a_text.data() + a_text.size(), a_value, 16);
		return error == std::errc{} && end == a_text.data() + a_text.size() && a_value != 0;
	}
	// Converts a runtime FormID to its plugin-local FormID.
	std::uint32_t GetLocalFormID(const RE::TESFile* a_file, std::uint32_t a_formID)
	{
		return a_file && a_file->IsLight() ? a_formID & 0x00000FFF : a_formID & 0x00FFFFFF;
	}
	// Checks whether a form comes from the named plugin.
	bool SamePlugin(const RE::TESForm* a_form, const std::string& a_plugin)
	{
		if (!a_form) {
			return false;
		}
		const auto* files = a_form->sourceFiles.array;
		return files && !files->empty() && (*files)[0] &&
			_stricmp((*files)[0]->GetFilename().data(), a_plugin.c_str()) == 0;
	}
	// Builds a lookup table for configured EditorIDs by plugin.
	void BuildEditorIDIndex(RE::TESDataHandler* a_data, FormCache& a_cache, Diagnostics& a_diagnostics)
	{
		if (!a_data) {
			return;
		}

		auto index = [&a_cache, &a_diagnostics](RE::TESForm* a_form, const char* a_editorID = nullptr) {
			const auto* files = a_form ? a_form->sourceFiles.array : nullptr;
			const char* editorID = a_editorID ? a_editorID : (a_form ? a_form->GetFormEditorID() : nullptr);
			if (!files || files->empty() || !(*files)[0] || !editorID || !editorID[0]) {
				return;
			}
			const auto key = MakeReferenceKey((*files)[0]->GetFilename(), editorID);
			auto [entry, inserted] = a_cache.try_emplace(key, a_form);
			if (!inserted && entry->second != a_form) {
				entry->second = nullptr;
				a_diagnostics.push_back(fmt::format("Ambiguous EditorID {} in originating plugin {}; use a FormID",
					editorID, (*files)[0]->GetFilename()));
			}
		};

		for (auto* perk : a_data->GetFormArray<RE::BGSPerk>()) {
			index(perk);
		}
		for (auto* actorValue : a_data->GetFormArray<RE::ActorValueInfo>()) {
			index(actorValue);
		}
		for (auto* globalValue : a_data->GetFormArray<RE::TESGlobal>()) {
			index(globalValue, globalValue ? globalValue->formEditorID.c_str() : nullptr);
		}
	}
	// Finds a form from a plugin plus FormID or EditorID.
	RE::TESForm* FindReference(RE::TESDataHandler* a_data, const std::string& a_plugin,
		const std::string& a_reference, FormCache& a_cache, Diagnostics& a_diagnostics,
		const LookupInfo* a_context = nullptr)
	{
		const auto warn = [&a_diagnostics, a_context](std::string_view a_reason, const std::string& a_pluginName,
			const std::string& a_formReference) {
			if (a_context && a_context->file) {
				a_diagnostics.push_back(fmt::format("{}:{} - {} {}: {} | {}",
					a_context->file->filename().string(), a_context->line, a_reason, a_context->kind,
					a_pluginName, a_formReference));
			} else {
				a_diagnostics.push_back(fmt::format("{}: {} | {}", a_reason, a_pluginName, a_formReference));
			}
		};

		const auto key = MakeReferenceKey(a_plugin, a_reference);
		std::uint32_t formID = 0;
		if (ReadFormID(a_reference, formID)) {
			if (const auto cached = a_cache.find(key); cached != a_cache.end() && (cached->second || !a_context)) {
				return cached->second;
			}
			if (!a_data) {
				return nullptr;
			}
			const auto* file = a_data->LookupModByName(a_plugin);
			if (!file) {
				warn("Missing plugin for", a_plugin, a_reference);
				a_cache.emplace(key, nullptr);
				return nullptr;
			}
			if ((formID >> 24) != 0 && !file->IsFormInMod(formID)) {
				warn("Full FormID does not belong to named plugin for", a_plugin, a_reference);
				a_cache.emplace(key, nullptr);
				return nullptr;
			}
			auto* form = a_data->LookupForm(GetLocalFormID(file, formID), a_plugin);
			if (!SamePlugin(form, a_plugin)) {
				form = nullptr;
			}
			a_cache.emplace(key, form);
			if (!form) {
				warn("Missing plugin-local FormID for", a_plugin, a_reference);
			}
			return form;
		}

		const auto cached = a_cache.find(key);
		auto* form = cached != a_cache.end() ? cached->second : nullptr;
		if (!form || !SamePlugin(form, a_plugin)) {
			warn("Missing or ambiguous plugin-local EditorID for", a_plugin, a_reference);
			return nullptr;
		}
		return form;
	}
	// Resolves a [Conditions] target only when the normal loaded-form lookup can do so immediately.
	RE::TESForm* TryFindCustomConditionTarget(RE::TESDataHandler* a_data, const std::string& a_plugin,
		const std::string& a_reference, FormCache& a_cache, Diagnostics& a_attemptDiagnostics, const LookupInfo& a_context)
	{
		return FindReference(a_data, a_plugin, a_reference, a_cache, a_attemptDiagnostics, std::addressof(a_context));
	}

	// Emits the final custom-condition summary only after all deferred INFO targets have been resolved.
	void LogCustomConditionSummary()
	{
		if (g_customConditionSummaryLogged || !g_customConditionsConfigured) {
			return;
		}
		g_customConditionSummaryLogged = true;
		spdlog::info("Custom conditions: {} target(s), {} condition(s), {} crafting, {} dialogue, {} quest-stage",
			PCF::CustomConditions::TargetCount(), PCF::CustomConditions::ConditionCount(),
			PCF::CustomConditions::CraftingTargetCount(), PCF::CustomConditions::DialogueTargetCount(),
			PCF::CustomConditions::QuestConditionCount());
	}

	// Finds a configured form and checks that it has the expected type.
	template <class T>
	T* FindForm(RE::TESDataHandler* a_data, const std::string& a_plugin,
		const std::string& a_reference, FormCache& a_cache, Diagnostics& a_diagnostics,
		const LookupInfo* a_context = nullptr)
	{
		auto* form = FindReference(a_data, a_plugin, a_reference, a_cache, a_diagnostics, a_context);
		if (!form) {
			return nullptr;
		}
		auto* typed = form->As<T>();
		if (!typed) {
			if (a_context && a_context->file) {
				a_diagnostics.push_back(fmt::format("{}:{} - Incompatible form type; expected {}: {} | {}",
					a_context->file->filename().string(), a_context->line, a_context->kind, a_plugin, a_reference));
			} else {
				a_diagnostics.push_back(fmt::format("Incompatible form type: {} | {}", a_plugin, a_reference));
			}
		}
		return typed;
	}
	// Cleans up an interface artwork path.
	std::string CleanPath(std::string a_path)
	{
		std::replace(a_path.begin(), a_path.end(), '\\', '/');
		const auto startsWithInsensitive = [&a_path](std::string_view a_prefix) {
			return a_path.size() >= a_prefix.size() &&
				std::equal(a_prefix.begin(), a_prefix.end(), a_path.begin(), [](char a_left, char a_right) {
					return std::tolower(static_cast<unsigned char>(a_left)) ==
						std::tolower(static_cast<unsigned char>(a_right));
				});
		};
		if (startsWithInsensitive("Data/Interface/")) {
			a_path.erase(0, 15);
		} else if (startsWithInsensitive("Interface/")) {
			a_path.erase(0, 10);
		}
		if (a_path.size() >= 4) {
			const auto suffix = std::string_view(a_path).substr(a_path.size() - 4);
			if (std::equal(suffix.begin(), suffix.end(), ".swf", [](char a_left, char a_right) {
				return std::tolower(static_cast<unsigned char>(a_left)) ==
					std::tolower(static_cast<unsigned char>(a_right));
			})) {
				a_path.resize(a_path.size() - 4);
			}
		}
		return a_path;
	}
	// Checks whether two configured replacements are the same.
	bool SameAlternative(const PCF::PerkAlternative& a_left, const PCF::PerkAlternative& a_right)
	{
		if (a_left.type != a_right.type) {
			return false;
		}
		switch (a_left.type) {
		case PCF::AlternativeType::kPerk:
			return a_left.perk == a_right.perk && a_left.rank == a_right.rank;
		case PCF::AlternativeType::kActorValue:
			return a_left.actorValue == a_right.actorValue && a_left.comparison == a_right.comparison &&
				a_left.requiredValue == a_right.requiredValue;
		case PCF::AlternativeType::kGlobalValue:
			return a_left.globalValue == a_right.globalValue && a_left.comparison == a_right.comparison &&
				a_left.requiredValue == a_right.requiredValue;
		}
		return false;
	}
	// Gets the number of internal ranks on a perk.
	std::uint32_t CountInternalRanks(const RE::BGSPerk* a_perk)
	{
		if (!a_perk) {
			return 0;
		}
		const auto rawCount = static_cast<std::int32_t>(a_perk->data.numRanks);
		const auto count = rawCount > 0 ? static_cast<std::uint32_t>(rawCount) : 1u;
		return (std::min)(count, 255u);
	}
	// Checks whether a perk belongs to a linked perk family.
	bool IsLinkedRank(RE::BGSPerk* a_perk)
	{
		if (!a_perk) {
			return false;
		}
		const auto rank = g_ranks.find(a_perk);
		if (rank == g_ranks.end() || !rank->second.base) {
			return false;
		}
		const auto count = g_linkedRankCounts.find(rank->second.base);
		return count != g_linkedRankCounts.end() && count->second > 1;
	}
	// Builds rank information for internal and linked perk families.
	void BuildPerkRanks(RE::TESDataHandler* a_data, Diagnostics& a_diagnostics)
	{
		g_ranks.clear();
		g_linkedRankCounts.clear();
		if (!a_data) {
			return;
		}

		const auto& perks = a_data->GetFormArray<RE::BGSPerk>();
		std::unordered_map<RE::BGSPerk*, RE::BGSPerk*> parents;
		parents.reserve(perks.size());
		g_ranks.reserve(perks.size());
		g_linkedRankCounts.reserve(perks.size());

		for (auto* perk : perks) {
			if (!perk || !perk->nextPerk) {
				continue;
			}
			auto [entry, inserted] = parents.try_emplace(perk->nextPerk, perk);
			if (!inserted && entry->second != perk) {
				entry->second = nullptr;
			}
		}

		for (auto* root : perks) {
			if (!root || parents.contains(root)) {
				continue;
			}

			auto* perk = root;
			std::unordered_set<RE::BGSPerk*> visited;
			visited.reserve(8);
			std::uint32_t rank = 1;
			while (perk && rank <= 255) {
				if (!visited.emplace(perk).second) {
					a_diagnostics.push_back(fmt::format("Cyclic NNAM perk chain detected at {:08X}; chain indexing stopped", perk->GetFormID()));
					break;
				}
				const auto parent = parents.find(perk);
				if ((parent != parents.end() && !parent->second) || g_ranks.contains(perk)) {
					break;
				}
				g_ranks.emplace(perk, PCF::PerkRank{ root, rank });
				perk = perk->nextPerk;
				++rank;
			}
			g_linkedRankCounts[root] = rank - 1;
		}

		for (auto* member : perks) {
			if (!member || !member->nextPerk || g_ranks.contains(member)) {
				continue;
			}

			std::vector<RE::BGSPerk*> cycle;
			cycle.reserve(8);
			std::unordered_set<RE::BGSPerk*> visited;
			visited.reserve(8);
			auto* perk = member;
			while (perk && cycle.size() < 255 && !g_ranks.contains(perk) && visited.emplace(perk).second) {
				cycle.push_back(perk);
				perk = perk->nextPerk;
			}
			if (perk != member || cycle.size() < 2) {
				continue;
			}
			if (std::ranges::any_of(cycle, [&parents](RE::BGSPerk* a_perk) {
				const auto parent = parents.find(a_perk);
				return parent == parents.end() || !parent->second;
			})) {
				continue;
			}

			auto* root = *std::ranges::min_element(cycle, {}, [](const RE::BGSPerk* a_perk) {
				return std::pair{ static_cast<std::int32_t>(a_perk->data.level), a_perk->GetFormID() };
			});
			perk = root;
			std::uint32_t rank = 1;
			do {
				g_ranks.emplace(perk, PCF::PerkRank{ root, rank });
				perk = perk->nextPerk;
				++rank;
			} while (perk && perk != root && rank <= cycle.size());
			g_linkedRankCounts[root] = static_cast<std::uint32_t>(cycle.size());
		}
	}
	// Reads a one-based rank number from a perk EditorID.
	std::uint32_t GetEditorRank(const RE::BGSPerk* a_perk)
	{
		if (!a_perk) {
			return 0;
		}
		const char* editorID = a_perk->GetFormEditorID();
		if (!editorID) {
			return 0;
		}
		const std::string_view text(editorID);
		std::size_t first = text.size();
		while (first > 0 && std::isdigit(static_cast<unsigned char>(text[first - 1]))) {
			--first;
		}
		if (first == text.size()) {
			return 0;
		}
		std::uint32_t rank = 0;
		const auto [end, error] = std::from_chars(text.data() + first, text.data() + text.size(), rank);
		return error == std::errc{} && end == text.data() + text.size() && rank <= 255 ? rank : 0;
	}
	// Checks whether two perks share the same EditorID family name.
	bool SameEditorName(const RE::BGSPerk* a_left, const RE::BGSPerk* a_right)
	{
		if (!a_left || !a_right) {
			return false;
		}
		const auto* leftFiles = a_left->sourceFiles.array;
		const auto* rightFiles = a_right->sourceFiles.array;
		if (!leftFiles || !rightFiles || leftFiles->empty() || rightFiles->empty() || (*leftFiles)[0] != (*rightFiles)[0]) {
			return false;
		}
		const char* leftID = a_left->GetFormEditorID();
		const char* rightID = a_right->GetFormEditorID();
		if (!leftID || !rightID) {
			return false;
		}
		std::string_view left(leftID);
		std::string_view right(rightID);
		while (!left.empty() && std::isdigit(static_cast<unsigned char>(left.back()))) {
			left.remove_suffix(1);
		}
		while (!right.empty() && std::isdigit(static_cast<unsigned char>(right.back()))) {
			right.remove_suffix(1);
		}
		if (left.empty() || left.size() != right.size()) {
			return false;
		}
		return std::ranges::equal(left, right, [](char a_leftChar, char a_rightChar) {
			const auto leftChar = a_leftChar >= 'A' && a_leftChar <= 'Z' ? static_cast<char>(a_leftChar + ('a' - 'A')) : a_leftChar;
			const auto rightChar = a_rightChar >= 'A' && a_rightChar <= 'Z' ? static_cast<char>(a_rightChar + ('a' - 'A')) : a_rightChar;
			return leftChar == rightChar;
		});
	}

}

namespace PCF::RuleRegistry
{
	// Tests whether the rule registry is empty.
	bool Empty()
	{
		return g_rules.empty();
	}
	// Tests whether explicit artwork assignments are loaded.
	bool HasSWFAssignments()
	{
		return !g_swfPaths.empty();
	}
	// Tests whether any description replacement is loaded.
	bool HasDescriptionRules()
	{
		return !g_descriptionRules.empty() || !g_actorValueDescriptions.empty();
	}
	// Tests whether conditional perk-description rules are loaded.
	bool HasPerkDescriptionRules()
	{
		return !g_descriptionRules.empty();
	}
	// Returns the registered source perks.
	const std::vector<RE::BGSPerk*>& Sources()
	{
		return g_sources;
	}
	// Finds the rule registered for a source perk.
	const PerkRule* Find(RE::BGSPerk* a_perk)
	{
		const auto entry = g_rules.find(a_perk);
		return entry != g_rules.end() ? std::addressof(entry->second) : nullptr;
	}
	// Finds explicit artwork registered for a form.
	const std::string* FindSWF(RE::TESForm* a_form)
	{
		if (!a_form) {
			return nullptr;
		}
		const auto entry = g_swfPaths.find(a_form);
		return entry != g_swfPaths.end() ? std::addressof(entry->second) : nullptr;
	}
	// Finds an explicit player-facing display name registered for a form.
	const std::string* GetDisplayName(RE::TESForm* a_form)
	{
		if (!a_form) {
			return nullptr;
		}
		const auto entry = g_displayNames.find(a_form);
		return entry != g_displayNames.end() ? std::addressof(entry->second) : nullptr;
	}
	// Finds conditional description rules registered for a perk.
	const std::vector<DescriptionRule>* GetDescriptions(RE::BGSPerk* a_perk)
	{
		const auto entry = g_descriptionRules.find(a_perk);
		return entry != g_descriptionRules.end() ? std::addressof(entry->second) : nullptr;
	}
	// Returns perks with conditional description rules.
	const std::vector<RE::BGSPerk*>& GetDescriptionSources()
	{
		return g_descriptionSources;
	}
	// Finds the configured perk that owns a TESDescription.
	RE::BGSPerk* FindDescriptionPerk(const RE::TESDescription* a_description)
	{
		const auto entry = g_descriptionOwners.find(a_description);
		return entry != g_descriptionOwners.end() ? entry->second : nullptr;
	}
	// Finds a direct Actor Value description replacement through its shared description component.
	const std::string* FindActorValueDescription(const RE::TESDescription* a_description)
	{
		if (!a_description || g_actorValueDescriptions.empty()) {
			return nullptr;
		}
		const auto entry = g_actorValueDescriptions.find(a_description);
		return entry != g_actorValueDescriptions.end() ? std::addressof(entry->second) : nullptr;
	}
	// Tests whether any target-specific requirement labels are loaded.
	bool HasRequirementLabels()
	{
		return !g_requirementLabels.empty();
	}
	// Tests whether a target has any requirement labels.
	bool HasRequirementLabels(const RE::TESForm* a_target)
	{
		return a_target && g_requirementLabels.contains(a_target);
	}
	// Finds the label for one displayed condition form on one target.
	const std::string* FindRequirementLabel(const RE::TESForm* a_target, const RE::TESForm* a_conditionForm)
	{
		if (!a_target || !a_conditionForm || g_requirementLabels.empty()) {
			return nullptr;
		}
		const auto target = g_requirementLabels.find(a_target);
		if (target == g_requirementLabels.end()) {
			return nullptr;
		}
		const auto label = target->second.find(a_conditionForm);
		return label != target->second.end() ? std::addressof(label->second) : nullptr;
	}

	// Chooses the custom replacement shown for a rule.
	const PerkAlternative* GetDisplay(const PerkRule& a_rule)
	{
		return a_rule.display == DisplayMode::kCustom && !a_rule.alternatives.empty() ?
			std::addressof(a_rule.alternatives.front()) : nullptr;
	}

	// Finds a requested rank across internal and linked perk forms.
	PerkRankResult FindPerkRank(RE::BGSPerk* a_base, std::uint32_t a_rank)
	{
		if (!a_base || a_rank == 0 || a_rank > 255) {
			return {};
		}

		const auto start = GetRank(a_base);
		if (!IsLinkedRank(a_base)) {
			const auto internalRanks = CountInternalRanks(a_base);
			if (a_rank <= internalRanks) {
				return { a_base, a_rank, PerkRankType::kInternal };
			}
		} else if (start.base) {
			auto* candidate = start.base;
			for (std::uint32_t rank = 1; rank < a_rank && candidate; ++rank) {
				candidate = candidate->nextPerk;
			}
			if (candidate) {
				const auto candidateRank = GetRank(candidate);
				if (candidateRank.base == start.base && candidateRank.rank == a_rank) {
					return { candidate, a_rank, PerkRankType::kLinked };
				}
			}
		}

		const RankKey key{ a_base, a_rank };
		const auto fallback = g_fallbackRanks.find(key);
		if (fallback != g_fallbackRanks.end() && fallback->second) {
			return { fallback->second, a_rank, PerkRankType::kFallback };
		}
		return {};
	}
	// Gets the saved family rank for a perk.
	PerkRank GetRank(RE::BGSPerk* a_perk)
	{
		const auto entry = g_ranks.find(a_perk);
		return entry != g_ranks.end() ? entry->second : PerkRank{};
	}
	// Builds the runtime lookup tables for rules, descriptions, names, and artwork.
	void Load()
	{
		g_rules.clear();
		g_sources.clear();
		g_fallbackRanks.clear();
		g_linkedRankCounts.clear();
		g_displayNames.clear();
		g_swfPaths.clear();
		g_descriptionRules.clear();
		g_descriptionSources.clear();
		g_descriptionOwners.clear();
		g_actorValueDescriptions.clear();
		g_requirementLabels.clear();
		g_pendingCustomConditions.clear();
		g_customConditionsConfigured = false;
		g_customConditionsFinalized = true;
		g_customConditionSummaryLogged = false;
		PCF::CustomConditions::Clear();

		const auto config = Config::Load();
		g_customConditionsConfigured = !config.customConditions.empty();
		Diagnostics diagnostics;
		diagnostics.reserve(config.rules.size() + config.customConditions.size() + config.descriptionRules.size() + config.actorValueDescriptions.size() + config.requirementLabels.size() + config.nameAssignments.size() + config.swfAssignments.size());
		auto* data = RE::TESDataHandler::GetSingleton();
		if (!data) {
			throw std::runtime_error("TESDataHandler unavailable while building registry");
		}

		if (!config.rules.empty() || !config.customConditions.empty() || !config.descriptionRules.empty() ||
			!config.requirementLabels.empty() || !config.swfAssignments.empty()) {
			BuildPerkRanks(data, diagnostics);
		} else {
			g_ranks.clear();
			g_linkedRankCounts.clear();
		}
		g_rules.reserve(config.rules.size());
		FormCache cache;
		cache.reserve((config.rules.size() + config.customConditions.size() * 2 + config.descriptionRules.size() + config.actorValueDescriptions.size() + config.requirementLabels.size() * 2 + config.nameAssignments.size() + config.swfAssignments.size()) * 2);
		BuildEditorIDIndex(data, cache, diagnostics);

		std::vector<RE::BGSPerk*> resolvedSources(config.rules.size(), nullptr);
		for (std::size_t i = 0; i < config.rules.size(); ++i) {
			const auto& raw = config.rules[i];
			resolvedSources[i] = FindForm<RE::BGSPerk>(data, raw.sourcePlugin, raw.sourceReference, cache, diagnostics);
		}

		std::unordered_map<RE::BGSPerk*, std::pair<RuleMode, DisplayMode>> sourcePolicies;
		std::unordered_set<RE::BGSPerk*> conflictingSources;
		for (std::size_t i = 0; i < config.rules.size(); ++i) {
			auto* source = resolvedSources[i];
			if (!source) {
				continue;
			}
			const auto& raw = config.rules[i];
			const auto policy = std::pair{ raw.mode, raw.display };
			const auto [entry, inserted] = sourcePolicies.try_emplace(source, policy);
			if (!inserted && entry->second != policy && conflictingSources.insert(source).second) {
				diagnostics.push_back(fmt::format("{}:{} - Conflicting mode/display policy for source {:08X}; rejecting that source",
					raw.file.filename().string(), raw.line, source->GetFormID()));
			}
		}

		std::unordered_set<std::string> unresolvedNames;
		std::unordered_set<RE::TESForm*> conflictingNames;
		for (const auto& raw : config.nameAssignments) {
			const auto key = MakeReferenceKey(raw.plugin, raw.formReference);
			if (unresolvedNames.contains(key)) {
				continue;
			}
			const LookupInfo context{ std::addressof(raw.file), raw.line, "Name" };
			auto* form = FindReference(data, raw.plugin, raw.formReference, cache, diagnostics, std::addressof(context));
			if (!form) {
				unresolvedNames.insert(key);
				continue;
			}
			const auto [entry, inserted] = g_displayNames.try_emplace(form, raw.name);
			if (!inserted && entry->second != raw.name && conflictingNames.insert(form).second) {
				diagnostics.push_back(fmt::format("{}:{} - Conflicting display names for {} | {}; ignoring that name assignment",
					raw.file.filename().string(), raw.line, raw.plugin, raw.formReference));
			}
		}
		for (auto* form : conflictingNames) {
			g_displayNames.erase(form);
		}

		g_requirementLabels.reserve(config.requirementLabels.size());
		std::size_t requirementLabelsLoaded = 0;
		std::size_t requirementLabelDuplicates = 0;
		for (const auto& raw : config.requirementLabels) {
			const auto resolveLocal = [&](const std::string& a_plugin, const std::string& a_reference, std::string_view a_kind) -> RE::TESForm* {
				std::uint32_t localFormID = 0;
				if (!ReadFormID(a_reference, localFormID) || (localFormID >> 24) != 0) {
					diagnostics.push_back(fmt::format("{}:{} - RequirementLabels {} requires a plugin-local FormID: {} | {}",
						raw.file.filename().string(), raw.line, a_kind, a_plugin, a_reference));
					return nullptr;
				}
				if (const auto* file = data->LookupModByName(a_plugin); file && file->IsLight() && localFormID > 0x00000FFF) {
					diagnostics.push_back(fmt::format("{}:{} - RequirementLabels {} FormID exceeds the local range of light plugin {}: {}",
						raw.file.filename().string(), raw.line, a_kind, a_plugin, a_reference));
					return nullptr;
				}
				const LookupInfo context{ std::addressof(raw.file), raw.line, a_kind };
				return FindReference(data, a_plugin, a_reference, cache, diagnostics, std::addressof(context));
			};

			auto* target = resolveLocal(raw.targetPlugin, raw.targetReference, "target");
			auto* conditionForm = resolveLocal(raw.conditionPlugin, raw.conditionReference, "condition form");
			if (!target || !conditionForm) {
				continue;
			}

			auto& labels = g_requirementLabels[target];
			auto [entry, inserted] = labels.try_emplace(conditionForm, raw.label);
			if (!inserted) {
				++requirementLabelDuplicates;
				if (entry->second != raw.label) {
					diagnostics.push_back(fmt::format("{}:{} - Conflicting requirement label for {} | {} and {} | {}; keeping the first assignment",
						raw.file.filename().string(), raw.line, raw.targetPlugin, raw.targetReference, raw.conditionPlugin, raw.conditionReference));
				}
				continue;
			}
			++requirementLabelsLoaded;
			spdlog::debug("RequirementLabels: parsed source={}|{} -> {:08X}, requirement={}|{} -> {:08X}, label=\"{}\"",
				raw.targetPlugin, raw.targetReference, target->GetFormID(), raw.conditionPlugin, raw.conditionReference,
				conditionForm->GetFormID(), raw.label);
		}

		g_actorValueDescriptions.reserve(config.actorValueDescriptions.size());
		std::size_t actorValueDescriptionsLoaded = 0;
		std::size_t actorValueDescriptionDuplicates = 0;
		for (const auto& raw : config.actorValueDescriptions) {
			std::uint32_t localFormID = 0;
			if (!ReadFormID(raw.formReference, localFormID) || (localFormID >> 24) != 0) {
				diagnostics.push_back(fmt::format("{}:{} - Actor Value descriptions require a plugin-local FormID: {} | {}",
					raw.file.filename().string(), raw.line, raw.plugin, raw.formReference));
				continue;
			}
			if (const auto* file = data->LookupModByName(raw.plugin); file && file->IsLight() && localFormID > 0x00000FFF) {
				diagnostics.push_back(fmt::format("{}:{} - Actor Value description FormID exceeds the local range of light plugin {}: {}",
					raw.file.filename().string(), raw.line, raw.plugin, raw.formReference));
				continue;
			}

			const LookupInfo context{ std::addressof(raw.file), raw.line, "Actor Value" };
			auto* actorValue = FindForm<RE::ActorValueInfo>(data, raw.plugin, raw.formReference, cache, diagnostics, std::addressof(context));
			if (!actorValue) {
				continue;
			}

			auto* description = static_cast<RE::TESDescription*>(actorValue);
			auto [entry, inserted] = g_actorValueDescriptions.try_emplace(description, raw.description);
			if (!inserted) {
				++actorValueDescriptionDuplicates;
				if (entry->second != raw.description) {
					diagnostics.push_back(fmt::format("{}:{} - Conflicting Actor Value description for {} | {}; keeping the first assignment",
						raw.file.filename().string(), raw.line, raw.plugin, raw.formReference));
				}
				continue;
			}
			++actorValueDescriptionsLoaded;
		}

		std::size_t descriptionsLoaded = 0;
		std::size_t descriptionDuplicates = 0;
		for (const auto& raw : config.descriptionRules) {
			const LookupInfo sourceContext{ std::addressof(raw.file), raw.line, "Description source perk" };
			auto* source = FindForm<RE::BGSPerk>(data, raw.sourcePlugin, raw.sourceReference, cache, diagnostics, std::addressof(sourceContext));
			if (!source) {
				continue;
			}
			const LookupInfo targetContext{ std::addressof(raw.file), raw.line, "Description condition" };
			auto* target = FindReference(data, raw.conditionPlugin, raw.conditionReference, cache, diagnostics, std::addressof(targetContext));
			if (!target) {
				continue;
			}

			DescriptionRule rule;
			rule.comparison = raw.comparison;
			rule.requiredValue = raw.requiredValue;
			rule.description = raw.description;
			if ((rule.perk = target->As<RE::BGSPerk>())) {
				rule.type = AlternativeType::kPerk;
			} else if ((rule.actorValue = target->As<RE::ActorValueInfo>())) {
				rule.type = AlternativeType::kActorValue;
			} else if ((rule.globalValue = target->As<RE::TESGlobal>())) {
				rule.type = AlternativeType::kGlobalValue;
			} else {
				diagnostics.push_back(fmt::format("{}:{} - Description condition must reference a Perk, ActorValue, or GlobalValue: {} | {}",
					raw.file.filename().string(), raw.line, raw.conditionPlugin, raw.conditionReference));
				continue;
			}

			auto& rules = g_descriptionRules[source];
			const auto duplicate = std::ranges::find_if(rules, [&rule](const DescriptionRule& a_existing) {
				if (a_existing.type != rule.type || a_existing.comparison != rule.comparison || a_existing.requiredValue != rule.requiredValue) {
					return false;
				}
				switch (rule.type) {
				case AlternativeType::kPerk:
					return a_existing.perk == rule.perk;
				case AlternativeType::kActorValue:
					return a_existing.actorValue == rule.actorValue;
				case AlternativeType::kGlobalValue:
					return a_existing.globalValue == rule.globalValue;
				}
				return false;
			});
			if (duplicate != rules.end()) {
				++descriptionDuplicates;
				if (duplicate->description != rule.description) {
					diagnostics.push_back(fmt::format("{}:{} - Conflicting description for an identical condition on {} | {}; keeping the first description",
						raw.file.filename().string(), raw.line, raw.sourcePlugin, raw.sourceReference));
				}
				continue;
			}
			rules.push_back(std::move(rule));
			if (rules.size() == 1) {
				g_descriptionSources.push_back(source);
			}
			g_descriptionOwners.try_emplace(static_cast<RE::TESDescription*>(source), source);
			++descriptionsLoaded;
		}

		for (const auto& raw : config.swfAssignments) {
			auto* form = FindReference(data, raw.plugin, raw.formReference, cache, diagnostics);
			if (!form) {
				continue;
			}
			const auto path = CleanPath(raw.path);
			if (path.empty()) {
				diagnostics.push_back(fmt::format("{}:{} - Empty SWF path for {} | {}", raw.file.filename().string(), raw.line,
					raw.plugin, raw.formReference));
				continue;
			}
			auto [entry, inserted] = g_swfPaths.try_emplace(form, path);
			if (!inserted && entry->second != path) {
				diagnostics.push_back(fmt::format("{}:{} - Conflicting SWF paths for {} | {}; keeping the first path",
					raw.file.filename().string(), raw.line, raw.plugin, raw.formReference));
			}
		}

		struct CustomTargetGroup
		{
			std::vector<const RawCustomCondition*> rows;
			RE::TESForm* resolvedTarget{ nullptr };
			std::uint32_t configuredFormID{ 0 };
			bool ambiguous{ false };
		};
		std::vector<CustomTargetGroup> customTargetGroups;
		std::unordered_map<std::string, std::size_t> customTargetIndexes;
		customTargetGroups.reserve(config.customConditions.size());
		customTargetIndexes.reserve(config.customConditions.size());
		for (const auto& raw : config.customConditions) {
			const LookupInfo targetContext{ std::addressof(raw.file), raw.line, "Custom condition target" };
			Diagnostics targetAttemptDiagnostics;
			auto* target = TryFindCustomConditionTarget(data, raw.targetPlugin, raw.targetReference, cache,
				targetAttemptDiagnostics, targetContext);
			if (target && !target->As<RE::BGSConstructibleObject>() && !target->As<RE::TESTopicInfo>()) {
				diagnostics.push_back(fmt::format("{}:{} - [Conditions] target must be a COBJ or INFO: {} | {}",
					raw.file.filename().string(), raw.line, raw.targetPlugin, raw.targetReference));
				continue;
			}

			std::uint32_t configuredFormID = target ? target->GetFormID() : 0;
			const auto* file = data ? data->LookupModByName(raw.targetPlugin) : nullptr;
			if (!configuredFormID && !ReadFormID(raw.targetReference, configuredFormID)) {
				diagnostics.insert(diagnostics.end(), targetAttemptDiagnostics.begin(), targetAttemptDiagnostics.end());
				diagnostics.push_back(fmt::format("{}:{} - Rejected entire [Conditions] target because it could not be resolved: {} | {}",
					raw.file.filename().string(), raw.line, raw.targetPlugin, raw.targetReference));
				continue;
			}
			if (file && ((configuredFormID >> 24) == 0 || file->IsFormInMod(configuredFormID))) {
				configuredFormID = GetLocalFormID(file, configuredFormID);
			}

			const auto key = MakeReferenceKey(raw.targetPlugin, fmt::format("{:08X}", configuredFormID));
			auto [entry, inserted] = customTargetIndexes.try_emplace(key, customTargetGroups.size());
			if (inserted) {
				customTargetGroups.push_back({ {}, target, configuredFormID, false });
			} else if (target) {
				auto& group = customTargetGroups[entry->second];
				if (group.resolvedTarget && group.resolvedTarget != target) {
					diagnostics.push_back(fmt::format("{}:{} - Rejected ambiguous [Conditions] target identity: {} | {}",
						raw.file.filename().string(), raw.line, raw.targetPlugin, raw.targetReference));
					group.ambiguous = true;
				} else if (!group.resolvedTarget) {
					group.resolvedTarget = target;
				}
			}
			customTargetGroups[entry->second].rows.push_back(std::addressof(raw));
		}

		const auto resolveCustomCondition = [&](const RawCustomCondition& raw, PCF::CustomConditions::Condition& resolved) {
			resolved.orWithNext = raw.orWithNext;
			if (!std::isfinite(raw.requiredValue) || raw.requiredValue < 0.0F || raw.requiredValue > 65535.0F ||
				std::trunc(raw.requiredValue) != raw.requiredValue) {
				if (raw.type != CustomConditionType::kFormValue) {
					diagnostics.push_back(fmt::format("{}:{} - Quest stage must be an integer from 0 to 65535: {}",
					raw.file.filename().string(), raw.line, raw.requiredValue));
					return false;
				}
			}

			const LookupInfo valueContext{ std::addressof(raw.file), raw.line,
				raw.type == CustomConditionType::kFormValue ? "Custom condition value" : "Custom condition quest" };
			auto* form = FindReference(data, raw.conditionPlugin, raw.conditionReference, cache, diagnostics, std::addressof(valueContext));
			if (!form) {
				return false;
			}

			if (raw.type == CustomConditionType::kQuestStage || raw.type == CustomConditionType::kQuestStageDone) {
				auto* quest = form->As<RE::TESQuest>();
				if (!quest) {
					diagnostics.push_back(fmt::format("{}:{} - [Conditions] expected TESQuest but resolved a different form type: {} | {}",
						raw.file.filename().string(), raw.line, raw.conditionPlugin, raw.conditionReference));
					return false;
				}
				resolved.kind = raw.type == CustomConditionType::kQuestStage ?
					PCF::CustomConditions::ConditionKind::kQuestStage : PCF::CustomConditions::ConditionKind::kQuestStageDone;
				resolved.quest = quest;
				resolved.questStage = static_cast<std::uint16_t>(raw.requiredValue);
				resolved.value.comparison = raw.comparison;
				resolved.value.requiredValue = raw.requiredValue;
				resolved.value.name = RE::TESFullName::GetFullName(*form, false);
				if (resolved.value.name.empty()) {
					resolved.value.name = fmt::format("{}|{}", raw.conditionPlugin, raw.conditionReference);
				}
				return true;
			}

			auto& condition = resolved.value;
			resolved.kind = PCF::CustomConditions::ConditionKind::kFormValue;
			condition.comparison = raw.comparison;
			condition.requiredValue = raw.requiredValue;
			if ((condition.perk = form->As<RE::BGSPerk>())) {
				condition.type = AlternativeType::kPerk;
				if (!std::isfinite(raw.requiredValue) || raw.requiredValue < 0.0F || raw.requiredValue > 255.0F ||
					std::trunc(raw.requiredValue) != raw.requiredValue) {
					diagnostics.push_back(fmt::format("{}:{} - Perk conditions require an integer rank from 0 to 255: {}",
						raw.file.filename().string(), raw.line, raw.requiredValue));
					return false;
				}
				condition.rank = static_cast<std::uint32_t>(raw.requiredValue);
				if (condition.rank > 0) {
					condition.foundRank = FindPerkRank(condition.perk, condition.rank);
				}
			} else if ((condition.actorValue = form->As<RE::ActorValueInfo>())) {
				condition.type = AlternativeType::kActorValue;
			} else if ((condition.globalValue = form->As<RE::TESGlobal>())) {
				condition.type = AlternativeType::kGlobalValue;
			} else {
				diagnostics.push_back(fmt::format("{}:{} - [Conditions] value must reference a Perk, ActorValue, GlobalValue, or use an explicit quest condition type: {} | {}",
					raw.file.filename().string(), raw.line, raw.conditionPlugin, raw.conditionReference));
				return false;
			}

			if (const auto swf = g_swfPaths.find(form); swf != g_swfPaths.end()) {
				condition.iconPath = swf->second;
			} else if (condition.type == AlternativeType::kPerk) {
				condition.iconPath = CleanPath(condition.perk->swfFile.c_str());
			} else if (condition.type == AlternativeType::kActorValue) {
				const char* editorID = condition.actorValue->GetFormEditorID();
				if (editorID && editorID[0]) {
					condition.iconPath = fmt::format("Components/VaultBoys/Skills/{}", editorID);
				}
			}
			condition.iconPath = CleanPath(std::move(condition.iconPath));

			if (condition.type == AlternativeType::kGlobalValue) {
				const char* editorID = condition.globalValue->formEditorID.c_str();
				condition.name = editorID && editorID[0] ? editorID : fmt::format("{}|{}", raw.conditionPlugin, raw.conditionReference);
			} else {
				condition.name = RE::TESFullName::GetFullName(*form, false);
				if (condition.name.empty()) {
					condition.name = raw.conditionReference;
				}
			}
			return true;
		};

		g_pendingCustomConditions.reserve(customTargetGroups.size());
		for (const auto& group : customTargetGroups) {
			if (group.rows.empty()) {
				continue;
			}
			const auto& first = *group.rows.front();
			if (group.ambiguous) {
				diagnostics.push_back(fmt::format("{}:{} - Rejected entire [Conditions] target because its identity was ambiguous: {} | {}",
					first.file.filename().string(), first.line, first.targetPlugin, first.targetReference));
				continue;
			}

			std::vector<PCF::CustomConditions::Condition> conditions;
			conditions.reserve(group.rows.size());
			bool targetValid = true;
			for (const auto* raw : group.rows) {
				PCF::CustomConditions::Condition condition;
				if (!raw || !resolveCustomCondition(*raw, condition)) {
					targetValid = false;
					continue;
				}
				conditions.push_back(std::move(condition));
			}
			if (!targetValid || conditions.size() != group.rows.size()) {
				diagnostics.push_back(fmt::format("{}:{} - Rejected entire [Conditions] target because one or more configured rows are invalid: {} | {}",
					first.file.filename().string(), first.line, first.targetPlugin, first.targetReference));
				continue;
			}

			if (!conditions.empty() && conditions.back().orWithNext) {
				diagnostics.push_back(fmt::format("{}:{} - Rejected entire [Conditions] target because the final row cannot end with OR: {} | {}",
					first.file.filename().string(), first.line, first.targetPlugin, first.targetReference));
				continue;
			}

			if (group.resolvedTarget && group.resolvedTarget->As<RE::BGSConstructibleObject>()) {
				if (!PCF::CustomConditions::Add(group.resolvedTarget, std::move(conditions))) {
					diagnostics.push_back(fmt::format("{}:{} - Failed to register complete [Conditions] target {} | {}",
						first.file.filename().string(), first.line, first.targetPlugin, first.targetReference));
				}
				continue;
			}

			g_pendingCustomConditions.push_back({
				first.file,
				first.line,
				first.targetPlugin,
				first.targetReference,
				group.configuredFormID,
				std::move(conditions)
			});
		}

		g_customConditionsFinalized = g_pendingCustomConditions.empty();

		std::size_t resolved = 0;
		std::size_t duplicates = 0;
		std::size_t perkToPerkCount = 0;
		std::size_t perkToActorValueCount = 0;
		std::size_t perkToGlobalValueCount = 0;
		for (std::size_t i = 0; i < config.rules.size(); ++i) {
			const auto& raw = config.rules[i];
			auto* source = resolvedSources[i];
			if (!source) {
				continue;
			}
			if (conflictingSources.contains(source)) {
				continue;
			}

			PerkAlternative alternative;
			alternative.type = raw.type;
			alternative.comparison = raw.comparison;
			alternative.rank = raw.rank;
			alternative.requiredValue = raw.requiredValue;
			RE::TESForm* form = nullptr;
			switch (raw.type) {
			case AlternativeType::kPerk:
				alternative.perk = FindForm<RE::BGSPerk>(data, raw.alternativePlugin, raw.alternativeReference, cache, diagnostics);
				form = alternative.perk;
				break;
			case AlternativeType::kActorValue:
				alternative.actorValue = FindForm<RE::ActorValueInfo>(data, raw.alternativePlugin, raw.alternativeReference, cache, diagnostics);
				form = alternative.actorValue;
				break;
			case AlternativeType::kGlobalValue: {
				const LookupInfo context{ std::addressof(raw.file), raw.line, "GlobalValue" };
				alternative.globalValue = FindForm<RE::TESGlobal>(data, raw.alternativePlugin, raw.alternativeReference, cache, diagnostics, std::addressof(context));
				form = alternative.globalValue;
				break;
			}
			}
			if (!form) {
				continue;
			}

			if (alternative.perk) {
				alternative.foundRank = FindPerkRank(alternative.perk, alternative.rank);
				if (!alternative.foundRank) {
					const auto internalRanks = CountInternalRanks(alternative.perk);
					const char* editorID = alternative.perk->GetFormEditorID();
					if (!IsLinkedRank(alternative.perk)) {
						diagnostics.push_back(fmt::format("{}:{} - {} [{:08X}] has {} internal rank(s); requested rank {} is invalid; actor rank fallback will be used",
							raw.file.filename().string(), raw.line, editorID ? editorID : "PERK", alternative.perk->GetFormID(),
							internalRanks, alternative.rank));
					} else {
						diagnostics.push_back(fmt::format("{}:{} - {} [{:08X}] linked perk family does not provide rank {}; actor rank fallback will be used",
							raw.file.filename().string(), raw.line, editorID ? editorID : "PERK", alternative.perk->GetFormID(),
							alternative.rank));
					}
				}
			}

			if (const auto swf = g_swfPaths.find(form); swf != g_swfPaths.end()) {
				alternative.iconPath = swf->second;
			} else if (alternative.iconPath.empty()) {
				switch (alternative.type) {
				case AlternativeType::kPerk:
					alternative.iconPath = alternative.perk->swfFile.c_str();
					break;
				case AlternativeType::kActorValue: {
					const char* editorID = alternative.actorValue->GetFormEditorID();
					if (editorID && editorID[0]) {
						alternative.iconPath = fmt::format("Components/VaultBoys/Skills/{}.swf", editorID);
					}
					break;
				}
				case AlternativeType::kGlobalValue:
					break;
				}
			}
			alternative.iconPath = CleanPath(std::move(alternative.iconPath));
			if (alternative.type == AlternativeType::kGlobalValue) {
				const char* editorID = alternative.globalValue->formEditorID.c_str();
				alternative.name = editorID && editorID[0] ? editorID :
					fmt::format("{}|{}", raw.alternativePlugin, raw.alternativeReference);
			} else {
				alternative.name = RE::TESFullName::GetFullName(*form, false);
				if (alternative.name.empty()) {
					alternative.name = raw.alternativeReference;
				}
			}

			auto& rule = g_rules[source];
			if (rule.alternatives.empty()) {
				rule.mode = raw.mode;
				rule.display = raw.display;
			}

			const auto duplicate = std::ranges::find_if(rule.alternatives, [&alternative](const PerkAlternative& a_existing) {
				return SameAlternative(a_existing, alternative);
			});
			if (duplicate == rule.alternatives.end()) {
				rule.alternatives.push_back(std::move(alternative));
				++resolved;
				switch (raw.type) {
				case AlternativeType::kPerk:
					++perkToPerkCount;
					break;
				case AlternativeType::kActorValue:
					++perkToActorValueCount;
					break;
				case AlternativeType::kGlobalValue:
					++perkToGlobalValueCount;
					break;
				}
			} else {
				++duplicates;
				if (duplicate->iconPath != alternative.iconPath) {
					diagnostics.push_back(fmt::format("{}:{} - Duplicate alternative has conflicting artwork; keeping the first path",
						raw.file.filename().string(), raw.line));
				}
			}
		}

		g_sources.reserve(g_rules.size());
		for (auto& [source, rule] : g_rules) {
			if (!rule.alternatives.empty()) {
				g_sources.push_back(source);
			}
		}

		std::ranges::sort(g_sources, {}, [](const RE::BGSPerk* a_perk) { return a_perk->GetFormID(); });

		if (!g_rules.empty()) {
			for (auto* base : g_sources) {
				for (auto* candidate : g_sources) {
					const auto rank = GetEditorRank(candidate);
					if (rank && SameEditorName(base, candidate)) {
						const RankKey key{ base, rank };
						auto [entry, inserted] = g_fallbackRanks.try_emplace(key, candidate);
						if (!inserted && entry->second != candidate) {
							entry->second = nullptr;
						}
					}
				}
			}
		} else if (g_swfPaths.empty() && g_descriptionRules.empty() && g_actorValueDescriptions.empty() && PCF::CustomConditions::Empty() &&
			g_pendingCustomConditions.empty()) {
			g_ranks.clear();
			g_linkedRankCounts.clear();
		}

		// Finalize player-facing Custom Conditions only after the perk replacement
		// registry is complete. Custom condition rows that reference source perks
		// can then inherit the same configured replacement text/artwork semantics.
		if (g_customConditionsFinalized) {
			PCF::CustomConditions::Finalize();
		}

		if (!diagnostics.empty()) {
			std::string detail;
			for (const auto& diagnostic : diagnostics) {
				detail.append("\n- ");
				detail.append(diagnostic);
			}
			spdlog::warn("Registry: {} diagnostic issue(s):{}", diagnostics.size(), detail);
		}

		spdlog::info("Loaded {} of {} configuration file(s), {} [Perk=Perk] Rule(s), {} [Perk=ActorValue] Rule(s), {} [Perk=GlobalValue] Rule(s), {} description rule(s), {} Actor Value description assignment(s), {} requirement label(s) across {} target(s), {} name assignment(s), {} SWF assignment(s)",
			config.loadedFileCount, config.matchingFileCount, perkToPerkCount, perkToActorValueCount, perkToGlobalValueCount, descriptionsLoaded, actorValueDescriptionsLoaded, requirementLabelsLoaded, g_requirementLabels.size(), g_displayNames.size(), g_swfPaths.size());
		if (g_customConditionsFinalized) {
			LogCustomConditionSummary();
		}
		spdlog::info("Registry: {} source perk(s), {} resolved alternative(s), {} duplicate row(s), {} rejected row(s); {} description source perk(s), {} duplicate description row(s); {} Actor Value description(s), {} duplicate Actor Value description row(s), {} rejected Actor Value description row(s); {} requirement label(s) across {} target(s), {} duplicate requirement label row(s), {} rejected requirement label row(s)",
			g_rules.size(), resolved, duplicates, config.rules.size() - resolved - duplicates, g_descriptionRules.size(), descriptionDuplicates,
			g_actorValueDescriptions.size(), actorValueDescriptionDuplicates,
			config.actorValueDescriptions.size() - actorValueDescriptionsLoaded - actorValueDescriptionDuplicates,
			requirementLabelsLoaded, g_requirementLabels.size(), requirementLabelDuplicates,
			config.requirementLabels.size() - requirementLabelsLoaded - requirementLabelDuplicates);
	}

	// Finalizes deferred INFO [Conditions] targets against the authoritative dialogue-ready INFO snapshot.
	void FinalizeCustomConditions(const std::vector<RE::TESTopicInfo*>& a_infos)
	{
		if (g_customConditionsFinalized) {
			LogCustomConditionSummary();
			return;
		}

		Diagnostics diagnostics;
		// INFO records are nested beneath DIAL and are not guaranteed to exist in the global TESForm lookup.
		// Their runtime FormID still carries the stable identity of the plugin that originally defined them,
		// including through override chains. Index the authoritative loaded INFO snapshot by that identity.
		std::unordered_map<std::uint32_t, RE::TESTopicInfo*> loadedInfosByFormID;
		loadedInfosByFormID.reserve(a_infos.size());
		for (auto* info : a_infos) {
			if (!info) {
				continue;
			}
			auto [entry, inserted] = loadedInfosByFormID.try_emplace(info->GetFormID(), info);
			if (!inserted && entry->second != info) {
				entry->second = nullptr;
			}
		}

		auto* data = RE::TESDataHandler::GetSingleton();
		for (auto& pending : g_pendingCustomConditions) {
			if (!data) {
				diagnostics.push_back(fmt::format("{}:{} - TESDataHandler unavailable while resolving deferred INFO target: {} | {}",
					pending.file.filename().string(), pending.line, pending.targetPlugin, pending.targetReference));
				continue;
			}
			const auto* file = data->LookupModByName(pending.targetPlugin);
			if (!file) {
				diagnostics.push_back(fmt::format("{}:{} - Missing plugin for deferred Custom condition INFO target: {} | {}",
					pending.file.filename().string(), pending.line, pending.targetPlugin, pending.targetReference));
				continue;
			}
			if ((pending.configuredFormID >> 24) != 0 && !file->IsFormInMod(pending.configuredFormID)) {
				diagnostics.push_back(fmt::format("{}:{} - Full FormID does not belong to named plugin for deferred Custom condition INFO target: {} | {}",
					pending.file.filename().string(), pending.line, pending.targetPlugin, pending.targetReference));
				continue;
			}

			const auto localFormID = GetLocalFormID(file, pending.configuredFormID);
			const auto runtimeFormID = data->LookupFormID(localFormID, pending.targetPlugin);
			const auto found = runtimeFormID ? loadedInfosByFormID.find(runtimeFormID) : loadedInfosByFormID.end();
			if (found == loadedInfosByFormID.end() || !found->second) {
				diagnostics.push_back(fmt::format("{}:{} - [Conditions] target did not resolve in the loaded INFO graph (runtime FormID {:08X}): {} | {}",
					pending.file.filename().string(), pending.line, runtimeFormID, pending.targetPlugin, pending.targetReference));
				continue;
			}
			if (!PCF::CustomConditions::Add(found->second, std::move(pending.conditions))) {
				diagnostics.push_back(fmt::format("{}:{} - Failed to register complete deferred [Conditions] INFO target: {} | {}",
					pending.file.filename().string(), pending.line, pending.targetPlugin, pending.targetReference));
			}
		}

		g_pendingCustomConditions.clear();
		PCF::CustomConditions::Finalize();
		g_customConditionsFinalized = true;

		if (!diagnostics.empty()) {
			std::string detail;
			for (const auto& diagnostic : diagnostics) {
				detail.append("\n- ");
				detail.append(diagnostic);
			}
			spdlog::warn("Registry: {} deferred custom condition diagnostic issue(s):{}", diagnostics.size(), detail);
		}
		LogCustomConditionSummary();
	}

	// Reports whether INFO custom-condition targets are still waiting for dialogue-ready binding.
	bool HasPendingCustomConditions()
	{
		return !g_pendingCustomConditions.empty();
	}

	// Reports whether every configured custom-condition target has completed immediate/deferred binding.
	bool CustomConditionsFinalized()
	{
		return g_customConditionsFinalized;
	}
}
