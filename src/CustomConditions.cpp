// Perk Conditions Framework
// SilentlyGayming
// CustomConditions.cpp

#include "PCH.h"
#include "CustomConditions.h"
#include "EngineIDs.h"
#include "NativeHooks.h"
#include "PerkConditions.h"
#include "RuleRegistry.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
	using IsTrueFunction = bool (*)(const RE::TESCondition*, RE::TESObjectREFR*, RE::TESObjectREFR*);
	using IsTrueContextFunction = bool (*)(const RE::TESCondition*, RE::ConditionCheckParams&);
	using IsTrueForAllButFunction = bool (*)(const RE::TESCondition*, RE::ConditionCheckParams&, RE::SCRIPT_OUTPUT);
	using WorkbenchChoiceRequirementsFunction = bool (*)(RE::WorkbenchMenuBase*, const RE::WorkbenchMenuBase::ModChoiceData*, bool);

	std::vector<std::unique_ptr<PCF::CustomConditions::ConditionSet>> g_sets;
	std::unordered_map<const RE::TESForm*, PCF::CustomConditions::ConditionSet*> g_byTarget;
	std::unordered_map<const RE::TESCondition*, PCF::CustomConditions::ConditionSet*> g_byConditionList;
	IsTrueFunction g_isTrueOriginal{ nullptr };
	IsTrueContextFunction g_isTrueContextOriginal{ nullptr };
	IsTrueForAllButFunction g_isTrueForAllButOriginal{ nullptr };
	WorkbenchChoiceRequirementsFunction g_workbenchChoiceRequirementsOriginal{ nullptr };
	bool g_installAttempted{ false };
	bool g_installed{ false };
	bool g_behaviorActive{ false };

	enum class HookKind
	{
		kIsTrue,
		kContext,
		kAllButFunction,
		kWorkbench
	};

	struct HookPlan
	{
		HookKind kind;
		std::uintptr_t source{ 0 };
		std::uintptr_t hook{ 0 };
		std::size_t stolenLength{ 0 };
		std::vector<std::uint8_t> originalBytes;
		std::vector<std::uint8_t> patch;
		void* original{ nullptr };
		void* relay{ nullptr };
	};

	// Handles complete normal condition lists.
	bool IsTrueHook(const RE::TESCondition* a_conditions, RE::TESObjectREFR* a_actionRef, RE::TESObjectREFR* a_targetRef)
	{
		if (const auto* set = PCF::CustomConditions::Find(a_conditions)) {
			return PCF::CustomConditions::Evaluate(*set);
		}
		return g_isTrueOriginal(a_conditions, a_actionRef, a_targetRef);
	}

	// Handles INFO consumers that evaluate a whole condition list with a native context object.
	bool IsTrueContextHook(const RE::TESCondition* a_conditions, RE::ConditionCheckParams& a_params)
	{
		if (const auto* set = PCF::CustomConditions::Find(a_conditions)) {
			a_params.outDispFailure = false;
			return PCF::CustomConditions::Evaluate(*set);
		}
		return g_isTrueContextOriginal(a_conditions, a_params);
	}

	// Handles condition-list consumers that intentionally exclude one native function.
	bool IsTrueForAllButFunctionHook(const RE::TESCondition* a_conditions, RE::ConditionCheckParams& a_params,
		RE::SCRIPT_OUTPUT a_excludedFunction)
	{
		if (const auto* set = PCF::CustomConditions::Find(a_conditions)) {
			a_params.outDispFailure = false;
			// WorkshopCanShowRecipe excludes HasPerk so unmet requirements stay visible. A registered
			// COBJ has no effective native CTDAs, so this visibility stage must pass; Workshop's later
			// full TESCondition::IsTrue call enforces the complete PCF replacement set.
			if (set->owner == PCF::CustomConditions::OwnerKind::kCrafting &&
				a_excludedFunction == RE::SCRIPT_OUTPUT::FUNCTION_HAS_PERK) {
				return true;
			}
			return PCF::CustomConditions::Evaluate(*set);
		}
		return g_isTrueForAllButOriginal(a_conditions, a_params, a_excludedFunction);
	}

	// Adds the replacement set after an empty native Workbench recipe passes Bethesda's other checks.
	bool WorkbenchChoiceRequirementsHook(RE::WorkbenchMenuBase* a_menu,
		const RE::WorkbenchMenuBase::ModChoiceData* a_choice, bool a_flag)
	{
		const bool nativeResult = g_workbenchChoiceRequirementsOriginal(a_menu, a_choice, a_flag);
		if (!nativeResult || !a_choice || !a_choice->recipe || a_choice->recipe->conditions.head) {
			return nativeResult;
		}
		const auto* set = PCF::CustomConditions::Find(static_cast<const RE::TESForm*>(a_choice->recipe));
		return !set || PCF::CustomConditions::Evaluate(*set);
	}

	// Resolves one runtime-database hook target and proves a safe whole-instruction overwrite span.
	bool AddHookPlan(std::vector<HookPlan>& a_plans, HookKind a_kind, const REL::ID& a_id,
		std::uintptr_t a_hook, std::string_view a_name)
	{
		const auto resolved = REL::IDDatabase::get().resolve(a_id);
		if (!resolved) {
			spdlog::error("Custom conditions: {} relocation unavailable; feature disabled", a_name);
			return false;
		}
		const auto source = REL::Module::get().base() + *resolved.rva;
		const auto stolenLength = PCF::NativeHooks::FindSafeOverwriteLength(source);
		if (stolenLength < PCF::NativeHooks::kRelativeJumpSize) {
			spdlog::error("Custom conditions: {} entry cannot be copied safely; feature disabled", a_name);
			return false;
		}

		HookPlan plan{ a_kind, source, a_hook, stolenLength };
		plan.originalBytes.resize(stolenLength);
		std::memcpy(plan.originalBytes.data(), reinterpret_cast<const void*>(source), stolenLength);
		a_plans.push_back(std::move(plan));
		return true;
	}

	// Installs the required whole-list hooks only after every target and patch span has been validated.
	bool InstallHooks()
	{
		std::vector<HookPlan> plans;
		plans.reserve(1 + (PCF::CustomConditions::HasDialogueTargets() ? 1 : 0) +
			(PCF::CustomConditions::HasCraftingTargets() ? 2 : 0));
		if (!AddHookPlan(plans, HookKind::kIsTrue, PCF::EngineIDs::TESConditionIsTrue,
			reinterpret_cast<std::uintptr_t>(&IsTrueHook), "TESCondition::IsTrue")) {
			return false;
		}
		if (PCF::CustomConditions::HasDialogueTargets() &&
			!AddHookPlan(plans, HookKind::kContext, PCF::EngineIDs::TESConditionIsTrueContext,
				reinterpret_cast<std::uintptr_t>(&IsTrueContextHook), "TESCondition context evaluator")) {
			return false;
		}
		if (PCF::CustomConditions::HasCraftingTargets()) {
			if (!AddHookPlan(plans, HookKind::kAllButFunction, PCF::EngineIDs::TESConditionIsTrueForAllButFunction,
				reinterpret_cast<std::uintptr_t>(&IsTrueForAllButFunctionHook), "TESCondition::IsTrueForAllButFunction") ||
				!AddHookPlan(plans, HookKind::kWorkbench, PCF::EngineIDs::WorkbenchChoiceRequirements,
					reinterpret_cast<std::uintptr_t>(&WorkbenchChoiceRequirementsHook), "Workbench requirement helper 2223051")) {
				return false;
			}
		}

		std::size_t poolBytes = 0;
		for (const auto& plan : plans) {
			poolBytes += plan.stolenLength + (PCF::NativeHooks::kAbsoluteJumpSize * 2);
		}
		const auto* trampoline = F4SE::GetTrampolineInterface();
		auto* memory = trampoline ? static_cast<std::uint8_t*>(trampoline->AllocateFromBranchPool(poolBytes)) : nullptr;
		if (!memory) {
			spdlog::error("Custom conditions: branch-pool allocation failed; feature disabled");
			return false;
		}

		auto* cursor = memory;
		for (auto& plan : plans) {
			plan.original = cursor;
			std::memcpy(cursor, plan.originalBytes.data(), plan.stolenLength);
			PCF::NativeHooks::WriteAbsoluteJump(cursor + plan.stolenLength, plan.source + plan.stolenLength);
			cursor += plan.stolenLength + PCF::NativeHooks::kAbsoluteJumpSize;

			plan.relay = cursor;
			PCF::NativeHooks::WriteAbsoluteJump(cursor, plan.hook);
			cursor += PCF::NativeHooks::kAbsoluteJumpSize;

			if (!PCF::NativeHooks::MakeRelativeJumpPatch(plan.source,
				reinterpret_cast<std::uintptr_t>(plan.relay), plan.stolenLength, plan.patch)) {
				spdlog::error("Custom conditions: hook relay is outside rel32 range; feature disabled");
				return false;
			}
		}

		for (const auto& plan : plans) {
			switch (plan.kind) {
			case HookKind::kIsTrue:
				g_isTrueOriginal = reinterpret_cast<IsTrueFunction>(plan.original);
				break;
			case HookKind::kContext:
				g_isTrueContextOriginal = reinterpret_cast<IsTrueContextFunction>(plan.original);
				break;
			case HookKind::kAllButFunction:
				g_isTrueForAllButOriginal = reinterpret_cast<IsTrueForAllButFunction>(plan.original);
				break;
			case HookKind::kWorkbench:
				g_workbenchChoiceRequirementsOriginal = reinterpret_cast<WorkbenchChoiceRequirementsFunction>(plan.original);
				break;
			}
		}

		for (const auto& plan : plans) {
			if (std::memcmp(reinterpret_cast<const void*>(plan.source), plan.originalBytes.data(), plan.originalBytes.size()) != 0) {
				spdlog::error("Custom conditions: native hook target changed before installation; feature disabled");
				return false;
			}
			if (!PCF::NativeHooks::WriteVerified(plan.source, plan.patch)) {
				const bool restored = PCF::NativeHooks::WriteVerified(plan.source, plan.originalBytes);
				spdlog::error("Custom conditions: native hook write verification failed{}; feature disabled",
					restored ? " and the original bytes were restored" : " and the original bytes could not be verified");
				return false;
			}
		}
		return true;
	}
}

