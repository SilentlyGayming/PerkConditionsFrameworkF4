// Perk Conditions Framework
// SilentlyGayming
// SourcePerkNames.h

#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace RE
{
	class BGSPerk;
}

namespace PCF::SourcePerkNames
{
	struct Result
	{
		std::unordered_map<RE::BGSPerk*, std::string> names;
	};

	[[nodiscard]] Result LoadNames(const std::vector<RE::BGSPerk*>& a_sources);
}
