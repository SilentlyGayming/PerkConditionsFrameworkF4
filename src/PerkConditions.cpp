// Perk Conditions Framework
// SilentlyGayming
// PerkConditions.cpp

#include "PCH.h"
#include "PerkConditions.h"
#include "RuleRegistry.h"

#include <cmath>
#include <memory>

namespace
{
	using ConditionFunction = RE::SCRIPT_FUNCTION::ConditionFunctionType;
	ConditionFunction* g_hasPerk{ nullptr };
	ConditionFunction* g_dialogueHasPerk{ nullptr };
	bool g_installed{ false };
	// Checks a numeric condition.
	bool CheckBoolean(float a_value, RE::ENUM_COMPARISON_CONDITION a_condition, float a_compare)
	{
		switch (a_condition) {
		case RE::ENUM_COMPARISON_CONDITION::kEqual:
			return a_value == a_compare;
		case RE::ENUM_COMPARISON_CONDITION::kNotEqual:
			return a_value != a_compare;
		case RE::ENUM_COMPARISON_CONDITION::kGreaterThan:
			return a_value > a_compare;
		case RE::ENUM_COMPARISON_CONDITION::kGreaterThanEqual:
			return a_value >= a_compare;
		case RE::ENUM_COMPARISON_CONDITION::kLessThan:
			return a_value < a_compare;
		case RE::ENUM_COMPARISON_CONDITION::kLessThanEqual:
			return a_value <= a_compare;
		default:
			return false;
		}
	}
	// Gets the actor used by a condition check.
	RE::Actor* GetConditionActor(const RE::ConditionCheckParams& a_params)
	{
		// Both game callbacks use actionRef (1.11.240: RVA 0x5A3D4E).
		auto* reference = a_params.actionRef;
		return reference ? reference->As<RE::Actor>() : nullptr;
	}
	// Checks one configured replacement requirement.
	bool MeetsAlternative(ConditionFunction* a_original, RE::ConditionCheckParams a_params,
		void* a_param1, const PCF::PerkAlternative& a_alternative,
		RE::Actor*& a_actor, bool& a_actorChecked)
	{
		auto actor = [&]() {
			if (!a_actorChecked) {
				a_actor = GetConditionActor(a_params);
				a_actorChecked = true;
			}
			return a_actor;
		};
		switch (a_alternative.type) {
		case PCF::AlternativeType::kActorValue: {
			auto* currentActor = actor();
			if (!currentActor || !a_alternative.actorValue) {
				return false;
			}
			const auto value = currentActor->GetActorValue(*a_alternative.actorValue);
			return std::isfinite(value) && PCF::CheckComparison(value, a_alternative.comparison, a_alternative.requiredValue);
		}
		case PCF::AlternativeType::kGlobalValue: {
			if (!a_alternative.globalValue) {
				return false;
			}
			const auto value = a_alternative.globalValue->GetValue();
			return std::isfinite(value) && PCF::CheckComparison(value, a_alternative.comparison, a_alternative.requiredValue);
		}
		case PCF::AlternativeType::kPerk:
			break;
		}

		auto* currentActor = actor();
		auto* perk = a_alternative.foundRank ? a_alternative.foundRank.perk : a_alternative.perk;
		if (!perk) {
			return false;
		}
		float value = 0.0F;
		if (!a_original(a_params, perk, a_param1, value) || value == 0.0F) {
			return false;
		}
		if (a_alternative.foundRank.kind == PCF::PerkRankType::kInternal) {
			return currentActor && a_alternative.perk && currentActor->GetPerkRank(a_alternative.perk) >= a_alternative.rank;
		}
		if (a_alternative.foundRank) {
			return true;
		}
		return currentActor && a_alternative.perk && currentActor->GetPerkRank(a_alternative.perk) >= a_alternative.rank;
	}
	// Combines the game perk result with the configured rule.
	bool Evaluate(ConditionFunction* a_original, RE::ConditionCheckParams& a_params,
		void* a_perk, void* a_param1, float& a_value)
	{
		if (!a_original) {
			return false;
		}

		auto* source = static_cast<RE::BGSPerk*>(a_perk);
		const auto* rule = PCF::RuleRegistry::Find(source);
		if (!rule) {
			return a_original(a_params, a_perk, a_param1, a_value);
		}

		bool nativeSatisfied = false;
		if (rule->mode == PCF::RuleMode::kOr) {
			if (!a_original(a_params, a_perk, a_param1, a_value)) {
				return false;
			}
			nativeSatisfied = a_value != 0.0F;
			if (nativeSatisfied) {
				return true;
			}
		}

		RE::Actor* actor = nullptr;
		bool actorChecked = false;
		bool alternativeSatisfied = false;
		for (const auto& alternative : rule->alternatives) {
			if (MeetsAlternative(a_original, a_params, a_param1, alternative, actor, actorChecked)) {
				alternativeSatisfied = true;
				break;
			}
		}
		a_value = PCF::CombineResults(nativeSatisfied, alternativeSatisfied, rule->mode) ? 1.0F : 0.0F;
		return true;
	}
	// Handles game HasPerk checks.
	bool HasPerk(RE::ConditionCheckParams& a_params, void* a_perk, void* a_param1, float& a_value)
	{
		return Evaluate(g_hasPerk, a_params, a_perk, a_param1, a_value);
	}
	// Handles game DialogueHasPerk checks.
	bool DialogueHasPerk(RE::ConditionCheckParams& a_params, void* a_perk, void* a_param1, float& a_value)
	{
		return Evaluate(g_dialogueHasPerk, a_params, a_perk, a_param1, a_value);
	}
}