namespace PCF::CustomConditions
{
	void Clear()
	{
		if (g_installAttempted) {
			return;
		}
		g_sets.clear();
		g_byTarget.clear();
		g_byConditionList.clear();
		g_installAttempted = false;
		g_behaviorActive = false;
		g_isTrueOriginal = nullptr;
		g_isTrueContextOriginal = nullptr;
		g_isTrueForAllButOriginal = nullptr;
		g_workbenchChoiceRequirementsOriginal = nullptr;
	}

	bool Add(RE::TESForm* a_target, std::vector<Condition> a_conditions)
	{
		if (!a_target || a_conditions.empty() || g_installed || g_byTarget.contains(a_target)) {
			return false;
		}
		OwnerKind owner;
		RE::TESCondition* nativeConditions = nullptr;
		bool nativeListEmpty = false;
		if (auto* recipe = a_target->As<RE::BGSConstructibleObject>()) {
			owner = OwnerKind::kCrafting;
			nativeConditions = std::addressof(recipe->conditions);
			nativeListEmpty = recipe->conditions.head == nullptr;
		} else if (auto* info = a_target->As<RE::TESTopicInfo>()) {
			owner = OwnerKind::kDialogue;
			nativeConditions = std::addressof(info->objConditions);
			nativeListEmpty = info->objConditions.head == nullptr;
		} else {
			return false;
		}
		if (!nativeConditions || g_byConditionList.contains(nativeConditions)) {
			return false;
		}

		auto owned = std::make_unique<ConditionSet>();
		owned->target = a_target;
		owned->nativeConditions = nativeConditions;
		owned->owner = owner;
		owned->nativeListEmpty = nativeListEmpty;
		owned->conditions = std::move(a_conditions);
		auto* set = owned.get();
		g_sets.push_back(std::move(owned));
		g_byTarget.emplace(a_target, set);
		g_byConditionList.emplace(nativeConditions, set);
		return true;
	}

