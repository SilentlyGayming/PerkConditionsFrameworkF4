// Perk Conditions Framework
// SilentlyGayming
// CraftingMenus.cpp

#include "PCH.h"
#include "CraftingMenus.h"
#include "CustomConditions.h"
#include "NativeHooks.h"
#include "PerkConditions.h"
#include "RuleRegistry.h"
#include "TextManager.h"
#include "UICommon.h"
#include "UIManager.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{
	using PCF::UICommon::DepthGuard;
	using PCF::UICommon::EnsureNumber;
	using PCF::UICommon::ReadBool;
	using PCF::UICommon::ReadElement;
	using PCF::UICommon::ReadIndex;
	using PCF::UICommon::ReadNumber;
	using PCF::UICommon::ReadText;
	using PCF::UICommon::SetBool;
	using PCF::UICommon::SetNumber;
	using PCF::UICommon::SetText;
	using Value = PCF::UICommon::Value;

	// Builds a readable name for a game form.
	std::string DescribeForm(const RE::TESForm* a_form)
	{
		if (!a_form) {
			return "<unknown>";
		}
		const auto formID = a_form->GetFormID();
		const auto* files = a_form->sourceFiles.array;
		const auto* file = files && !files->empty() ? (*files)[0] : nullptr;
		const char* editorID = a_form->GetFormEditorID();
		if (file) {
			const auto localID = file->IsLight() ? formID & 0x00000FFF : formID & 0x00FFFFFF;
			if (editorID && editorID[0]) {
				return fmt::format("{}|{:08X} editorID={}", file->filename.data(), localID, editorID);
			}
			return fmt::format("{}|{:08X}", file->filename.data(), localID);
		}
		if (editorID && editorID[0]) {
			return fmt::format("{:08X} editorID={}", formID, editorID);
		}
		return fmt::format("{:08X}", formID);
	}
	using Params = PCF::UIHookState::Params;
	using CallFunction = PCF::UIHookState::CallFunction;
	constexpr std::size_t kCallSlot = 0x01;
	constexpr std::size_t kPreDisplaySlot = 0x05;
	constexpr std::size_t kExamineIndex = 0;
	constexpr std::size_t kCookingIndex = 1;
	constexpr std::size_t kPowerArmorIndex = 2;
	constexpr std::size_t kRobotIndex = 3;
	constexpr std::size_t kCraftingMenuCount = 4;
	constexpr std::size_t kRequirementSlots = 4;
	constexpr std::array<std::string_view, kRequirementSlots> kPerkPanelNames{
		"PerkPanel0_mc", "PerkPanel1_mc", "PerkPanel2_mc", "PerkPanel3_mc"
	};
	constexpr std::array<std::string_view, kRequirementSlots> kPerkPanelCloneNames{
		"PerkPanelClone0_mc", "PerkPanelClone1_mc", "PerkPanelClone2_mc", "PerkPanelClone3_mc"
	};
	constexpr std::uint32_t kPanelSearchDepth = 6;
	constexpr std::uint32_t kPanelSearchChildren = 96;
	constexpr std::uint32_t kPanelSearchLimit = 512;
	constexpr std::uint32_t kPanelLoaderChildren = 32;
	constexpr std::string_view kArtworkPathKey = "__PCFArtworkPath";

	struct ArtworkSearchResult
	{
		bool foundPanel{ false };
		bool truncated{ false };
	};

	struct PanelSearchItem
	{
		Value object;
		std::uint32_t depth{ 0 };
	};

	struct ArtworkSlot
	{
		std::uint32_t index{ 0 };
		RE::BGSPerk* originalPerk{ nullptr };
		std::uint32_t originalRank{ 0 };
		RE::TESForm* displayForm{ nullptr };
		std::string path;
		std::string originalRowName;
		bool armed{ false };
	};

	struct ArtworkPlan
	{
		std::uint32_t selectedIndex{ 0 };
		std::array<ArtworkSlot, kRequirementSlots> slots{};
		std::size_t count{ 0 };
		const RE::BGSConstructibleObject* customRecipe{ nullptr };
	};

	thread_local std::uint32_t g_callDepth{ 0 };
	thread_local std::uint32_t g_cookingPreDisplayDepth{ 0 };
	std::unordered_set<std::string> g_artworkWarnings;
