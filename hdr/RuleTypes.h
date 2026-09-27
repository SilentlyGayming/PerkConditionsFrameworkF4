// Perk Conditions Framework
// SilentlyGayming
// RuleTypes.h

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace RE
{
	class ActorValueInfo;
	class BGSPerk;
	class TESGlobal;
}

namespace PCF
{
	enum class AlternativeType : std::uint8_t
	{
		kPerk,
		kActorValue,
		kGlobalValue
	};

	enum class ComparisonOp : std::uint8_t
	{
		kEqual,
		kGreater,
		kLess,
		kGreaterEqual,
		kLessEqual
	};

	enum class RuleMode : std::uint8_t
	{
		kReplace,
		kOr
	};

	enum class DisplayMode : std::uint8_t
	{
		kOriginal,
		kCustom
	};

	// Checks one configured number comparison.
	[[nodiscard]] constexpr bool CheckComparison(float a_value, ComparisonOp a_comparison, float a_required) noexcept
	{
		switch (a_comparison) {
		case ComparisonOp::kEqual:
			return a_value == a_required;
		case ComparisonOp::kGreater:
			return a_value > a_required;
		case ComparisonOp::kLess:
			return a_value < a_required;
		case ComparisonOp::kGreaterEqual:
			return a_value >= a_required;
		case ComparisonOp::kLessEqual:
			return a_value <= a_required;
		}
		return false;
	}

	// Combines the game result with the configured rule result.
	[[nodiscard]] constexpr bool CombineResults(bool a_native, bool a_alternative, RuleMode a_mode) noexcept
	{
		return a_mode == RuleMode::kOr ? a_native || a_alternative : a_alternative;
	}

	// Chooses which display mode the rule should use.
	[[nodiscard]] constexpr bool ChooseDisplayMode(RuleMode a_mode, bool a_specified, DisplayMode a_requested, DisplayMode& a_effective) noexcept
	{
		if (a_mode == RuleMode::kReplace) {
			if (a_specified && a_requested == DisplayMode::kOriginal) {
				return false;
			}
			a_effective = DisplayMode::kCustom;
			return true;
		}
		if (!a_specified) {
			return false;
		}
		a_effective = a_requested;
		return true;
	}

	enum class PerkRankType : std::uint8_t
	{
		kUnresolved,
		kInternal,
		kLinked,
		kFallback
	};

	struct PerkRankResult
	{
		RE::BGSPerk* perk{ nullptr };
		std::uint32_t rank{ 0 };
		PerkRankType kind{ PerkRankType::kUnresolved };

		// Checks whether a perk rank was found.
		[[nodiscard]] explicit operator bool() const noexcept
		{
			return perk != nullptr;
		}
	};

	struct PerkAlternative
	{
		AlternativeType type{ AlternativeType::kPerk };
		ComparisonOp comparison{ ComparisonOp::kGreaterEqual };
		RE::BGSPerk* perk{ nullptr };
		PerkRankResult foundRank;
		RE::ActorValueInfo* actorValue{ nullptr };
		RE::TESGlobal* globalValue{ nullptr };
		std::uint32_t rank{ 1 };
		float requiredValue{ 0.0F };
		std::string name;
		std::string iconPath;
	};

	struct PerkRule
	{
		std::vector<PerkAlternative> alternatives;
		RuleMode mode{ RuleMode::kReplace };
		DisplayMode display{ DisplayMode::kCustom };
	};

	struct DescriptionRule
	{
		AlternativeType type{ AlternativeType::kGlobalValue };
		ComparisonOp comparison{ ComparisonOp::kEqual };
		RE::BGSPerk* perk{ nullptr };
		RE::ActorValueInfo* actorValue{ nullptr };
		RE::TESGlobal* globalValue{ nullptr };
		float requiredValue{ 0.0F };
		std::string description;
	};

	struct PerkRank
	{
		RE::BGSPerk* base{ nullptr };
		std::uint32_t rank{ 0 };
	};
}