	void Finalize()
	{
		for (auto& owned : g_sets) {
			auto& set = *owned;
			set.dialoguePrefix.clear();
			std::vector<const std::string*> requirementLabels(set.conditions.size(), nullptr);
			for (std::size_t conditionIndex = 0; conditionIndex < set.conditions.size(); ++conditionIndex) {
				auto& condition = set.conditions[conditionIndex];
				if (condition.kind == ConditionKind::kFormValue) {
					// A custom condition that references a source perk must follow the same
					// configured PCF display policy as a native HasPerk condition. Otherwise
					// authoring a replacement condition set would accidentally resurrect the
					// vanilla perk name/icon even though that perk is replaced elsewhere.
					const auto* configured = condition.value.type == AlternativeType::kPerk && condition.value.perk ?
						TextManager::GetConfiguredRequirement(condition.value.perk) : nullptr;
					condition.presentation = configured ? *configured : TextManager::BuildRequirementPresentation(condition.value);
					if (set.owner == OwnerKind::kDialogue) {
						auto* displayForm = GetDisplayForm(condition);
						if (const auto* label = RuleRegistry::FindRequirementLabel(set.target, displayForm)) {
							requirementLabels[conditionIndex] = label;
							(void)TextManager::ApplyRequirementLabel(set.target, displayForm, condition.presentation);
						}
					}
				} else {
					// Quest progression conditions are intentionally gameplay-only.  They may
					// unlock or gate an object, including as part of an OR group, but PCF must
					// never expose quest-stage logic through requirement text, cards, or icons.
					condition.presentation = {};
				}
			}
			if (set.owner == OwnerKind::kDialogue && RuleRegistry::HasRequirementLabels(set.target)) {
				std::pair<RE::TESForm*, const std::string*> previousStandalone{};
				bool hasPreviousStandalone = false;
				for (std::size_t groupStart = 0; groupStart < set.conditions.size();) {
					std::size_t groupEnd = groupStart;
					while (groupEnd + 1 < set.conditions.size() && set.conditions[groupEnd].orWithNext) {
						++groupEnd;
					}

					std::vector<std::size_t> visible;
					for (std::size_t i = groupStart; i <= groupEnd; ++i) {
						if (set.conditions[i].presentation.fullLabel.empty()) {
							continue;
						}
						if (requirementLabels[i]) {
							auto* displayForm = GetDisplayForm(set.conditions[i]);
							const auto duplicate = std::ranges::find_if(visible, [&](std::size_t a_existing) {
								return requirementLabels[a_existing] == requirementLabels[i] &&
									GetDisplayForm(set.conditions[a_existing]) == displayForm;
							});
							if (duplicate != visible.end()) {
								set.conditions[i].presentation = {};
								continue;
							}
						}
						visible.push_back(i);
					}

					if (visible.size() == 1 && requirementLabels[visible.front()]) {
						auto* displayForm = GetDisplayForm(set.conditions[visible.front()]);
						const auto identity = std::pair{ displayForm, requirementLabels[visible.front()] };
						if (hasPreviousStandalone && previousStandalone == identity) {
							set.conditions[visible.front()].presentation = {};
						} else {
							previousStandalone = identity;
							hasPreviousStandalone = true;
						}
					} else {
						hasPreviousStandalone = false;
					}
					groupStart = groupEnd + 1;
				}
			}

			if (set.owner == OwnerKind::kCrafting) {
				for (std::size_t i = 1; i < set.conditions.size(); ++i) {
					if (set.conditions[i - 1].orWithNext && IsPlayerFacing(set.conditions[i - 1]) &&
						IsPlayerFacing(set.conditions[i]) && !set.conditions[i].presentation.rowLabel.empty()) {
						set.conditions[i].presentation.rowLabel = "OR " + set.conditions[i].presentation.rowLabel;
					}
				}
			}
			if (set.owner == OwnerKind::kDialogue) {
				if (RuleRegistry::HasRequirementLabels(set.target)) {
					for (std::size_t groupStart = 0; groupStart < set.conditions.size();) {
						std::size_t groupEnd = groupStart;
						while (groupEnd + 1 < set.conditions.size() && set.conditions[groupEnd].orWithNext) {
							++groupEnd;
						}

						std::vector<std::string_view> visible;
						for (std::size_t i = groupStart; i <= groupEnd; ++i) {
							if (IsPlayerFacing(set.conditions[i]) && !set.conditions[i].presentation.fullLabel.empty()) {
								visible.push_back(set.conditions[i].presentation.fullLabel);
							}
						}
						if (!visible.empty()) {
							set.dialoguePrefix.push_back('[');
							for (std::size_t i = 0; i < visible.size(); ++i) {
								if (i) {
									set.dialoguePrefix.append(" OR ");
								}
								set.dialoguePrefix.append(visible[i]);
							}
							set.dialoguePrefix.append("] ");
						}
						groupStart = groupEnd + 1;
					}
				} else {
					bool groupOpen = false;
					for (std::size_t i = 0; i < set.conditions.size(); ++i) {
						const auto& condition = set.conditions[i];
						if (condition.presentation.fullLabel.empty()) {
							continue;
						}
						if (!groupOpen) {
							set.dialoguePrefix.push_back('[');
							groupOpen = true;
						}
						set.dialoguePrefix.append(condition.presentation.fullLabel);
						const bool visibleOrNext = condition.orWithNext && i + 1 < set.conditions.size() &&
							IsPlayerFacing(set.conditions[i + 1]) && !set.conditions[i + 1].presentation.fullLabel.empty();
						if (visibleOrNext) {
							set.dialoguePrefix.append(" OR ");
						} else {
							set.dialoguePrefix.append("] ");
							groupOpen = false;
						}
					}
					if (groupOpen) {
						set.dialoguePrefix.append("] ");
					}
				}
			}
			const auto visibleConditionCount = static_cast<std::size_t>(std::count_if(
				set.conditions.begin(), set.conditions.end(), [](const Condition& condition) { return IsPlayerFacing(condition); }));
			if (set.owner == OwnerKind::kCrafting && visibleConditionCount > 2) {
				const auto* files = set.target ? set.target->sourceFiles.array : nullptr;
				const auto plugin = files && !files->empty() && (*files)[0] ? (*files)[0]->GetFilename() : std::string_view{ "unknown" };
				if (visibleConditionCount > 4) {
					spdlog::warn("Custom conditions: {} [{:08X}] has {} player-facing condition(s); crafting UI displays up to 4 and Workshop UI up to 2 when applicable; gameplay evaluates all {} condition(s)",
						plugin, set.target ? set.target->GetFormID() : 0, visibleConditionCount, set.conditions.size());
				} else {
					spdlog::warn("Custom conditions: {} [{:08X}] has {} player-facing condition(s); Workshop UI displays up to 2 when applicable; gameplay evaluates all {} condition(s)",
						plugin, set.target ? set.target->GetFormID() : 0, visibleConditionCount, set.conditions.size());
				}
			}
		}
	}

