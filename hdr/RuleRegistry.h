// Perk Conditions Framework
// SilentlyGayming
// RuleRegistry.h

#pragma once

#include "RuleTypes.h"

namespace RE
{
	class TESDescription;
	class TESForm;
	class TESTopicInfo;
}

namespace PCF::RuleRegistry
{
	void Load();
	void FinalizeCustomConditions(const std::vector<RE::TESTopicInfo*>& a_infos);
	[[nodiscard]] bool HasPendingCustomConditions();
	[[nodiscard]] bool CustomConditionsFinalized();
	[[nodiscard]] bool Empty();
	[[nodiscard]] bool HasSWFAssignments();
	[[nodiscard]] bool HasDescriptionRules();
	[[nodiscard]] bool HasPerkDescriptionRules();
	[[nodiscard]] const std::vector<RE::BGSPerk*>& Sources();
	[[nodiscard]] const PerkRule* Find(RE::BGSPerk* a_perk);
	[[nodiscard]] const std::string* FindSWF(RE::TESForm* a_form);
	[[nodiscard]] const std::string* GetDisplayName(RE::TESForm* a_form);
	[[nodiscard]] const std::vector<DescriptionRule>* GetDescriptions(RE::BGSPerk* a_perk);
	[[nodiscard]] const std::vector<RE::BGSPerk*>& GetDescriptionSources();
	[[nodiscard]] RE::BGSPerk* FindDescriptionPerk(const RE::TESDescription* a_description);
	[[nodiscard]] const std::string* FindActorValueDescription(const RE::TESDescription* a_description);
	[[nodiscard]] bool HasRequirementLabels();
	[[nodiscard]] bool HasRequirementLabels(const RE::TESForm* a_target);
	[[nodiscard]] const std::string* FindRequirementLabel(const RE::TESForm* a_target, const RE::TESForm* a_conditionForm);
	[[nodiscard]] const PerkAlternative* GetDisplay(const PerkRule& a_rule);
	[[nodiscard]] PerkRankResult FindPerkRank(RE::BGSPerk* a_base, std::uint32_t a_rank);
	[[nodiscard]] PerkRank GetRank(RE::BGSPerk* a_perk);
}