namespace PCF::PerkConditions
{
	// Gets the perk used by a supported condition.
	RE::BGSPerk* GetConditionPerk(RE::TESConditionItem* a_item)
	{
		if (!a_item || a_item->data.aliasParams || a_item->data.packDataParams) {
			return nullptr;
		}
		const auto function = static_cast<std::uint32_t>(a_item->data.functionData.function.get());
		const auto hasPerk = static_cast<std::uint32_t>(RE::SCRIPT_OUTPUT::kScript_HasPerk) +
			static_cast<std::uint32_t>(RE::SCRIPT_OUTPUT::kScript_Offset);
		const auto dialogueHasPerk = static_cast<std::uint32_t>(RE::SCRIPT_OUTPUT::kScript_DialogueHasPerk) +
			static_cast<std::uint32_t>(RE::SCRIPT_OUTPUT::kScript_Offset);
		if (function != hasPerk && function != hasPerk - 0x1000 &&
			function != dialogueHasPerk && function != dialogueHasPerk - 0x1000) {
			return nullptr;
		}
		auto* form = static_cast<RE::TESForm*>(a_item->data.functionData.param[0]);
		return form ? form->As<RE::BGSPerk>() : nullptr;
	}
	// Checks whether a condition requires the perk.
	bool IsPositivePerkCheck(RE::TESConditionItem* a_item)
	{
		if (!GetConditionPerk(a_item)) {
			return false;
		}
		const auto compare = a_item->GetComparisonValue();
		return std::isfinite(compare) && !CheckBoolean(0.0F, static_cast<RE::ENUM_COMPARISON_CONDITION>(a_item->data.condition), compare) &&
			CheckBoolean(1.0F, static_cast<RE::ENUM_COMPARISON_CONDITION>(a_item->data.condition), compare);
	}
	// Gets the player's current numeric rank for a perk family.
	float GetPlayerPerkRank(RE::PlayerCharacter* a_player, RE::BGSPerk* a_perk)
	{
		if (!a_player || !a_perk) {
			return 0.0F;
		}
		const auto indexed = PCF::RuleRegistry::GetRank(a_perk);
		if (!indexed.base || (indexed.base == a_perk && !indexed.base->nextPerk)) {
			return static_cast<float>(a_player->GetPerkRank(a_perk));
		}

		std::uint32_t highest = 0;
		for (std::uint32_t rank = 1; rank <= 255; ++rank) {
			const auto resolved = PCF::RuleRegistry::FindPerkRank(indexed.base, rank);
			if (!resolved || resolved.kind == PCF::PerkRankType::kInternal) {
				break;
			}
			if (a_player->GetPerkRank(resolved.perk) != 0) {
				highest = rank;
			}
		}
		return static_cast<float>(highest);
	}


	// Checks whether the player passes a replaced perk requirement.
	bool PlayerPasses(RE::BGSPerk* a_source)
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player || !a_source) {
			return false;
		}
		RE::ConditionCheckParams params;
		params.actionRef = player;
		params.targetRef = player;
		params.scopeActor = player;
		float value = 0.0F;
		return HasPerk(params, a_source, nullptr, value) && value != 0.0F;
	}
	// Installs the perk condition hooks.
	bool Install()
	{
		if (g_installed) {
			return true;
		}

		RE::SCRIPT_FUNCTION* hasPerk = nullptr;
		RE::SCRIPT_FUNCTION* dialogueHasPerk = nullptr;
		for (auto& function : RE::SCRIPT_FUNCTION::GetScriptFunctions()) {
			if (!function.functionName) {
				continue;
			}
			const std::string_view name(function.functionName);
			if (name == "HasPerk") {
				hasPerk = std::addressof(function);
			} else if (name == "DialogueHasPerk") {
				dialogueHasPerk = std::addressof(function);
			}
		}
		if (!hasPerk || !dialogueHasPerk || !hasPerk->conditionFunction || !dialogueHasPerk->conditionFunction) {
			spdlog::error("Perk condition callbacks unavailable");
			return false;
		}

		g_hasPerk = hasPerk->conditionFunction;
		g_dialogueHasPerk = dialogueHasPerk->conditionFunction;
		hasPerk->conditionFunction = HasPerk;
		dialogueHasPerk->conditionFunction = DialogueHasPerk;
		g_installed = true;
		return true;
	}
}