	bool Install()
	{
		if (g_installAttempted) {
			return g_installed || Empty();
		}
		g_installAttempted = true;
		g_behaviorActive = false;
		if (g_sets.empty()) {
			return true;
		}
		bool hooksInstalled = false;
		try {
			hooksInstalled = InstallHooks();
		} catch (...) {
			spdlog::error("Custom condition hook installation failed unexpectedly; custom target conditions disabled");
		}
		if (!hooksInstalled) {
			for (auto& set : g_sets) {
				set->active = false;
			}
			spdlog::error("Custom condition hooks unavailable; custom target conditions disabled");
			return false;
		}
		g_installed = true;
		g_behaviorActive = true;
		spdlog::info("Custom condition hooks active");
		return true;
	}

	bool Empty()
	{
		return std::none_of(g_sets.begin(), g_sets.end(), [](const auto& set) { return set->active; });
	}

	bool HasCraftingTargets()
	{
		return std::any_of(g_sets.begin(), g_sets.end(), [](const auto& set) {
			return set->active && set->owner == OwnerKind::kCrafting;
		});
	}

	bool HasDialogueTargets()
	{
		return std::any_of(g_sets.begin(), g_sets.end(), [](const auto& set) {
			return set->active && set->owner == OwnerKind::kDialogue;
		});
	}

