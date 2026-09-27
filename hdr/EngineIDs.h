// Perk Conditions Framework
// SilentlyGayming
// EngineIDs.h

#pragma once

#include "REL/Relocation.h"

namespace PCF::EngineIDs
{
	inline constexpr REL::ID BGSMessageGetConvertedDescription{ 8331, 2203353 };
	inline constexpr REL::ID TESDescriptionGetDescription{ 523613, 2193019 };
	inline constexpr REL::ID PipboyPerksMenuUpdateData{ 783380, 2224224 };
	inline constexpr REL::ID TESTopicInfoLoad{ 2208420 };
	inline constexpr REL::ID TESTopicInfoInitItemImpl{ 2208421 };
	inline constexpr REL::ID LocalizedSubrecordLoad{ 2194235 };
	inline constexpr REL::ID TESResponseTextLoad{ 2208286 };
	inline constexpr REL::ID DialoguePromptFallback{ 2208435 };
	inline constexpr REL::ID DialoguePromptGetter{ 2208446 };
	inline constexpr REL::ID DialoguePromptSetter{ 2208447 };
	inline constexpr REL::ID DialoguePromptInsert{ 2208487 };
	inline constexpr REL::ID WorkshopPublishRequirements{ 2225003 };
	inline constexpr REL::ID WorkshopAppendPerkRow{ 2225058 };
	inline constexpr REL::ID TESConditionIsTrue{ 1275731, 2211989 };
	inline constexpr REL::ID TESConditionIsTrueContext{ 2211990 };
	inline constexpr REL::ID TESConditionIsTrueForAllButFunction{ 1182457, 2211991 };
	inline constexpr REL::ID WorkbenchChoiceRequirements{ 2223051 };
	inline constexpr REL::ID GFxSetMember{ 1360149, 2286589 };
}
