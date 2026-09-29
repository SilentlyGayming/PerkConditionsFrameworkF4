// Perk Conditions Framework
// SilentlyGayming
// EngineIDs.h

#pragma once

#include "F4SE/F4SE.hpp"
#include "REL/Relocation.hpp"

namespace PCF::EngineIDs
{
	inline constexpr F4SE::VariantId BGSMessageGetConvertedDescription{ REL::INVALID_ID, 2203353, 2203353 };
	inline constexpr F4SE::VariantId TESDescriptionGetDescription{ REL::INVALID_ID, 2193019, 2193019 };
	inline constexpr F4SE::VariantId PipboyPerksMenuUpdateData{ REL::INVALID_ID, 2224224, 2224224 };
	inline constexpr F4SE::VariantId TESTopicInfoLoad{ REL::INVALID_ID, 2208420, 2208420 };
	inline constexpr F4SE::VariantId TESTopicInfoInitItemImpl{ REL::INVALID_ID, 2208421, 2208421 };
	inline constexpr F4SE::VariantId LocalizedSubrecordLoad{ REL::INVALID_ID, 2194235, 2194235 };
	inline constexpr F4SE::VariantId TESResponseTextLoad{ REL::INVALID_ID, 2208286, 2208286 };
	inline constexpr F4SE::VariantId DialoguePromptFallback{ REL::INVALID_ID, 2208435, 2208435 };
	inline constexpr F4SE::VariantId DialoguePromptGetter{ REL::INVALID_ID, 2208446, 2208446 };
	inline constexpr F4SE::VariantId DialoguePromptSetter{ REL::INVALID_ID, 2208447, 2208447 };
	inline constexpr F4SE::VariantId DialoguePromptInsert{ REL::INVALID_ID, 2208487, 2208487 };
	inline constexpr F4SE::VariantId WorkshopPublishRequirements{ REL::INVALID_ID, 2225003, 2225003 };
	inline constexpr F4SE::VariantId WorkshopAppendPerkRow{ REL::INVALID_ID, 2225058, 2225058 };
	inline constexpr F4SE::VariantId TESConditionIsTrue{ REL::INVALID_ID, 2211989, 2211989 };
	inline constexpr F4SE::VariantId TESConditionIsTrueContext{ REL::INVALID_ID, 2211990, 2211990 };
	inline constexpr F4SE::VariantId TESConditionIsTrueForAllButFunction{ REL::INVALID_ID, 2211991, 2211991 };
	inline constexpr F4SE::VariantId WorkbenchChoiceRequirements{ REL::INVALID_ID, 2223051, 2223051 };
	inline constexpr F4SE::VariantId GFxSetMember{ REL::INVALID_ID, 2286589, 2286589 };
}