	std::size_t TargetCount()
	{
		return static_cast<std::size_t>(std::count_if(g_sets.begin(), g_sets.end(), [](const auto& set) { return set->active; }));
	}

	std::size_t ConditionCount()
	{
		std::size_t count = 0;
		for (const auto& set : g_sets) {
			if (set->active) {
				count += set->conditions.size();
			}
		}
		return count;
	}

	std::size_t CraftingTargetCount()
	{
		return static_cast<std::size_t>(std::count_if(g_sets.begin(), g_sets.end(), [](const auto& set) {
			return set->active && set->owner == OwnerKind::kCrafting;
		}));
	}

	std::size_t DialogueTargetCount()
	{
		return static_cast<std::size_t>(std::count_if(g_sets.begin(), g_sets.end(), [](const auto& set) {
			return set->active && set->owner == OwnerKind::kDialogue;
		}));
	}

	std::size_t QuestConditionCount()
	{
		std::size_t count = 0;
		for (const auto& set : g_sets) {
			if (!set->active) {
				continue;
			}
			count += static_cast<std::size_t>(std::count_if(set->conditions.begin(), set->conditions.end(), [](const Condition& condition) {
				return condition.kind == ConditionKind::kQuestStage || condition.kind == ConditionKind::kQuestStageDone;
			}));
		}
		return count;
	}