// Checks whether a perk matches the requested rank.
	bool MatchesCraftingRank(RE::BGSPerk* a_source, std::uint32_t a_rank)
	{
		if (!a_source || a_rank == 0) {
			return false;
		}
		if (a_rank == 1) {
			return true;
		}
		const auto linkedRank = PCF::RuleRegistry::GetRank(a_source);
		if (linkedRank.base && linkedRank.rank == a_rank) {
			return true;
		}
		const auto foundRank = PCF::RuleRegistry::FindPerkRank(a_source, a_rank);
		return foundRank && foundRank.perk == a_source;
	}
	// Finds the configured perk used by a recipe row.
	RE::BGSPerk* FindRecipePerk(const RE::BGSConstructibleObject* a_recipe, RE::BGSPerk* a_originalPerk,
		std::uint32_t a_originalRank, std::size_t a_requirementIndex, std::size_t a_requirementCount)
	{
		if (!a_recipe || !a_originalPerk || a_originalRank == 0) {
			return nullptr;
		}

		RE::BGSPerk* indexedCondition = nullptr;
		std::size_t positivePerkCount = 0;
		RE::BGSPerk* exactSource = nullptr;
		RE::BGSPerk* rankMatch = nullptr;
		RE::BGSPerk* onlyCandidate = nullptr;
		std::size_t candidateCount = 0;
		const auto originalFamily = PCF::RuleRegistry::GetRank(a_originalPerk);
		for (auto* item = a_recipe->conditions.head; item; item = item->next) {
			if (!PCF::PerkConditions::IsPositivePerkCheck(item)) {
				continue;
			}
			auto* source = PCF::PerkConditions::GetConditionPerk(item);
			if (!source) {
				continue;
			}
			if (positivePerkCount == a_requirementIndex) {
				indexedCondition = source;
			}
			++positivePerkCount;

			const auto* configuredRule = PCF::RuleRegistry::Find(source);
			if (!configuredRule) {
				continue;
			}
			const auto sourceRank = PCF::RuleRegistry::GetRank(source);
			const bool sameFamily = sourceRank.base && originalFamily.base ?
				sourceRank.base == originalFamily.base : source == a_originalPerk;
			if (!sameFamily) {
				continue;
			}

			++candidateCount;
			onlyCandidate = source;
			if (source == a_originalPerk) {
				exactSource = source;
			}
			const auto value = sourceRank.base ? sourceRank.rank : 1u;
			if (value == a_originalRank) {
				rankMatch = source;
			}
		}

		if (indexedCondition && positivePerkCount == a_requirementCount &&
			PCF::RuleRegistry::Find(indexedCondition)) {
			return indexedCondition;
		}
		if (exactSource) {
			return exactSource;
		}
		if (rankMatch) {
			return rankMatch;
		}
		if (candidateCount == 1) {
			return onlyCandidate;
		}
		return nullptr;
	}
	// Finds the PCF rule for a crafting requirement row.
	const PCF::PerkRule* FindCraftingRule(const RE::WorkbenchMenuBase::ModChoiceData& a_choice,
		RE::BGSPerk* a_originalPerk, std::uint32_t a_originalRank, std::size_t a_requirementIndex,
		std::size_t a_requirementCount, RE::BGSPerk*& a_source)
	{
		a_source = nullptr;

		RE::BGSPerk* conditionSource = nullptr;
		const PCF::PerkRule* conditionRule = nullptr;
		if (a_choice.recipe) {
			conditionSource = FindRecipePerk(a_choice.recipe, a_originalPerk, a_originalRank, a_requirementIndex,
				a_requirementCount);
			conditionRule = conditionSource ? PCF::RuleRegistry::Find(conditionSource) : nullptr;
		}

		RE::BGSPerk* exactSource = nullptr;
		const PCF::PerkRule* exactRule = nullptr;
		if (MatchesCraftingRank(a_originalPerk, a_originalRank)) {
			exactRule = PCF::RuleRegistry::Find(a_originalPerk);
			if (exactRule) {
				exactSource = a_originalPerk;
			}
		}

		RE::BGSPerk* familySource = PCF::RuleRegistry::FindPerkRank(a_originalPerk, a_originalRank).perk;
		const auto* familyRule = familySource ? PCF::RuleRegistry::Find(familySource) : nullptr;

		if (conditionRule) {
			a_source = conditionSource;
			return conditionRule;
		}
		if (exactRule) {
			a_source = exactSource;
			return exactRule;
		}
		if (familyRule) {
			a_source = familySource;
			return familyRule;
		}
		return nullptr;
	}
	// Gets the form used for custom display.
	RE::TESForm* GetDisplayForm(const PCF::PerkAlternative& a_alternative)
	{
		switch (a_alternative.type) {
		case PCF::AlternativeType::kPerk:
			return a_alternative.perk;
		case PCF::AlternativeType::kActorValue:
			return a_alternative.actorValue;
		case PCF::AlternativeType::kGlobalValue:
			return a_alternative.globalValue;
		}
		return nullptr;
	}
	// Finds custom artwork for a crafting requirement.
	bool FindCustomArtwork(const RE::WorkbenchMenuBase::ModChoiceData& a_choice,
		RE::BGSPerk* a_originalPerk, std::uint32_t a_originalRank, std::size_t a_requirementIndex,
		std::size_t a_requirementCount, RE::TESForm*& a_form, std::string& a_path)
	{
		a_form = nullptr;
		a_path.clear();
		RE::BGSPerk* source = nullptr;
		const auto* rule = FindCraftingRule(a_choice, a_originalPerk, a_originalRank, a_requirementIndex,
			a_requirementCount, source);
		if (rule) {
			if (const auto* display = PCF::RuleRegistry::GetDisplay(*rule)) {
				a_form = GetDisplayForm(*display);
				if (display->iconPath.empty()) {
					a_form = nullptr;
					return false;
				}
				a_path = display->iconPath;
				return true;
			}
		}

		const auto resolved = PCF::RuleRegistry::FindPerkRank(a_originalPerk, a_originalRank);
		if (resolved.perk) {
			if (const auto* path = PCF::RuleRegistry::FindSWF(resolved.perk); path && !path->empty()) {
				a_form = resolved.perk;
				a_path = *path;
				return true;
			}
		}
		if (const auto* path = PCF::RuleRegistry::FindSWF(a_originalPerk); path && !path->empty()) {
			a_form = a_originalPerk;
			a_path = *path;
			return true;
		}
		return false;
	}
	// Gets the selected crafting requirement rows.
	bool GetSelectedRows(RE::IMenu* a_menu, std::uint32_t& a_selected, Value& a_rows)
	{
		if (!a_menu || !a_menu->menuObj.IsAnyObject()) {
			return false;
		}
		Value list;
		Value entries;
		Value entry;
		std::uint32_t count = 0;
		if (!a_menu->menuObj.GetMember("ModListObject", &list) || !list.IsAnyObject() ||
			!list.GetMember("entryList", &entries) || !entries.IsArray() || !ReadIndex(entries, "length", count) ||
			!ReadIndex(list, "selectedIndex", a_selected) || a_selected >= count ||
			!ReadElement(entries, a_selected, entry) || !entry.GetMember("perkData", &a_rows) || !a_rows.IsArray()) {
			return false;
		}
		return true;
	}
	// Collects custom artwork for the selected crafting row.
	bool BuildArtworkPlan(RE::IMenu* a_menu, ArtworkPlan& a_plan)
	{
		a_plan = {};
		auto* workbench = RE::DynamicCast<RE::WorkbenchMenuBase*>(a_menu);
		Value list;
		if (!workbench || !a_menu || !a_menu->menuObj.IsAnyObject() ||
			!a_menu->menuObj.GetMember("ModListObject", &list) || !list.IsAnyObject() ||
			!ReadIndex(list, "selectedIndex", a_plan.selectedIndex) ||
			static_cast<std::size_t>(a_plan.selectedIndex) >= static_cast<std::size_t>(workbench->modChoiceArray.size())) {
			return false;
		}

		const auto& choice = workbench->modChoiceArray[
			static_cast<decltype(workbench->modChoiceArray.size())>(a_plan.selectedIndex)];
		if (choice.recipe) {
			if (const auto* custom = PCF::CustomConditions::Find(static_cast<const RE::TESForm*>(choice.recipe));
				custom && custom->owner == PCF::CustomConditions::OwnerKind::kCrafting) {
				a_plan.customRecipe = choice.recipe;
				for (const auto& condition : custom->conditions) {
					if (!PCF::CustomConditions::IsPlayerFacing(condition) || a_plan.count >= kRequirementSlots) {
						continue;
					}
					auto& slot = a_plan.slots[a_plan.count++];
					slot.index = static_cast<std::uint32_t>(a_plan.count - 1);
					slot.displayForm = PCF::CustomConditions::GetDisplayForm(condition);
					if (const auto* path = PCF::CustomConditions::GetArtworkPath(condition)) {
						slot.path = *path;
					}
				}
				return a_plan.count > 0;
			}
		}
		Value rows;
		std::uint32_t selected = 0;
		std::uint32_t rowCount = 0;
		if (!GetSelectedRows(a_menu, selected, rows) || selected != a_plan.selectedIndex || !ReadIndex(rows, "length", rowCount)) {
			return false;
		}
		const auto& requirements = choice.requiredPerks;
		const auto count = (std::min)({ static_cast<std::size_t>(rowCount),
			static_cast<std::size_t>(requirements.size()), kRequirementSlots });
		for (std::size_t i = 0; i < count; ++i) {
			const auto& requirement = requirements[static_cast<decltype(requirements.size())>(i)];
			RE::TESForm* form = nullptr;
			std::string path;
			if (!FindCustomArtwork(choice, requirement.first, requirement.second, i, requirements.size(), form, path)) {
				continue;
			}
			auto& slot = a_plan.slots[a_plan.count++];
			slot.index = static_cast<std::uint32_t>(i);
			slot.originalPerk = requirement.first;
			slot.originalRank = requirement.second;
			slot.displayForm = form;
			slot.path = std::move(path);
		}
		return a_plan.count > 0;
	}
	// Checks whether the artwork still matches the current selection.
	bool ArtworkMatchesSelection(RE::IMenu* a_menu, const ArtworkPlan& a_plan)
	{
		auto* workbench = RE::DynamicCast<RE::WorkbenchMenuBase*>(a_menu);
		Value list;
		std::uint32_t selected = 0;
		if (!workbench || !a_menu || !a_menu->menuObj.IsAnyObject() ||
			!a_menu->menuObj.GetMember("ModListObject", &list) || !list.IsAnyObject() ||
			!ReadIndex(list, "selectedIndex", selected) || selected != a_plan.selectedIndex ||
			static_cast<std::size_t>(selected) >= static_cast<std::size_t>(workbench->modChoiceArray.size())) {
			return false;
		}
		const auto& choice = workbench->modChoiceArray[
			static_cast<decltype(workbench->modChoiceArray.size())>(selected)];
		if (a_plan.customRecipe) {
			return choice.recipe == a_plan.customRecipe &&
				PCF::CustomConditions::Find(static_cast<const RE::TESForm*>(choice.recipe)) != nullptr;
		}
		Value rows;
		if (!GetSelectedRows(a_menu, selected, rows)) {
			return false;
		}
		const auto& requirements = choice.requiredPerks;
		for (std::size_t i = 0; i < a_plan.count; ++i) {
			const auto& slot = a_plan.slots[i];
			if (static_cast<std::size_t>(slot.index) >= static_cast<std::size_t>(requirements.size())) {
				return false;
			}
			const auto& requirement = requirements[static_cast<decltype(requirements.size())>(slot.index)];
			if (requirement.first != slot.originalPerk || requirement.second != slot.originalRank) {
				return false;
			}
		}
		return true;
	}
	// Finds the ExamineMenu panel for a requirement slot.
	bool GetExaminePanel(RE::IMenu* a_menu, std::uint32_t a_index, Value& a_panel)
	{
		if (!a_menu || !a_menu->menuObj.IsAnyObject() || a_index >= kRequirementSlots) {
			return false;
		}
		const auto name = kPerkPanelNames[a_index];
		if (a_menu->menuObj.GetMember(name.data(), &a_panel) && a_panel.IsAnyObject()) {
			return true;
		}
		const Value argument(name.data());
		return a_menu->menuObj.Invoke("getChildByName", &a_panel, &argument, 1) && a_panel.IsAnyObject();
	}
	// Marks every artwork slot as inactive.
	void ResetArtworkSlots(ArtworkPlan& a_plan)
	{
		for (std::size_t i = 0; i < a_plan.count; ++i) {
			a_plan.slots[i].armed = false;
		}
	}
	// Updates the perk name shown on an ExamineMenu panel.
	bool SetPanelName(Value& a_nameField, const std::string& a_name)
	{
		std::string existing;
		if (ReadText(a_nameField, "text", existing) && existing == a_name) {
			return true;
		}
		Value format;
		const bool hasFormat = a_nameField.IsAnyObject() && a_nameField.Invoke("getTextFormat", &format, nullptr, 0) &&
			format.IsAnyObject();
		if (!a_nameField.IsAnyObject() || !a_nameField.SetMember("text", Value(a_name.c_str()))) {
			return false;
		}
		if (hasFormat) {
			a_nameField.Invoke("setTextFormat", nullptr, &format, 1);
		}
		return true;
	}
	// Finds a crafting requirement panel by name.
	bool FindPanelByName(RE::IMenu* a_menu, std::string_view a_name, Value& a_panel)
	{
		if (!a_menu || !a_menu->menuObj.IsAnyObject()) {
			return false;
		}
		if (a_menu->menuObj.GetMember(a_name.data(), &a_panel) && a_panel.IsAnyObject()) {
			return true;
		}
		const Value argument(a_name.data());
		return a_menu->menuObj.Invoke("getChildByName", &a_panel, &argument, 1) && a_panel.IsAnyObject();
	}
	// Finds the panels used by a crafting slot.
	std::size_t FindPanelsForSlot(RE::IMenu* a_menu, std::size_t a_menuIndex,
		std::uint32_t a_requirementIndex, std::array<Value, 2>& a_panels)
	{
		if (!a_menu ||
			(a_menuIndex != kCookingIndex && a_menuIndex != kPowerArmorIndex && a_menuIndex != kRobotIndex) ||
			a_requirementIndex >= kRequirementSlots) {
			return 0;
		}

		const auto directName = kPerkPanelNames[a_requirementIndex];
		const auto cloneName = kPerkPanelCloneNames[a_requirementIndex];
		const std::array names = a_menuIndex == kCookingIndex ?
			std::array{ cloneName, directName } : std::array{ directName, cloneName };

		std::size_t count = 0;
		for (const auto& name : names) {
			Value panel;
			if (FindPanelByName(a_menu, name, panel)) {
				a_panels[count++] = std::move(panel);
			}
		}
		return count;
	}
	// Checks whether a panel belongs to a requirement row.
	bool PanelMatchesRow(Value& a_panel, const std::string& a_rowName)
	{
		Value nameField;
		std::string panelName;
		return a_panel.IsAnyObject() &&
			a_panel.GetMember("PerkName_tf", &nameField) && nameField.IsAnyObject() &&
			ReadText(nameField, "text", panelName) && panelName == a_rowName;
	}
	// Prepares crafting artwork before the game updates the menu.
	bool PrepareCraftingArtwork(RE::IMenu* a_menu, ArtworkPlan& a_plan,
		std::size_t a_menuIndex, bool a_initial)
	{
		if (!ArtworkMatchesSelection(a_menu, a_plan)) {
			ResetArtworkSlots(a_plan);
			return false;
		}
		Value rows;
		std::uint32_t selected = 0;
		if (!GetSelectedRows(a_menu, selected, rows)) {
			ResetArtworkSlots(a_plan);
			return false;
		}

		bool any = false;
		for (std::size_t i = 0; i < a_plan.count; ++i) {
			auto& slot = a_plan.slots[i];
			if (!a_initial && !slot.armed) {
				continue;
			}

			Value row;
			std::string rowName;
			bool ready = false;
			if (ReadElement(rows, slot.index, row) && ReadText(row, "perkName", rowName)) {
				std::array<Value, 2> panels;
				const auto panelCount = FindPanelsForSlot(a_menu, a_menuIndex, slot.index, panels);
				for (std::size_t panelIndex = 0; panelIndex < panelCount; ++panelIndex) {
					Value nameField;
					if (panels[panelIndex].GetMember("PerkName_tf", &nameField) && nameField.IsAnyObject() &&
						SetPanelName(nameField, rowName)) {
						ready = true;
					}
				}
			}

			if (a_initial) {
				slot.originalRowName = ready ? rowName : std::string{};
				slot.armed = ready;
			} else if (!ready) {
				slot.armed = false;
			}
			any = ready || any;
		}
		return any;
	}
	// Checks that crafting artwork still matches after the game updates the menu.
	bool CheckCraftingArtwork(RE::IMenu* a_menu, ArtworkPlan& a_plan,
		std::size_t a_menuIndex)
	{
		if (!ArtworkMatchesSelection(a_menu, a_plan)) {
			ResetArtworkSlots(a_plan);
			return false;
		}

		Value rows;
		std::uint32_t selected = 0;
		if (!GetSelectedRows(a_menu, selected, rows)) {
			ResetArtworkSlots(a_plan);
			return false;
		}

		bool any = false;
		for (std::size_t i = 0; i < a_plan.count; ++i) {
			auto& slot = a_plan.slots[i];
			if (!slot.armed) {
				continue;
			}

			Value row;
			std::string rowName;
			if (!ReadElement(rows, slot.index, row) || !ReadText(row, "perkName", rowName) ||
				rowName != slot.originalRowName) {
				slot.armed = false;
				continue;
			}

			std::array<Value, 2> panels;
			const auto panelCount = FindPanelsForSlot(a_menu, a_menuIndex, slot.index, panels);
			bool represented = false;
			for (std::size_t panelIndex = 0; panelIndex < panelCount; ++panelIndex) {
				represented = PanelMatchesRow(panels[panelIndex], rowName) || represented;
			}
			if (!represented) {
				slot.armed = false;
				continue;
			}
			any = true;
		}
		return any;
	}
	// Prepares ExamineMenu artwork before the game updates the menu.
	bool PrepareExamineArtwork(RE::IMenu* a_menu, ArtworkPlan& a_plan, bool a_initial)
	{
		if (!ArtworkMatchesSelection(a_menu, a_plan)) {
			ResetArtworkSlots(a_plan);
			return false;
		}
		Value rows;
		std::uint32_t selected = 0;
		if (!GetSelectedRows(a_menu, selected, rows)) {
			ResetArtworkSlots(a_plan);
			return false;
		}

		bool any = false;
		for (std::size_t i = 0; i < a_plan.count; ++i) {
			auto& slot = a_plan.slots[i];
			if (!a_initial && !slot.armed) {
				continue;
			}
			Value row;
			Value panel;
			Value nameField;
			std::string rowName;
			const bool ready = ReadElement(rows, slot.index, row) && ReadText(row, "perkName", rowName) &&
				GetExaminePanel(a_menu, slot.index, panel) &&
				panel.GetMember("PerkName_tf", &nameField) && nameField.IsAnyObject() &&
				SetPanelName(nameField, rowName);
			if (a_initial) {
				slot.originalRowName = ready ? rowName : std::string{};
				slot.armed = ready;
			} else if (!ready) {
				slot.armed = false;
			}
			any = ready || any;
		}
		return any;
	}
	// Checks that ExamineMenu artwork still matches after the game updates the menu.
	bool CheckExamineArtwork(RE::IMenu* a_menu, ArtworkPlan& a_plan)
	{
		if (!ArtworkMatchesSelection(a_menu, a_plan)) {
			ResetArtworkSlots(a_plan);
			return false;
		}
		Value rows;
		std::uint32_t selected = 0;
		if (!GetSelectedRows(a_menu, selected, rows)) {
			ResetArtworkSlots(a_plan);
			return false;
		}
		bool any = false;
		for (std::size_t i = 0; i < a_plan.count; ++i) {
			auto& slot = a_plan.slots[i];
			if (!slot.armed) {
				continue;
			}
			Value row;
			std::string rowName;
			if (!ReadElement(rows, slot.index, row) || !ReadText(row, "perkName", rowName) ||
				rowName != slot.originalRowName) {
				slot.armed = false;
				continue;
			}
			any = true;
		}
		return any;
	}
	// Updates one requirement row with PCF text and lock state.
	bool UpdateRow(Value& a_row, RE::BGSPerk* a_source, const PCF::PerkRule* a_rule)
	{
		if (!a_rule || !a_source || !a_row.IsAnyObject()) {
			return false;
		}
		const auto* presentation = PCF::TextManager::GetConfiguredRequirement(a_source);
		bool changed = SetBool(a_row, "visible", true);
		changed = SetBool(a_row, "perkLocked", !PCF::PerkConditions::PlayerPasses(a_source)) || changed;
		if (presentation && presentation->overrideNativeText) {
			changed = SetText(a_row, "perkName", presentation->rowLabel) || changed;
			changed = SetNumber(a_row, "perkRank", presentation->value) || changed;
		}
		return changed;
	}
	// Replaces one crafting entry's native requirement rows with PCF-owned custom rows.
	bool UpdateCustomEntry(Value& a_entry, RE::IMenu* a_menu, const PCF::CustomConditions::ConditionSet& a_set)
	{
		if (!a_menu || !a_menu->uiMovie || !a_entry.IsAnyObject()) {
			return false;
		}
		Value rows;
		a_menu->uiMovie->CreateArray(&rows);
		if (!rows.IsArray()) {
			return false;
		}
		std::size_t visibleCount = 0;
		for (const auto& condition : a_set.conditions) {
			if (!PCF::CustomConditions::IsPlayerFacing(condition) || visibleCount >= kRequirementSlots) {
				continue;
			}
			Value row;
			a_menu->uiMovie->CreateObject(&row);
			auto* form = PCF::CustomConditions::GetDisplayForm(condition);
			if (!row.IsAnyObject() || !form ||
				!row.SetMember("visible", Value(true)) ||
				!row.SetMember("perkName", Value(condition.presentation.rowLabel.c_str())) ||
				!row.SetMember("perkID", Value(static_cast<double>(form->GetFormID()))) ||
				!row.SetMember("perkRank", Value(condition.presentation.value)) ||
				!row.SetMember("perkLocked", Value(!PCF::CustomConditions::Evaluate(condition)))) {
				return false;
			}
			const auto* artwork = PCF::CustomConditions::GetArtworkPath(condition);
			const auto swfName = artwork && !artwork->empty() ? *artwork + ".swf" : std::string{};
			if (!row.SetMember("swfName", Value(swfName.c_str())) || !rows.PushBack(row)) {
				return false;
			}
			++visibleCount;
		}
		return a_entry.SetMember("perkData", rows);
	}

	// Updates one crafting list entry.
	bool UpdateEntry(RE::IMenu* a_menu, const Value& a_entries, std::uint32_t a_entryIndex,
		const RE::WorkbenchMenuBase::ModChoiceData& a_choice)
	{
		Value entry;
		if (!ReadElement(a_entries, a_entryIndex, entry) || !entry.IsAnyObject()) {
			return false;
		}
		if (a_choice.recipe) {
			if (const auto* custom = PCF::CustomConditions::Find(static_cast<const RE::TESForm*>(a_choice.recipe));
				custom && custom->owner == PCF::CustomConditions::OwnerKind::kCrafting) {
				return UpdateCustomEntry(entry, a_menu, *custom);
			}
		}

		Value rows;
		std::uint32_t count = 0;
		if (!entry.GetMember("perkData", &rows) || !rows.IsArray() || !ReadIndex(rows, "length", count)) {
			return false;
		}

		const auto& requirements = a_choice.requiredPerks;
		const auto rowsToVisit = (std::min)(static_cast<std::size_t>(count), static_cast<std::size_t>(requirements.size()));
		bool changed = false;
		for (std::size_t i = 0; i < rowsToVisit; ++i) {
			const auto& requirement = requirements[static_cast<decltype(requirements.size())>(i)];
			RE::BGSPerk* source = nullptr;
			const auto* rule = FindCraftingRule(a_choice, requirement.first, requirement.second, i,
				requirements.size(), source);
			Value row;
			if (rule && source && ReadElement(rows, static_cast<std::uint32_t>(i), row)) {
				changed = UpdateRow(row, source, rule) || changed;
			}
		}
		return changed;
	}

	// Applies one player-facing PCF-owned custom requirement after the menu rebuilds its panels.
	bool ApplyCustomRequirementPanel(Value& a_panel, const PCF::CustomConditions::Condition& a_condition)
	{
		if (!a_panel.IsAnyObject() || !PCF::CustomConditions::IsPlayerFacing(a_condition)) {
			return false;
		}
		bool changed = SetBool(a_panel, "visible", true);
		Value nameField;
		Value requirementField;
		Value lock;
		if (a_panel.GetMember("PerkName_tf", &nameField) && nameField.IsAnyObject()) {
			changed = SetBool(nameField, "visible", true) || changed;
			changed = SetPanelName(nameField, a_condition.presentation.rowLabel) || changed;
		}
		if (a_panel.GetMember("Requires_tf", &requirementField) && requirementField.IsAnyObject()) {
			changed = SetBool(requirementField, "visible", true) || changed;
			std::string existing;
			std::string prefix;
			if (ReadText(requirementField, "text", existing)) {
				const auto colon = existing.find(':');
				if (colon != std::string::npos) {
					prefix = existing.substr(0, colon + 1);
				} else {
					const auto number = existing.find_first_of("0123456789<>=");
					if (number != std::string::npos) {
						prefix = existing.substr(0, number);
					}
				}
			}
			if (prefix.empty()) {
				prefix = "Requires:";
			}
			std::string text = prefix;
			if (!text.empty() && text.back() != ' ') {
				text.push_back(' ');
			}
			text.append(a_condition.presentation.valueText);
			changed = SetText(requirementField, "text", text) || changed;
		}
		if (a_panel.GetMember("PerkLock_mc", &lock) && lock.IsAnyObject()) {
			changed = SetBool(lock, "visible", !PCF::CustomConditions::Evaluate(a_condition)) || changed;
		}
		return changed;
	}

	// Clears one fixed crafting requirement panel that is not represented by the
	// current PCF-owned visible condition set. CookingMenu keeps these panel
	// instances alive between recipes, so merely shrinking perkData can leave
	// stale text/artwork from a hidden quest condition on screen.
	bool ClearCustomRequirementPanel(Value& a_panel)
	{
		if (!a_panel.IsAnyObject()) {
			return false;
		}
		bool changed = SetBool(a_panel, "visible", false);
		for (const auto* member : { "PerkName_tf", "Requires_tf", "PerkLock_mc", "PerkLoaderClip_mc" }) {
			Value child;
			if (!a_panel.GetMember(member, &child) || !child.IsAnyObject()) {
				continue;
			}
			changed = SetBool(child, "visible", false) || changed;
			if (std::string_view(member) == "PerkName_tf" || std::string_view(member) == "Requires_tf") {
				changed = SetText(child, "text", "") || changed;
			}
			if (std::string_view(member) == "PerkLoaderClip_mc") {
				child.SetMember(kArtworkPathKey.data(), Value());
			}
		}
		return changed;
	}

	// Ensures the currently selected custom recipe has exactly the player-facing
	// requirement panels it owns. Quest progression stays in gameplay evaluation
	// but never survives as a stale fixed panel.
	void ApplySelectedCustomRequirementPanels(RE::IMenu* a_menu, std::size_t a_menuIndex,
		const PCF::CustomConditions::ConditionSet& a_set)
	{
		std::size_t visibleIndex = 0;
		for (const auto& condition : a_set.conditions) {
			if (!PCF::CustomConditions::IsPlayerFacing(condition) || visibleIndex >= kRequirementSlots) {
				continue;
			}
			if (a_menuIndex == kExamineIndex) {
				Value panel;
				if (GetExaminePanel(a_menu, static_cast<std::uint32_t>(visibleIndex), panel)) {
					ApplyCustomRequirementPanel(panel, condition);
				}
				++visibleIndex;
				continue;
			}
			std::array<Value, 2> panels;
			const auto panelCount = FindPanelsForSlot(a_menu, a_menuIndex, static_cast<std::uint32_t>(visibleIndex), panels);
			for (std::size_t panelIndex = 0; panelIndex < panelCount; ++panelIndex) {
				ApplyCustomRequirementPanel(panels[panelIndex], condition);
			}
			++visibleIndex;
		}

		for (std::size_t i = visibleIndex; i < kRequirementSlots; ++i) {
			if (a_menuIndex == kExamineIndex) {
				Value panel;
				if (GetExaminePanel(a_menu, static_cast<std::uint32_t>(i), panel)) {
					ClearCustomRequirementPanel(panel);
				}
				continue;
			}
			std::array<Value, 2> panels;
			const auto panelCount = FindPanelsForSlot(a_menu, a_menuIndex, static_cast<std::uint32_t>(i), panels);
			for (std::size_t panelIndex = 0; panelIndex < panelCount; ++panelIndex) {
				ClearCustomRequirementPanel(panels[panelIndex]);
			}
		}
	}

	// Returns the PCF-owned condition set for the currently selected crafting row.
	const PCF::CustomConditions::ConditionSet* GetSelectedCustomConditionSet(RE::IMenu* a_menu)
	{
		if (!a_menu) {
			return nullptr;
		}
		auto* workbench = RE::DynamicCast<RE::WorkbenchMenuBase*>(a_menu);
		if (!workbench || !a_menu->menuObj.IsAnyObject()) {
			return nullptr;
		}
		Value list;
		std::uint32_t selected = 0;
		if (!a_menu->menuObj.GetMember("ModListObject", &list) || !list.IsAnyObject() ||
			!ReadIndex(list, "selectedIndex", selected) ||
			static_cast<std::size_t>(selected) >= static_cast<std::size_t>(workbench->modChoiceArray.size())) {
			return nullptr;
		}
		const auto& choice = workbench->modChoiceArray[
			static_cast<decltype(workbench->modChoiceArray.size())>(selected)];
		if (!choice.recipe) {
			return nullptr;
		}
		const auto* custom = PCF::CustomConditions::Find(static_cast<const RE::TESForm*>(choice.recipe));
		return custom && custom->owner == PCF::CustomConditions::OwnerKind::kCrafting ? custom : nullptr;
	}

	// Reasserts the final player-facing state for the selected PCF-owned recipe.
	// CookingMenu can rebuild its fixed perk panels after a native Call callback,
	// so this is also used from PreDisplay to win the final write for the frame.
	void ReassertSelectedCustomConditionPanels(RE::IMenu* a_menu, std::size_t a_menuIndex)
	{
		if (!a_menu || !PCF::CustomConditions::HasCraftingTargets()) {
			return;
		}
		if (const auto* custom = GetSelectedCustomConditionSet(a_menu)) {
			ApplySelectedCustomRequirementPanels(a_menu, a_menuIndex, *custom);
		}
	}

	// Updates requirement rows for a crafting menu.
	bool UpdateCraftingRows(RE::IMenu* a_menu, ArtworkPlan* a_artworkPlan = nullptr,
		std::size_t a_artworkMenuIndex = kExamineIndex)
	{
		auto* workbench = RE::DynamicCast<RE::WorkbenchMenuBase*>(a_menu);
		if (!workbench || !a_menu->menuObj.IsAnyObject()) {
			return false;
		}
		Value list;
		Value entries;
		std::uint32_t count = 0;
		if (!a_menu->menuObj.GetMember("ModListObject", &list) || !list.IsAnyObject() ||
			!list.GetMember("entryList", &entries) || !entries.IsArray() || !ReadIndex(entries, "length", count)) {
			return false;
		}
		const auto choices = workbench->modChoiceArray.size();
		const auto entriesToVisit = (std::min)(static_cast<std::size_t>(count), static_cast<std::size_t>(choices));
		std::uint32_t selected = 0;
		const bool hasSelection = ReadIndex(list, "selectedIndex", selected) && static_cast<std::size_t>(selected) < count;
		bool primed = false;
		const bool hasPreparedState = ReadBool(a_menu->menuObj, "__PCFWorkbenchPrimed", primed);
		const bool prepareAllEntries = !hasPreparedState || !primed;
		const bool hasEntriesToPrepare = entriesToVisit > 0;
		bool changed = false;

		if (prepareAllEntries && hasEntriesToPrepare) {
			for (std::size_t i = 0; i < entriesToVisit; ++i) {
				const auto choiceIndex = static_cast<decltype(workbench->modChoiceArray.size())>(i);
				changed = UpdateEntry(a_menu, entries, static_cast<std::uint32_t>(i), workbench->modChoiceArray[choiceIndex]) || changed;
			}
			a_menu->menuObj.SetMember("__PCFWorkbenchPrimed", Value(true));
		}

		if (hasSelection) {
			const auto selectedIndex = static_cast<std::size_t>(selected);
			if (selectedIndex < static_cast<std::size_t>(workbench->modChoiceArray.size())) {
				const auto& rowChoice = workbench->modChoiceArray[static_cast<decltype(workbench->modChoiceArray.size())>(selectedIndex)];
				changed = UpdateEntry(a_menu, entries, selected, rowChoice) || changed;
			}
		}

		if (changed) {
			if (a_artworkPlan) {
				const bool initializeArtwork = a_artworkPlan->customRecipe != nullptr;
				if (a_artworkMenuIndex == kExamineIndex) {
					PrepareExamineArtwork(a_menu, *a_artworkPlan, initializeArtwork);
				} else {
					PrepareCraftingArtwork(a_menu, *a_artworkPlan, a_artworkMenuIndex, initializeArtwork);
				}
			}
			a_menu->menuObj.Invoke("UpdatePerks");
		}

		// Panel instances outlive perkData changes in CookingMenu. Reassert custom
		// ownership on every hooked update, even when the data row was already
		// correct, so native frame logic cannot resurrect a hidden quest slot or
		// stale text from the previously selected recipe.
		if (hasSelection) {
			ReassertSelectedCustomConditionPanels(a_menu, a_artworkMenuIndex);
		}
		return true;
	}
	// Finds the artwork loader inside a requirement panel.
	bool FindLoader(Value& a_panel, Value& a_loader)
	{
		if (!a_panel.IsAnyObject()) {
			return false;
		}
		if (a_panel.GetMember("PerkLoaderClip_mc", &a_loader) && a_loader.IsAnyObject()) {
			return true;
		}
		Value nameField;
		Value requirementField;
		if (!a_panel.GetMember("PerkName_tf", &nameField) || !nameField.IsAnyObject() ||
			!a_panel.GetMember("Requires_tf", &requirementField) || !requirementField.IsAnyObject()) {
			return false;
		}
		std::uint32_t childCount = 0;
		if (!ReadIndex(a_panel, "numChildren", childCount)) {
			return false;
		}
		const auto childrenToVisit = (std::min)(childCount, kPanelLoaderChildren);
		for (std::uint32_t i = 0; i < childrenToVisit; ++i) {
			Value child;
			const Value argument(static_cast<double>(i));
			if (!a_panel.Invoke("getChildAt", &child, &argument, 1) || !child.IsAnyObject()) {
				continue;
			}
			Value load;
			if (child.GetMember("SWFLoad", &load) && (load.GetType() == Value::ValueType::kClosure || load.IsAnyObject())) {
				a_loader = child;
				return true;
			}
		}
		return false;
	}
	// Checks whether PCF is using an artwork slot.
	bool IsArtworkSlotActive(const ArtworkPlan* a_plan, std::uint32_t a_index)
	{
		if (!a_plan) {
			return false;
		}
		for (std::size_t i = 0; i < a_plan->count; ++i) {
			const auto& slot = a_plan->slots[i];
			if (slot.index == a_index && slot.armed) {
				return true;
			}
		}
		return false;
	}
	// Clears PCF artwork from unused ExamineMenu slots.
	void ClearUnusedExamineArtwork(RE::IMenu* a_menu, const ArtworkPlan* a_plan)
	{
		for (std::uint32_t i = 0; i < kRequirementSlots; ++i) {
			if (IsArtworkSlotActive(a_plan, i)) {
				continue;
			}
			Value panel;
			Value loader;
			if (GetExaminePanel(a_menu, i, panel) && FindLoader(panel, loader)) {
				loader.SetMember(kArtworkPathKey.data(), Value());
			}
		}
	}
	// Logs each failed artwork load once.
	void WarnArtworkFailure(RE::TESForm* a_form, const std::string& a_path)
	{
		const auto key = fmt::format("{}|{}", a_form ? a_form->GetFormID() : 0, a_path);
		if (g_artworkWarnings.insert(key).second) {
			spdlog::warn("[Artwork] Failed to request {} for {}", a_path, DescribeForm(a_form));
		}
	}
	// Shows custom artwork in ExamineMenu.
	void ShowExamineArtwork(RE::IMenu* a_menu, ArtworkPlan& a_plan)
	{
		if (!ArtworkMatchesSelection(a_menu, a_plan)) {
			ResetArtworkSlots(a_plan);
			return;
		}
		bool presented = false;
		for (std::size_t i = 0; i < a_plan.count; ++i) {
			auto& slot = a_plan.slots[i];
			if (!slot.armed || slot.path.empty()) {
				continue;
			}
			Value panel;
			Value loader;
			if (!GetExaminePanel(a_menu, slot.index, panel) || !FindLoader(panel, loader)) {
				continue;
			}

			std::string currentPath;
			const bool alreadyRequested = ReadText(loader, kArtworkPathKey.data(), currentPath) &&
				currentPath == slot.path;
			if (!alreadyRequested) {
				SetNumber(loader, "clipScale", 0.39);
				const Value argument(slot.path.c_str());
				if (!loader.Invoke("SWFLoad", nullptr, &argument, 1)) {
					loader.SetMember(kArtworkPathKey.data(), Value());
					WarnArtworkFailure(slot.displayForm, slot.path);
					continue;
				}
				loader.SetMember(kArtworkPathKey.data(), Value(slot.path.c_str()));
			}
			SetBool(loader, "visible", true);
			presented = true;
		}
		if (presented && a_menu && a_menu->menuObj.IsAnyObject()) {
			a_menu->menuObj.SetMember("__PCFPanelScanStable", Value(false));
		}
	}
	// Clears PCF artwork from unused crafting slots.
	void ClearUnusedCraftingArtwork(RE::IMenu* a_menu, std::size_t a_menuIndex,
		const ArtworkPlan* a_plan)
	{
		for (std::uint32_t i = 0; i < kRequirementSlots; ++i) {
			if (IsArtworkSlotActive(a_plan, i)) {
				continue;
			}
			std::array<Value, 2> panels;
			const auto panelCount = FindPanelsForSlot(a_menu, a_menuIndex, i, panels);
			for (std::size_t panelIndex = 0; panelIndex < panelCount; ++panelIndex) {
				Value loader;
				if (FindLoader(panels[panelIndex], loader)) {
					loader.SetMember(kArtworkPathKey.data(), Value());
				}
			}
		}
	}
	// Restores the game's artwork for slots without custom artwork.
	void RestoreOriginalCraftingArtwork(RE::IMenu* a_menu, std::size_t a_menuIndex,
		const ArtworkPlan* a_plan)
	{
		for (std::uint32_t i = 0; i < kRequirementSlots; ++i) {
			bool managed = false;
			if (a_plan) {
				for (std::size_t slot = 0; slot < a_plan->count; ++slot) {
					if (a_plan->slots[slot].index == i &&
						(a_plan->customRecipe != nullptr || !a_plan->slots[slot].path.empty())) {
						managed = true;
						break;
					}
				}
			}
			if (managed) {
				continue;
			}
			std::array<Value, 2> panels;
			const auto panelCount = FindPanelsForSlot(a_menu, a_menuIndex, i, panels);
			for (std::size_t panelIndex = 0; panelIndex < panelCount; ++panelIndex) {
				Value loader;
				if (FindLoader(panels[panelIndex], loader)) {
					loader.SetMember(kArtworkPathKey.data(), Value());
					SetBool(loader, "visible", true);
				}
			}
		}
	}
	// Finds the visible crafting panel for a requirement row.
	bool FindVisibleCraftingPanel(RE::IMenu* a_menu, std::size_t a_menuIndex,
		std::uint32_t a_requirementIndex, const std::string& a_rowName, Value& a_panel)
	{
		std::array<Value, 2> panels;
		const auto panelCount = FindPanelsForSlot(a_menu, a_menuIndex, a_requirementIndex, panels);
		for (std::size_t i = 0; i < panelCount; ++i) {
			if (!PanelMatchesRow(panels[i], a_rowName)) {
				continue;
			}
			bool visible = true;
			if (ReadBool(panels[i], "visible", visible) && !visible) {
				continue;
			}
			a_panel = std::move(panels[i]);
			return true;
		}
		return false;
	}
	// Shows custom artwork in a crafting menu.
	void ShowCraftingArtwork(RE::IMenu* a_menu, std::size_t a_menuIndex, ArtworkPlan& a_plan)
	{
		if (!ArtworkMatchesSelection(a_menu, a_plan)) {
			ResetArtworkSlots(a_plan);
			return;
		}

		Value rows;
		std::uint32_t selected = 0;
		if (!GetSelectedRows(a_menu, selected, rows)) {
			ResetArtworkSlots(a_plan);
			return;
		}

		bool presented = false;
		for (std::size_t i = 0; i < a_plan.count; ++i) {
			auto& slot = a_plan.slots[i];
			if (!slot.armed || slot.path.empty()) {
				continue;
			}

			Value row;
			std::string rowName;
			Value panel;
			Value loader;
			if (!ReadElement(rows, slot.index, row) || !ReadText(row, "perkName", rowName) ||
				!FindVisibleCraftingPanel(a_menu, a_menuIndex, slot.index, rowName, panel) ||
				!FindLoader(panel, loader)) {
				slot.armed = false;
				continue;
			}

			std::string currentPath;
			const bool alreadyRequested = ReadText(loader, kArtworkPathKey.data(), currentPath) &&
				currentPath == slot.path;
			if (!alreadyRequested) {
				if (!EnsureNumber(loader, "clipScale", 0.39)) {
					slot.armed = false;
					WarnArtworkFailure(slot.displayForm, slot.path);
					continue;
				}
				const Value argument(slot.path.c_str());
				if (!loader.Invoke("SWFLoad", nullptr, &argument, 1)) {
					loader.SetMember(kArtworkPathKey.data(), Value());
					WarnArtworkFailure(slot.displayForm, slot.path);
					continue;
				}
				loader.SetMember(kArtworkPathKey.data(), Value(slot.path.c_str()));
			}
			SetBool(loader, "visible", true);
			presented = true;
		}
		if (presented && a_menu && a_menu->menuObj.IsAnyObject()) {
			a_menu->menuObj.SetMember("__PCFPanelScanStable", Value(false));
		}
	}
	// Hides the game's artwork in a requirement panel.
	bool HidePanelArtwork(Value& a_panel, ArtworkSearchResult& a_stats)
	{
		Value loader;
		if (!FindLoader(a_panel, loader)) {
			return false;
		}
		a_stats.foundPanel = true;
		SetBool(loader, "visible", false);
		return true;
	}
	// Searches the menu and hides the game's artwork.
	void HideArtworkInMenu(Value& a_root, ArtworkSearchResult& a_stats)
	{
		if (!a_root.IsAnyObject()) {
			return;
		}

		std::vector<PanelSearchItem> queue;
		queue.reserve(kPanelSearchLimit);
		queue.push_back({ a_root, 0 });
		std::size_t cursor = 0;
		std::uint32_t visited = 0;

		while (cursor < queue.size()) {
			if (visited >= kPanelSearchLimit) {
				a_stats.truncated = true;
				break;
			}

			auto node = std::move(queue[cursor++]);
			if (!node.object.IsAnyObject()) {
				continue;
			}
			++visited;

			if (HidePanelArtwork(node.object, a_stats) || node.depth >= kPanelSearchDepth) {
				continue;
			}

			std::uint32_t childCount = 0;
			if (!ReadIndex(node.object, "numChildren", childCount)) {
				continue;
			}
			const auto childrenToVisit = (std::min)(childCount, kPanelSearchChildren);
			if (childCount > childrenToVisit) {
				a_stats.truncated = true;
			}

			for (std::uint32_t i = 0; i < childrenToVisit; ++i) {
				if (queue.size() >= kPanelSearchLimit) {
					a_stats.truncated = true;
					break;
				}
				Value child;
				const Value argument(static_cast<double>(i));
				if (!node.object.Invoke("getChildAt", &child, &argument, 1) || !child.IsAnyObject()) {
					continue;
				}
				queue.push_back({ std::move(child), node.depth + 1 });
			}
		}

		if (cursor < queue.size()) {
			a_stats.truncated = true;
		}
	}
	// Hides original artwork while PCF shows custom requirements.
	void HideOriginalArtwork(RE::IMenu* a_menu, bool a_cache)
	{
		if (!a_menu || !a_menu->menuObj.IsAnyObject()) {
			return;
		}
		ArtworkSearchResult stats;

		std::uint32_t rootChildren = 0;
		std::uint32_t cachedRootChildren = 0;
		bool stable = false;
		const bool hasRootChildren = ReadIndex(a_menu->menuObj, "numChildren", rootChildren);
		const bool hasCachedRootChildren = ReadIndex(a_menu->menuObj, "__PCFPanelRootChildren", cachedRootChildren);
		const bool hasStableState = ReadBool(a_menu->menuObj, "__PCFPanelScanStable", stable);
		if (hasStableState && stable && hasRootChildren && hasCachedRootChildren && rootChildren == cachedRootChildren) {
			return;
		}

		if (hasStableState && stable) {
			a_menu->menuObj.SetMember("__PCFPanelScanStable", Value(false));
		}

		HideArtworkInMenu(a_menu->menuObj, stats);
		const bool canCache = a_cache && hasRootChildren && !stats.truncated && stats.foundPanel;
		if (canCache) {
			a_menu->menuObj.SetMember("__PCFPanelRootChildren", Value(static_cast<double>(rootChildren)));
			a_menu->menuObj.SetMember("__PCFPanelScanStable", Value(true));
		}
	}
	// Updates one non-Workshop crafting menu.
	void UpdateMenu(RE::IMenu* a_menu, std::size_t a_index, ArtworkPlan* a_artworkPlan = nullptr)
	{
		if (!a_menu || (PCF::RuleRegistry::Empty() && !PCF::CustomConditions::HasCraftingTargets())) {
			return;
		}
		const bool ready = UpdateCraftingRows(a_menu, a_artworkPlan, a_index);
		if (!ready && a_menu->hasDoneFirstAdvanceMovie && !PCF::UIHookState::GetHooks()[a_index].warned) {
			PCF::UIHookState::GetHooks()[a_index].warned = true;
			spdlog::warn("{}: native requirement data unavailable; preserving the current UI", PCF::UIHookState::GetHooks()[a_index].name);
		}
	}
	// Handles a crafting menu update.
	template <std::size_t Index>
	void CraftingMenuCallHook(RE::IMenu* a_menu, const Params& a_params)
	{
		auto& hook = PCF::UIHookState::GetHooks()[Index];
		if (!hook.call) {
			return;
		}
		if (!hook.active) {
			hook.call(a_menu, a_params);
			return;
		}
		const DepthGuard guard(g_callDepth);
		if (!guard.IsOutermost()) {
			hook.call(a_menu, a_params);
			return;
		}
		const bool rulesActive = !PCF::RuleRegistry::Empty() || PCF::CustomConditions::HasCraftingTargets();
		ArtworkPlan artworkPlan;
		ArtworkPlan ownershipPlan;
		const bool supportsArtwork = Index < kCraftingMenuCount;
		const bool artworkActive = supportsArtwork && (rulesActive || PCF::RuleRegistry::HasSWFAssignments());
		const bool artwork = artworkActive && BuildArtworkPlan(a_menu, artworkPlan);
		if (rulesActive) {
			HideOriginalArtwork(a_menu, false);
		}
		if constexpr (Index == kExamineIndex) {
			if (artwork) {
				PrepareExamineArtwork(a_menu, artworkPlan, true);
				ClearUnusedExamineArtwork(a_menu, std::addressof(artworkPlan));
			} else {
				ClearUnusedExamineArtwork(a_menu, nullptr);
			}
		} else if constexpr (Index < kCraftingMenuCount) {
			if (artwork) {
				PrepareCraftingArtwork(a_menu, artworkPlan, Index, true);
				ClearUnusedCraftingArtwork(a_menu, Index, std::addressof(artworkPlan));
			} else {
				ClearUnusedCraftingArtwork(a_menu, Index, nullptr);
			}
		}
		PCF::UIHookState::GetHooks()[Index].call(a_menu, a_params);
		if constexpr (Index == kExamineIndex) {
			if (artwork) {
				CheckExamineArtwork(a_menu, artworkPlan);
			}
		} else if constexpr (Index < kCraftingMenuCount) {
			if (artwork) {
				CheckCraftingArtwork(a_menu, artworkPlan, Index);
			}
		}
		if (rulesActive) {
			UpdateMenu(a_menu, Index, artwork ? std::addressof(artworkPlan) : nullptr);
			HideOriginalArtwork(a_menu, true);
			if constexpr (Index > kExamineIndex && Index < kCraftingMenuCount) {
				const ArtworkPlan* ownership = artwork ? std::addressof(artworkPlan) : nullptr;
				if (!ownership && BuildArtworkPlan(a_menu, ownershipPlan)) {
					ownership = std::addressof(ownershipPlan);
				}
				RestoreOriginalCraftingArtwork(a_menu, Index, ownership);
			}
		}
		if constexpr (Index == kExamineIndex) {
			if (artwork) {
				ShowExamineArtwork(a_menu, artworkPlan);
			}
		} else if constexpr (Index < kCraftingMenuCount) {
			if (artwork) {
				ShowCraftingArtwork(a_menu, Index, artworkPlan);
			}
		}
	}

	// Performs the final CookingMenu custom-panel write immediately before the
	// frame is presented. Native Scaleform code may format perk rank text after
	// the Call hook returns; writing here prevents visible `1` / `Rank 1` races.
	void CookingPreDisplayHook(RE::IMenu* a_menu)
	{
		auto& hook = PCF::UIHookState::GetHooks()[kCookingIndex];
		if (!hook.preDisplay) {
			return;
		}
		if (!hook.active) {
			hook.preDisplay(a_menu);
			return;
		}
		const DepthGuard guard(g_cookingPreDisplayDepth);
		if (!guard.IsOutermost()) {
			hook.preDisplay(a_menu);
			return;
		}
		hook.preDisplay(a_menu);
		ReassertSelectedCustomConditionPanels(a_menu, kCookingIndex);
	}

	// Installs and verifies the update hook for a crafting menu.
	template <std::size_t Index>
	bool InstallMenuHook(std::uintptr_t a_vtable)
	{
		auto& hook = PCF::UIHookState::GetHooks()[Index];
		const auto replacement = reinterpret_cast<std::uintptr_t>(&CraftingMenuCallHook<Index>);
		const auto current = PCF::NativeHooks::ReadVtableSlot(a_vtable, kCallSlot);
		if (current == replacement) {
			return hook.call != nullptr;
		}
		if (!current) {
			return false;
		}
		REL::Relocation<std::uintptr_t> vtable{ a_vtable };
		const auto previous = vtable.WriteVirtualCall(kCallSlot, CraftingMenuCallHook<Index>);
		hook.call = reinterpret_cast<CallFunction>(previous);
		return hook.call != nullptr && PCF::NativeHooks::IsVtableSlotSet(a_vtable, kCallSlot, replacement);
	}

	// Installs the CookingMenu PreDisplay hook used for final-frame ownership.
	bool InstallCookingPreDisplayHook(std::uintptr_t a_vtable)
	{
		auto& hook = PCF::UIHookState::GetHooks()[kCookingIndex];
		const auto replacement = reinterpret_cast<std::uintptr_t>(&CookingPreDisplayHook);
		const auto current = PCF::NativeHooks::ReadVtableSlot(a_vtable, kPreDisplaySlot);
		if (current == replacement) {
			return hook.preDisplay != nullptr;
		}
		if (!current) {
			return false;
		}
		REL::Relocation<std::uintptr_t> vtable{ a_vtable };
		const auto previous = vtable.WriteVirtualCall(kPreDisplaySlot, CookingPreDisplayHook);
		hook.preDisplay = reinterpret_cast<PCF::UIHookState::PreDisplayFunction>(previous);
		return hook.preDisplay != nullptr &&
			PCF::NativeHooks::IsVtableSlotSet(a_vtable, kPreDisplaySlot, replacement);
	}
}

namespace PCF::CraftingMenus
{
	// Installs hooks for the four non-Workshop crafting menus.
	bool Install(const std::array<std::uintptr_t, 4>& a_vtables)
	{
		return InstallMenuHook<kExamineIndex>(a_vtables[kExamineIndex]) &&
			InstallMenuHook<kCookingIndex>(a_vtables[kCookingIndex]) &&
			InstallMenuHook<kPowerArmorIndex>(a_vtables[kPowerArmorIndex]) &&
			InstallMenuHook<kRobotIndex>(a_vtables[kRobotIndex]) &&
			InstallCookingPreDisplayHook(a_vtables[kCookingIndex]);
	}
}
