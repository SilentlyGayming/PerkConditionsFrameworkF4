// Perk Conditions Framework
// SilentlyGayming
// TextManager.h

#pragma once

#include "RuleTypes.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace RE
{
	class BGSPerk;
	class TESConditionItem;
	class TESForm;
}

namespace PCF::TextManager
{
	struct RequirementIdentity
	{
		AlternativeType type{ AlternativeType::kPerk };
		ComparisonOp comparison{ ComparisonOp::kGreaterEqual };
		const void* target{ nullptr };
		float equalityValue{ 0.0F };

		// Checks whether two requirements represent the same thing.
		[[nodiscard]] bool operator==(const RequirementIdentity& a_right) const noexcept
		{
			if (type != a_right.type || target != a_right.target) {
				return false;
			}
			if (type == AlternativeType::kPerk) {
				return true;
			}
			return comparison == a_right.comparison &&
				(comparison != ComparisonOp::kEqual || equalityValue == a_right.equalityValue);
		}
	};

	struct RequirementPresentation
	{
		RequirementIdentity identity;
		std::string label;
		std::string fullLabel;
		std::string rowLabel;
		std::string valueText;
		double value{ 0.0 };
		double strength{ 0.0 };
		bool overrideNativeText{ true };
	};

	struct Result
	{
		std::uint32_t aliases{ 0 };
		std::uint32_t messageButtonsChanged{ 0 };
	};

	[[nodiscard]] RequirementPresentation BuildRequirementPresentation(const PerkAlternative& a_display);
	[[nodiscard]] bool ApplyRequirementLabel(const RE::TESForm* a_target, const RE::TESForm* a_conditionForm, RequirementPresentation& a_presentation);
	[[nodiscard]] const RequirementPresentation* GetConfiguredRequirement(RE::BGSPerk* a_source);
	[[nodiscard]] std::optional<std::string> RewriteText(std::string_view a_original,
		std::span<const std::uintptr_t> a_preferredSources = {}, bool a_genericBareReplacement = false,
		bool a_restrictToPreferred = false);
	[[nodiscard]] Result UpdateMessages(std::vector<std::string>& a_messageLogLines);
	[[nodiscard]] bool Install();
	[[nodiscard]] bool InstallRequirementLabels();
}