	const ConditionSet* Find(const RE::TESForm* a_target)
	{
		if (!g_behaviorActive) {
			return nullptr;
		}
		const auto found = g_byTarget.find(a_target);
		return found != g_byTarget.end() && found->second->active ? found->second : nullptr;
	}

	const ConditionSet* Find(const RE::TESCondition* a_conditions)
	{
		if (!g_behaviorActive) {
			return nullptr;
		}
		const auto found = g_byConditionList.find(a_conditions);
		return found != g_byConditionList.end() && found->second->active ? found->second : nullptr;
	}

	RE::TESForm* GetValueForm(const Condition& a_condition)
	{
		if (a_condition.kind == ConditionKind::kQuestStage || a_condition.kind == ConditionKind::kQuestStageDone) {
			return a_condition.quest;
		}
		switch (a_condition.value.type) {
		case AlternativeType::kPerk:
			return a_condition.value.perk;
		case AlternativeType::kActorValue:
			return a_condition.value.actorValue;
		case AlternativeType::kGlobalValue:
			return a_condition.value.globalValue;
		}
		return nullptr;
	}

	RE::TESForm* GetDisplayForm(const Condition& a_condition)
	{
		if (!IsPlayerFacing(a_condition)) {
			return nullptr;
		}
		if (a_condition.value.type == AlternativeType::kPerk && a_condition.value.perk) {
			if (const auto* rule = PCF::RuleRegistry::Find(a_condition.value.perk)) {
				if (const auto* display = PCF::RuleRegistry::GetDisplay(*rule)) {
					switch (display->type) {
					case AlternativeType::kPerk:
						return display->perk;
					case AlternativeType::kActorValue:
						return display->actorValue;
					case AlternativeType::kGlobalValue:
						return display->globalValue;
					}
				}
			}
		}
		return GetValueForm(a_condition);
	}

	const std::string* GetArtworkPath(const Condition& a_condition)
	{
		if (!IsPlayerFacing(a_condition)) {
			return nullptr;
		}
		if (a_condition.value.type == AlternativeType::kPerk && a_condition.value.perk) {
			if (const auto* rule = PCF::RuleRegistry::Find(a_condition.value.perk)) {
				if (const auto* display = PCF::RuleRegistry::GetDisplay(*rule)) {
					if (!display->iconPath.empty()) {
						return std::addressof(display->iconPath);
					}
					if (auto* form = GetDisplayForm(a_condition)) {
						if (const auto* configured = PCF::RuleRegistry::FindSWF(form); configured && !configured->empty()) {
							return configured;
						}
					}
				}
			}
		}
		return a_condition.value.iconPath.empty() ? nullptr : std::addressof(a_condition.value.iconPath);
	}

	bool IsPlayerFacing(const Condition& a_condition)
	{
		// Quest-stage state is implementation/progression logic, not a requirement
		// the UI should disclose to the player. Keep this centralized so every menu
		// follows the same invariant.
		return a_condition.kind == ConditionKind::kFormValue;
	}

