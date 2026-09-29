// Perk Conditions Framework
// SilentlyGayming
// main.cpp

#include "PCH.h"
#include "PerkConditions.h"
#include "PerkDescriptions.h"
#include "RuleRegistry.h"
#include "TextManager.h"
#include "DialogueText.h"
#include "CustomConditions.h"
#include "UIManager.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace
{
	// Writes a startup log section header.
	void WriteLogHeader(std::string_view a_name)
	{
		const std::size_t width = 60 + (a_name.size() % 2);
		spdlog::info("{:*^{}}", a_name, width);
	}
	// Loads PCF state when game data is ready and finishes INFO text after the game is created or loaded.
	void OnGameReady(F4SE::MessagingInterface::Message* a_message)
	{
		static bool dataReady = false;
		static bool registryLoaded = false;
		static bool conditionsInstalled = false;
		static bool customConditionsInstalled = false;
		static bool descriptionsAttempted = false;
		static bool descriptionsInstalled = false;
		static bool textHooksInstalled = false;
		static bool requirementLabelsInstalled = false;
		static bool textApplied = false;
		static PCF::TextManager::Result textResult;
		static PCF::DialogueText::Result dialogueResult;
		static std::vector<std::string> messageLogLines;
		static std::vector<std::string> infoLogLines;
		static std::vector<std::string> topicLogLines;
		static bool uiInstalled = false;
		static bool finished = false;

		if (!a_message || finished) {
			return;
		}
		const bool gameDataReady = a_message->GetType() == F4SE::MessagingInterface::MessageType::kGameDataReady;
		const bool newGameReady = a_message->GetType() == F4SE::MessagingInterface::MessageType::kNewGame;
		const bool postLoadGame = a_message->GetType() == F4SE::MessagingInterface::MessageType::kPostLoadGame;
		if (postLoadGame && !a_message->GetData()) {
			return;
		}
		const bool gameStateReady = newGameReady || postLoadGame;
		if (gameDataReady) {
			if (!a_message->GetData()) {
				return;
			}
			dataReady = true;
		} else if (!dataReady || !gameStateReady) {
			return;
		}

		const auto report = [](spdlog::level::level_enum a_level, std::string_view a_message) {
			spdlog::log(a_level, "{}", a_message);
		};

		try {
			if (!registryLoaded) {
				WriteLogHeader("TOML");
				PCF::RuleRegistry::Load();
				registryLoaded = true;
			}
			const bool descriptionRulesActive = PCF::RuleRegistry::HasDescriptionRules();
			if (descriptionRulesActive && !descriptionsAttempted) {
				descriptionsAttempted = true;
				descriptionsInstalled = PCF::PerkDescriptions::Install();
				if (!descriptionsInstalled) {
					report(spdlog::level::err, "Description hooks unavailable; continuing without description replacement");
				}
			}
			const bool descriptionHooksActive = descriptionRulesActive && descriptionsInstalled;

			// Install the optional PRKF bridge only after the existing description hooks so it cannot
			// affect their established trampoline setup. The bridge itself uses private executable memory.
			const bool requirementLabelsActive = PCF::RuleRegistry::HasRequirementLabels();
			if (requirementLabelsActive && !requirementLabelsInstalled) {
				requirementLabelsInstalled = PCF::TextManager::InstallRequirementLabels();
				if (!requirementLabelsInstalled) {
					report(spdlog::level::warn, "PRKF requirement-label bridge unavailable; other PCF systems will continue normally");
				}
			}

			std::vector<RE::TESTopicInfo*> startupInfoSnapshot;
			const std::vector<RE::TESTopicInfo*>* startupInfos = nullptr;
			if (gameStateReady && !PCF::RuleRegistry::CustomConditionsFinalized()) {
				startupInfoSnapshot = PCF::DialogueText::CollectLoadedInfoForStartup();
				startupInfos = std::addressof(startupInfoSnapshot);
				PCF::RuleRegistry::FinalizeCustomConditions(startupInfoSnapshot);
			}

			const bool perkRulesActive = !PCF::RuleRegistry::Empty();
			const bool customConditionsPending = PCF::RuleRegistry::HasPendingCustomConditions();
			const bool customRegistryReady = PCF::RuleRegistry::CustomConditionsFinalized();
			const bool customConditionsActive = customRegistryReady && !PCF::CustomConditions::Empty();
			const bool customDialogueActive = customRegistryReady && PCF::CustomConditions::HasDialogueTargets();
			const bool customCraftingActive = customRegistryReady && PCF::CustomConditions::HasCraftingTargets();

			if (!perkRulesActive && !customConditionsActive && !customConditionsPending) {
				PCF::DialogueText::DisableInfoText();
				if (PCF::RuleRegistry::HasSWFAssignments()) {
					if (!textHooksInstalled) {
						textHooksInstalled = PCF::TextManager::Install();
					}
					if (!uiInstalled) {
						uiInstalled = PCF::UIManager::Install();
					}
					if (!uiInstalled) {
						report(spdlog::level::warn, "Standalone SWF UI hooks unavailable; retrying on the next new/load-game notification");
						return;
					}
					WriteLogHeader("HOOKS");
					if (!textHooksInstalled) {
						spdlog::warn("UI hook system active; MESSAGE body text hook unavailable");
					} else {
						spdlog::info("Required PCF hook systems active");
					}
					WriteLogHeader("RESULT");
					spdlog::info("Initialization complete");
				} else if (descriptionHooksActive) {
					WriteLogHeader("HOOKS");
					spdlog::info("Description hook system active");
					WriteLogHeader("RESULT");
					spdlog::info("Initialization complete");
				} else if (requirementLabelsActive) {
					WriteLogHeader("RESULT");
					spdlog::info("Requirement labels loaded; PRKF bridge {}", requirementLabelsInstalled ? "active" : "unavailable");
					spdlog::info("Initialization complete");
				} else {
					WriteLogHeader("RESULT");
					spdlog::info("No active PCF rules");
					spdlog::info("Initialization complete");
				}
				finished = true;
				return;
			}

			if (perkRulesActive && !conditionsInstalled) {
				conditionsInstalled = PCF::PerkConditions::Install();
			}
			if (perkRulesActive && !conditionsInstalled) {
				report(spdlog::level::err, "PCF condition hooks unavailable; retrying on the next new/load-game notification");
				return;
			}
			if (customRegistryReady && customConditionsActive && !customConditionsInstalled) {
				customConditionsInstalled = PCF::CustomConditions::Install();
			}
			if (customConditionsActive && !customConditionsInstalled) {
				report(spdlog::level::err, "Custom condition hooks unavailable; custom conditions remain disabled");
				return;
			}
			if (customRegistryReady && !customDialogueActive && !perkRulesActive) {
				PCF::DialogueText::DisableInfoText();
			}
			if (perkRulesActive && !textHooksInstalled) {
				textHooksInstalled = PCF::TextManager::Install();
			}
			const bool menuHooksNeeded = perkRulesActive || customCraftingActive || PCF::RuleRegistry::HasSWFAssignments();
			if (menuHooksNeeded && !uiInstalled) {
				uiInstalled = PCF::UIManager::Install();
				if (!uiInstalled) {
					report(spdlog::level::warn, "UI hooks unavailable; core condition framework remains active");
					return;
				}
			}
			// MESSAGE/INFO source text depends on fully initialized localized game data. Do not
			// permanently build the shared alias dictionary on the earlier GameDataReady event.
			if (!gameStateReady) {
				return;
			}
			if (!textApplied) {
				textResult = PCF::TextManager::UpdateMessages(messageLogLines);
				dialogueResult = PCF::DialogueText::UpdateTopics(topicLogLines);
				textApplied = true;
			}
			PCF::DialogueText::FinishInfoStartup(dialogueResult, infoLogLines, startupInfos);

			WriteLogHeader("HOOKS");
			if (perkRulesActive && !textHooksInstalled) {
				spdlog::warn("Condition and UI hook systems active; MESSAGE body text hook unavailable");
			} else {
				spdlog::info("Required PCF hook systems active");
			}
			WriteLogHeader("RESULT");
			spdlog::info("Registered {} aliases; modified {} message button(s), {} dialogue choice(s), and {} topic name(s)",
				textResult.aliases, textResult.messageButtonsChanged, dialogueResult.dialogueChoicesChanged,
				dialogueResult.topicNamesChanged);
			spdlog::info("INFO swaps: {} dialogue choice(s) modified across {} prompt(s) and {} response(s)",
				dialogueResult.dialogueChoicesChanged, dialogueResult.infoDirectChanges + dialogueResult.infoFallbackChanges,
				dialogueResult.infoResponseChanges);
			for (const auto& line : messageLogLines) {
				spdlog::info("{}", line);
			}
			for (const auto& line : infoLogLines) {
				spdlog::info("{}", line);
			}
			for (const auto& line : topicLogLines) {
				spdlog::info("{}", line);
			}
			spdlog::info("Initialization complete");
			finished = true;
		} catch (const std::exception& error) {
			spdlog::error("PCF initialization failed: {}", error.what());
		}
	}

}

// Starts PCF through the F4SE plugin entry point.
F4SE_PLUGIN_LOAD(const F4SE::LoadInterface* a_f4se)
{
	if (!a_f4se || a_f4se->IsEditor()) {
		return false;
	}
	try {
		F4SE::InitInfo initInfo;
		initInfo.logName = "PCF";
		initInfo.logFileName = std::string(Version::PROJECT);
		initInfo.logFormat = "[%H:%M:%S:%e] %v";

		F4SE::Init(a_f4se, initInfo);
		spdlog::info("{} v{} on Fallout 4 {}", Version::PROJECT, Version::NAME, F4SE::GetRuntimeVersion().ToString<char>());
		PCF::DialogueText::InstallEarlyHooks();
		const auto messaging = F4SE::GetMessagingInterface();
		messaging->RegisterListener(OnGameReady);
		return true;
	} catch (const std::exception& error) {
		spdlog::error("PCF load failed: {}", error.what());
		return false;
	}
}
