// Perk Conditions Framework
// SilentlyGayming
// DialogueText.h

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace RE
{
	class TESTopicInfo;
}

namespace PCF::DialogueText
{
	struct Result
	{
		std::uint32_t dialogueChoicesChanged{ 0 };
		std::uint32_t topicNamesChanged{ 0 };
		std::uint32_t infoDirectChanges{ 0 };
		std::uint32_t infoFallbackChanges{ 0 };
		std::uint32_t infoResponseChanges{ 0 };
	};

	[[nodiscard]] std::vector<RE::TESTopicInfo*> CollectLoadedInfoForStartup();
	[[nodiscard]] Result UpdateTopics(std::vector<std::string>& a_topicLogLines);
	void FinishInfoStartup(Result& a_result, std::vector<std::string>& a_infoLogLines,
		const std::vector<RE::TESTopicInfo*>* a_infos = nullptr);
	void InstallEarlyHooks();
	void DisableInfoText();
}