	bool Evaluate(const Condition& a_condition)
	{
		if (a_condition.kind == ConditionKind::kQuestStage || a_condition.kind == ConditionKind::kQuestStageDone) {
			if (!a_condition.quest) {
				return false;
			}

			// Reuse Fallout's condition-function implementation instead of approximating quest state.
			// This is a transient PCF-owned CTDA item; no Bethesda-owned list is modified.
			RE::TESConditionItem native{};
			native.data.functionData.param[0] = a_condition.quest;
			if (a_condition.kind == ConditionKind::kQuestStageDone) {
				// GetStageDone(Quest, Stage) returns a boolean-like result which vanilla CTDAs test == 1.
				native.data.value = 1.0F;
				native.data.functionData.function = static_cast<RE::SCRIPT_OUTPUT>(59);
				native.data.functionData.param[1] = reinterpret_cast<void*>(static_cast<std::uintptr_t>(a_condition.questStage));
				native.data.condition = RE::ENUM_COMPARISON_CONDITION::kEqual;
			} else {
				// GetStage(Quest) is compared numerically by the native CTDA operator.
				native.data.value = static_cast<float>(a_condition.questStage);
				native.data.functionData.function = static_cast<RE::SCRIPT_OUTPUT>(58);
				native.data.functionData.param[1] = nullptr;
				switch (a_condition.value.comparison) {
				case ComparisonOp::kEqual:
					native.data.condition = RE::ENUM_COMPARISON_CONDITION::kEqual;
					break;
				case ComparisonOp::kGreater:
					native.data.condition = RE::ENUM_COMPARISON_CONDITION::kGreaterThan;
					break;
				case ComparisonOp::kGreaterEqual:
					native.data.condition = RE::ENUM_COMPARISON_CONDITION::kGreaterThanEqual;
					break;
				case ComparisonOp::kLess:
					native.data.condition = RE::ENUM_COMPARISON_CONDITION::kLessThan;
					break;
				case ComparisonOp::kLessEqual:
					native.data.condition = RE::ENUM_COMPARISON_CONDITION::kLessThanEqual;
					break;
				}
			}
			return native.IsTrue(nullptr, nullptr);
		}

		const auto& value = a_condition.value;
		switch (value.type) {
		case AlternativeType::kPerk: {
			// Perks that are PCF source perks must use the same effective replacement
			// semantics as native HasPerk checks. This keeps [Conditions] composable
			// with [Perks]/[ActorValues]/[GlobalValues] instead of bypassing them.
			if (value.perk && PCF::RuleRegistry::Find(value.perk)) {
				const auto effective = PCF::PerkConditions::PlayerPasses(value.perk) ? 1.0F : 0.0F;
				return CheckComparison(effective, value.comparison, value.requiredValue);
			}
			auto* player = RE::PlayerCharacter::GetSingleton();
			return player && value.perk && CheckComparison(PCF::PerkConditions::GetPlayerPerkRank(player, value.perk), value.comparison, value.requiredValue);
		}
		case AlternativeType::kActorValue: {
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player || !value.actorValue) {
				return false;
			}
			const auto current = player->GetActorValue(*value.actorValue);
			return std::isfinite(current) && CheckComparison(current, value.comparison, value.requiredValue);
		}
		case AlternativeType::kGlobalValue: {
			if (!value.globalValue) {
				return false;
			}
			const auto current = value.globalValue->GetValue();
			return std::isfinite(current) && CheckComparison(current, value.comparison, value.requiredValue);
		}
		}
		return false;
	}

	bool Evaluate(const ConditionSet& a_set)
	{
		if (!a_set.active || a_set.conditions.empty()) {
			return false;
		}

		// Match native CTDA grouping: a row marked OR combines with the following row.
		// Unmarked boundaries AND the completed OR groups together. Existing rows never set
		// orWithNext, so legacy Custom Conditions retain their original all-AND behavior.
		bool groupResult = Evaluate(a_set.conditions.front());
		for (std::size_t i = 1; i < a_set.conditions.size(); ++i) {
			const bool current = Evaluate(a_set.conditions[i]);
			if (a_set.conditions[i - 1].orWithNext) {
				groupResult = groupResult || current;
			} else {
				if (!groupResult) {
					return false;
				}
				groupResult = current;
			}
		}
		return groupResult;
	}
}
