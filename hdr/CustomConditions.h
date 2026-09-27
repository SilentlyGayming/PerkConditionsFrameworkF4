// Perk Conditions Framework
// SilentlyGayming
// CustomConditions.h

#pragma once

#include "RuleTypes.h"
#include "TextManager.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace RE
{
	class TESCondition;
	class TESForm;
	class TESQuest;
}

namespace PCF::CustomConditions
{
	enum class OwnerKind : std::uint8_t
	{
		kCrafting,
		kDialogue
	};

	enum class ConditionKind : std::uint8_t
	{
		kFormValue,
		kQuestStage,
		kQuestStageDone
	};

	struct Condition
	{
		ConditionKind kind{ ConditionKind::kFormValue };
		PerkAlternative value;
		RE::TESQuest* quest{ nullptr };
		std::uint16_t questStage{ 0 };
		bool orWithNext{ false };
		TextManager::RequirementPresentation presentation;
	};

	struct ConditionSet
	{
		RE::TESForm* target{ nullptr };
		RE::TESCondition* nativeConditions{ nullptr };
		OwnerKind owner{ OwnerKind::kCrafting };
		std::vector<Condition> conditions;
		std::string dialoguePrefix;
		bool nativeListEmpty{ false };
		bool active{ true };
	};

	void Clear();
	[[nodiscard]] bool Add(RE::TESForm* a_target, std::vector<Condition> a_conditions);
	void Finalize();
	[[nodiscard]] bool Install();
	[[nodiscard]] bool Empty();
	[[nodiscard]] bool HasCraftingTargets();
	[[nodiscard]] bool HasDialogueTargets();
	[[nodiscard]] std::size_t TargetCount();
	[[nodiscard]] std::size_t ConditionCount();
	[[nodiscard]] std::size_t CraftingTargetCount();
	[[nodiscard]] std::size_t DialogueTargetCount();
	[[nodiscard]] std::size_t QuestConditionCount();
	[[nodiscard]] const ConditionSet* Find(const RE::TESForm* a_target);
	[[nodiscard]] const ConditionSet* Find(const RE::TESCondition* a_conditions);
	[[nodiscard]] RE::TESForm* GetValueForm(const Condition& a_condition);
	[[nodiscard]] RE::TESForm* GetDisplayForm(const Condition& a_condition);
	[[nodiscard]] const std::string* GetArtworkPath(const Condition& a_condition);
	// Quest progression is an internal gameplay gate and is never player-facing UI.
	[[nodiscard]] bool IsPlayerFacing(const Condition& a_condition);
	[[nodiscard]] bool Evaluate(const Condition& a_condition);
	[[nodiscard]] bool Evaluate(const ConditionSet& a_set);
}
