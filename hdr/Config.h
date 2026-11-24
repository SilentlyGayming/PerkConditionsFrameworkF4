// Perk Conditions Framework
// SilentlyGayming
// Config.h

#pragma once

#include "RuleTypes.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace PCF
{
	enum class CustomConditionType : std::uint8_t
	{
		kFormValue,
		kQuestStage,
		kQuestStageDone
	};

	struct RawCustomCondition
	{
		CustomConditionType type{ CustomConditionType::kFormValue };
		ComparisonOp comparison{ ComparisonOp::kGreaterEqual };
		std::string targetPlugin;
		std::string targetReference;
		std::string conditionPlugin;
		std::string conditionReference;
		float requiredValue{ 0.0F };
		bool orWithNext{ false };
		std::filesystem::path file;
		std::size_t line{ 0 };
	};

	struct RawPerkRule
	{
		AlternativeType type{ AlternativeType::kPerk };
		ComparisonOp comparison{ ComparisonOp::kGreaterEqual };
		RuleMode mode{ RuleMode::kReplace };
		DisplayMode display{ DisplayMode::kCustom };
		std::string sourcePlugin;
		std::string sourceReference;
		std::string alternativePlugin;
		std::string alternativeReference;
		std::uint32_t rank{ 1 };
		float requiredValue{ 0.0F };
		std::filesystem::path file;
		std::size_t line{ 0 };
	};

	struct RawDescriptionRule
	{
		ComparisonOp comparison{ ComparisonOp::kEqual };
		std::string sourcePlugin;
		std::string sourceReference;
		std::string conditionPlugin;
		std::string conditionReference;
		float requiredValue{ 0.0F };
		std::string description;
		std::filesystem::path file;
		std::size_t line{ 0 };
	};

	struct RawActorValueDescriptionAssignment
	{
		std::string plugin;
		std::string formReference;
		std::string description;
		std::filesystem::path file;
		std::size_t line{ 0 };
	};

	struct RawRequirementLabel
	{
		std::string targetPlugin;
		std::string targetReference;
		std::string conditionPlugin;
		std::string conditionReference;
		std::string label;
		std::filesystem::path file;
		std::size_t line{ 0 };
	};

	struct RawNameAssignment
	{
		std::string plugin;
		std::string formReference;
		std::string name;
		std::filesystem::path file;
		std::size_t line{ 0 };
	};

	struct RawSWFAssignment
	{
		std::string plugin;
		std::string formReference;
		std::string path;
		std::filesystem::path file;
		std::size_t line{ 0 };
	};

	struct LoadedConfig
	{
		std::vector<RawPerkRule> rules;
		std::vector<RawCustomCondition> customConditions;
		std::vector<RawDescriptionRule> descriptionRules;
		std::vector<RawActorValueDescriptionAssignment> actorValueDescriptions;
		std::vector<RawRequirementLabel> requirementLabels;
		std::vector<RawNameAssignment> nameAssignments;
		std::vector<RawSWFAssignment> swfAssignments;
		std::size_t matchingFileCount{ 0 };
		std::size_t loadedFileCount{ 0 };
	};

	namespace Config
	{
		[[nodiscard]] LoadedConfig Load();
	}
}
