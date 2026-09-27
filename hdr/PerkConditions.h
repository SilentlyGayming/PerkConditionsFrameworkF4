// Perk Conditions Framework
// SilentlyGayming
// PerkConditions.h

#pragma once

namespace RE
{
	class BGSPerk;
	class PlayerCharacter;
	class TESConditionItem;
}

namespace PCF::PerkConditions
{
	[[nodiscard]] bool Install();
	[[nodiscard]] RE::BGSPerk* GetConditionPerk(RE::TESConditionItem* a_item);
	[[nodiscard]] bool IsPositivePerkCheck(RE::TESConditionItem* a_item);
	[[nodiscard]] float GetPlayerPerkRank(RE::PlayerCharacter* a_player, RE::BGSPerk* a_perk);
	[[nodiscard]] bool PlayerPasses(RE::BGSPerk* a_source);
}
