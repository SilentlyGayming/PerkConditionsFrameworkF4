// Perk Conditions Framework
// SilentlyGayming
// WorkshopMenu.cpp

#include "PCH.h"
#include "WorkshopMenu.h"
#include "CustomConditions.h"
#include "NativeHooks.h"
#include "PerkConditions.h"
#include "RuleRegistry.h"
#include "TextManager.h"
#include "UICommon.h"
#include "UIManager.h"

#include <Zydis/Zydis.h>

#ifndef NOMINMAX
#	define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
	using PCF::UICommon::DepthGuard;
	using PCF::UICommon::ReadBool;
	using PCF::UICommon::ReadElement;
	using PCF::UICommon::ReadIndex;
	using PCF::UICommon::ReadNumber;
	using PCF::UICommon::ReadText;
	using PCF::UICommon::SetBool;
	using PCF::UICommon::SetNumber;
	using PCF::UICommon::SetText;
	using Value = PCF::UICommon::Value;
	using Params = PCF::UIHookState::Params;
	using CallFunction = PCF::UIHookState::CallFunction;
	using ProcessMessageFunction = PCF::UIHookState::ProcessMessageFunction;
	using AdvanceFunction = PCF::UIHookState::AdvanceFunction;
	using PreDisplayFunction = PCF::UIHookState::PreDisplayFunction;
	using ButtonEventFunction = void (*)(RE::BSInputEventUser*, const RE::ButtonEvent*);
	constexpr std::size_t kCallSlot = 0x01;
	constexpr std::size_t kProcessMessageSlot = 0x03;
	constexpr std::size_t kAdvanceSlot = 0x04;
	constexpr std::size_t kPreDisplaySlot = 0x05;
	constexpr std::size_t kButtonEventSlot = 0x08;
	constexpr std::size_t kWorkshopIndex = 4;
	constexpr std::size_t kExamineRequirementSlots = 4;
	constexpr std::array<std::string_view, kExamineRequirementSlots> kPerkPanelNames{
		"PerkPanel0_mc", "PerkPanel1_mc", "PerkPanel2_mc", "PerkPanel3_mc"
	};
	constexpr std::size_t kWorkshopRequirementSlots = 2;
	constexpr std::uint32_t kHashOffset = 2166136261u;
	constexpr std::uint32_t kHashPrime = 16777619u;
	constexpr std::uint32_t kWorkshopVisualOwnerDepthLimit = 8;
	constexpr std::array<std::string_view, kWorkshopRequirementSlots> kWorkshopFallUIShadowOwners{
		"FallUIShadow_PerkPanel0_mc", "FallUIShadow_PerkPanel1_mc"
	};
	constexpr std::array<std::string_view, kWorkshopRequirementSlots> kWorkshopFallUIAuthoredOwners{
		"PerkPanel0_mc_real", "PerkPanel1_mc_real"
	};
	constexpr std::string_view kWorkshopVisualReady = "__PCFWorkshopVisualResolved";
	constexpr std::string_view kWorkshopVisualGeneration = "__PCFWorkshopVisualGeneration";
	constexpr std::string_view kWorkshopSlot0Owners = "__PCFWorkshopSlot0Owners";
	constexpr std::string_view kWorkshopSlot1Owners = "__PCFWorkshopSlot1Owners";
	constexpr std::string_view kWorkshopSlot0Depth = "__PCFWorkshopSlot0Depth";
	constexpr std::string_view kWorkshopSlot1Depth = "__PCFWorkshopSlot1Depth";
	constexpr std::string_view kWorkshopSavedVisible = "__PCFSlotSavedVisible";
	constexpr std::string_view kWorkshopSavedAlpha = "__PCFSlotSavedAlpha";


	struct WorkshopRequirement
	{
		const PCF::PerkRule* rule{ nullptr };
		const PCF::PerkAlternative* display{ nullptr };
		const PCF::TextManager::RequirementPresentation* presentation{ nullptr };
		const std::string* artwork{ nullptr };
		RE::BGSPerk* source{ nullptr };
		const PCF::CustomConditions::Condition* custom{ nullptr };
		bool locked{ false };
	};

	struct WorkshopArtworkState
	{
		std::uint32_t selection{ 0 };
		std::uint32_t source{ 0 };
		std::uint32_t display{ 0 };
		std::uint32_t token{ 0 };
		std::string path;
		bool managed{ false };
		bool requested{ false };
		bool nativeDirty{ false };
	};

	struct WorkshopSelection
	{
		const RE::BGSConstructibleObject* recipe{ nullptr };
		const RE::BGSConstructibleObject* sourceRecipe{ nullptr };
		std::vector<WorkshopRequirement> requirements;
		bool customOwned{ false };
		std::size_t perkConditionCount{ 0 };
		std::size_t matchedRuleCount{ 0 };
		std::size_t customConditionCount{ 0 };
		std::uint16_t row{ 0 };
		std::uint32_t column{ 0 };
	};

	enum class WorkshopStatus : std::uint32_t
	{
		kReady,
		kMenuUnavailable,
		kCurrentRowUnavailable,
		kCurrentRowNull,
		kSelectedNodeNull,
		kRecipeUnavailable,
		kNoPerkRequirement,
		kNoRegistryMatch,
		kPanelsUnavailable,
		kApplied,
		kAlreadyCurrent
	};

	enum : std::uint32_t
	{
		kOwnerStructural = 1u << 0,
		kOwnerShadow = 1u << 1,
		kOwnerReal = 1u << 2
	};

	struct PanelOwners
	{
		std::array<std::uint32_t, kWorkshopRequirementSlots> ownerFlags{};
		std::array<std::uint32_t, kWorkshopRequirementSlots> structuralDepth{};
		std::uint32_t generation{ 0 };
	};

	std::array<WorkshopArtworkState, kWorkshopRequirementSlots> g_workshopArtwork;
	std::uint32_t g_workshopArtworkToken{ 0 };
	using WorkshopSetMemberFunction = bool (*)(Value::ObjectInterface*, void*, const char*, const Value&, bool);
	WorkshopSetMemberFunction g_workshopSetMember{ nullptr };
	bool g_workshopArtworkInstalled{ false };
	thread_local bool g_workshopPublishing{ false };
	thread_local std::uint32_t g_workshopCallDepth{ 0 };
	thread_local std::uint32_t g_workshopMessageDepth{ 0 };
	thread_local std::uint32_t g_workshopAdvanceDepth{ 0 };
	thread_local std::uint32_t g_workshopPreDisplayDepth{ 0 };
	thread_local std::uint32_t g_workshopButtonDepth{ 0 };
	ButtonEventFunction g_workshopButtonEvent{ nullptr };
	std::atomic_uint32_t g_workshopAdvanceContext{ 0 };
	std::atomic_uint32_t g_workshopPresentationSelectionKey{ 0 };
	std::atomic_uint32_t g_workshopDisplayKey{ 0 };
	std::atomic_bool g_workshopPresentationVerifyPending{ false };
	std::atomic_bool g_workshopPreDisplayPending{ false };
// Returns the TESForm represented by a display alternative.
	RE::TESForm* GetDisplayForm(const PCF::PerkAlternative& a_alternative)
	{
		switch (a_alternative.type) {
		case PCF::AlternativeType::kPerk:
			return a_alternative.perk;
		case PCF::AlternativeType::kActorValue:
			return a_alternative.actorValue;
		case PCF::AlternativeType::kGlobalValue:
			return a_alternative.globalValue;
		}
		return nullptr;
	}
	// Adds one value to a display key.
	void AddToHash(std::uint32_t& a_hash, std::uint32_t a_value)
	{
		a_hash = (a_hash ^ a_value) * kHashPrime;
	}
	// Reads whether the Workshop requirement area is visible.
	bool IsHostVisible(RE::IMenu* a_menu, bool& a_visible)
	{
		a_visible = true;
		auto* workshop = RE::DynamicCast<RE::WorkshopMenu*>(a_menu);
		if (!workshop || !workshop->workshopMenuBase || !workshop->workshopMenuBase->requirementsBase) {
			return false;
		}
		Value host(*workshop->workshopMenuBase->requirementsBase);
		return ReadBool(host, "visible", a_visible);
	}
	// Builds a key for the current Workshop context.
	std::uint32_t GetContextKey(RE::IMenu* a_menu, const WorkshopSelection& a_selection, WorkshopStatus a_status)
	{
		std::uint32_t key = kHashOffset;
		AddToHash(key, static_cast<std::uint32_t>(a_status));
		AddToHash(key, a_selection.row);
		AddToHash(key, a_selection.column);
		AddToHash(key, a_selection.recipe ? a_selection.recipe->GetFormID() : 0u);
		AddToHash(key, a_selection.sourceRecipe ? a_selection.sourceRecipe->GetFormID() : 0u);
		bool hostVisible = true;
		AddToHash(key, IsHostVisible(a_menu, hostVisible) ? (hostVisible ? 3u : 2u) : 1u);
		return key == 0 ? 1u : key;
	}
	// Builds a key for the current Workshop selection.
	std::uint32_t GetSelectionKey(const WorkshopSelection& a_selection)
	{
		std::uint32_t key = kHashOffset;
		AddToHash(key, a_selection.row);
		AddToHash(key, a_selection.column);
		AddToHash(key, a_selection.recipe ? a_selection.recipe->GetFormID() : 0u);
		AddToHash(key, a_selection.sourceRecipe ? a_selection.sourceRecipe->GetFormID() : 0u);
		return key == 0 ? 1u : key;
	}
	// Checks whether one display object contains another.
	bool ContainsDisplayObject(const RE::BSGFxDisplayObject* a_ancestor, const RE::BSGFxDisplayObject* a_object)
	{
		if (!a_ancestor || !a_object) {
			return false;
		}
		for (auto* current = a_object; current; current = current->parentDisplayObject) {
			if (current == a_ancestor) {
				return true;
			}
		}
		return false;
	}
	// Finds the display object that owns a Workshop panel.
	const RE::BSGFxDisplayObject* FindStructuralOwner(
		RE::BSGFxShaderFXTarget* a_panel, RE::BSGFxShaderFXTarget* a_sibling, std::uint32_t& a_depth)
	{
		a_depth = 0;
		if (!a_panel || !a_sibling) {
			return nullptr;
		}
		const RE::BSGFxDisplayObject* owner = a_panel;
		for (auto* parent = owner->parentDisplayObject; parent; parent = parent->parentDisplayObject) {
			if (ContainsDisplayObject(parent, a_sibling)) {
				break;
			}
			if (a_depth >= kWorkshopVisualOwnerDepthLimit) {
				a_depth = 0;
				return nullptr;
			}
			owner = parent;
			++a_depth;
		}
		if (owner == a_panel || a_depth == 0 || !owner->parentDisplayObject ||
			!ContainsDisplayObject(owner->parentDisplayObject, a_sibling)) {
			a_depth = 0;
			return nullptr;
		}
		return owner;
	}
	// Finds a valid display owner for a Workshop slot.
	const RE::BSGFxDisplayObject* FindPanelOwner(
		RE::BSGFxShaderFXTarget* a_panel, RE::BSGFxShaderFXTarget* a_sibling, std::uint32_t a_depth)
	{
		if (!a_panel || !a_sibling || a_depth == 0 || a_depth > kWorkshopVisualOwnerDepthLimit) {
			return nullptr;
		}
		const RE::BSGFxDisplayObject* owner = a_panel;
		for (std::uint32_t i = 0; i < a_depth; ++i) {
			owner = owner->parentDisplayObject;
			if (!owner) {
				return nullptr;
			}
		}
		if (!owner->parentDisplayObject || !ContainsDisplayObject(owner->parentDisplayObject, a_sibling)) {
			return nullptr;
		}
		return owner;
	}
	// Chooses the display owner for a Workshop slot.
	const RE::BSGFxDisplayObject* GetSlotOwner(RE::BSGFxShaderFXTarget* a_panel, RE::BSGFxShaderFXTarget* a_sibling)
	{
		if (!a_panel) {
			return nullptr;
		}
		const RE::BSGFxDisplayObject* owner = a_panel;
		if (!a_sibling) {
			return owner;
		}
		for (auto* parent = owner->parentDisplayObject; parent; parent = parent->parentDisplayObject) {
			if (ContainsDisplayObject(parent, a_sibling)) {
				break;
			}
			owner = parent;
		}
		return owner;
	}
	// Checks whether a named member is a display object.
	bool HasDisplayMember(const Value& a_object, std::string_view a_name)
	{
		Value member;
		return a_object.IsAnyObject() && a_object.GetMember(a_name.data(), &member) && member.IsDisplayObject();
	}

	// Builds a version number used to check saved panel owners.
	std::uint32_t GetMenuVersion(RE::IMenu* a_menu, const Value& a_base)
	{
		std::uint32_t key = kHashOffset;
		std::uint32_t menuChildren = 0;
		std::uint32_t baseChildren = 0;
		AddToHash(key, a_menu && ReadIndex(a_menu->menuObj, "numChildren", menuChildren) ? menuChildren : 0u);
		AddToHash(key, ReadIndex(a_base, "numChildren", baseChildren) ? baseChildren : 0u);
		return key == 0 ? 1u : key;
	}

	// Reads saved Workshop panel owners.
	bool GetSavedPanelOwners(RE::IMenu* a_menu, std::uint32_t a_generation, PanelOwners& a_ownership)
	{
		if (!a_menu || !a_menu->menuObj.IsAnyObject()) {
			return false;
		}
		bool resolved = false;
		std::uint32_t generation = 0;
		std::uint32_t slot0 = 0;
		std::uint32_t slot1 = 0;
		std::uint32_t depth0 = 0;
		std::uint32_t depth1 = 0;
		if (!ReadBool(a_menu->menuObj, kWorkshopVisualReady.data(), resolved) || !resolved ||
			!ReadIndex(a_menu->menuObj, kWorkshopVisualGeneration.data(), generation) || generation != a_generation ||
			!ReadIndex(a_menu->menuObj, kWorkshopSlot0Owners.data(), slot0) ||
			!ReadIndex(a_menu->menuObj, kWorkshopSlot1Owners.data(), slot1) ||
			!ReadIndex(a_menu->menuObj, kWorkshopSlot0Depth.data(), depth0) ||
			!ReadIndex(a_menu->menuObj, kWorkshopSlot1Depth.data(), depth1) ||
			depth0 > kWorkshopVisualOwnerDepthLimit || depth1 > kWorkshopVisualOwnerDepthLimit) {
			return false;
		}
		a_ownership.ownerFlags = { slot0, slot1 };
		a_ownership.structuralDepth = { depth0, depth1 };
		a_ownership.generation = generation;
		return true;
	}

	// Saves Workshop panel owners for the current menu.
	void SavePanelOwners(RE::IMenu* a_menu, const PanelOwners& a_ownership)
	{
		if (!a_menu || !a_menu->menuObj.IsAnyObject()) {
			return;
		}
		SetNumber(a_menu->menuObj, kWorkshopVisualGeneration.data(), static_cast<double>(a_ownership.generation));
		SetNumber(a_menu->menuObj, kWorkshopSlot0Owners.data(), static_cast<double>(a_ownership.ownerFlags[0]));
		SetNumber(a_menu->menuObj, kWorkshopSlot1Owners.data(), static_cast<double>(a_ownership.ownerFlags[1]));
		SetNumber(a_menu->menuObj, kWorkshopSlot0Depth.data(), static_cast<double>(a_ownership.structuralDepth[0]));
		SetNumber(a_menu->menuObj, kWorkshopSlot1Depth.data(), static_cast<double>(a_ownership.structuralDepth[1]));
		SetBool(a_menu->menuObj, kWorkshopVisualReady.data(), true);
	}

	// Finds the display owners for both Workshop slots.
	PanelOwners FindPanelOwners(RE::IMenu* a_menu)
	{
		PanelOwners ownership;
		auto* workshop = RE::DynamicCast<RE::WorkshopMenu*>(a_menu);
		if (!workshop || !workshop->workshopMenuBase) {
			return ownership;
		}
		auto* base = workshop->workshopMenuBase.get();
		Value baseValue(*base);
		ownership.generation = GetMenuVersion(a_menu, baseValue);
		if (GetSavedPanelOwners(a_menu, ownership.generation, ownership)) {
			return ownership;
		}

		const std::array<RE::BSGFxShaderFXTarget*, kWorkshopRequirementSlots> panels{
			base->perkPanel01.get(), base->perkPanel02.get()
		};
		for (std::size_t i = 0; i < panels.size(); ++i) {
			if (HasDisplayMember(baseValue, kWorkshopFallUIShadowOwners[i])) {
				ownership.ownerFlags[i] |= kOwnerShadow;
			}
			if (HasDisplayMember(baseValue, kWorkshopFallUIAuthoredOwners[i])) {
				ownership.ownerFlags[i] |= kOwnerReal;
			}
			std::uint32_t depth = 0;
			if (panels[i] && FindStructuralOwner(panels[i], panels[1 - i], depth)) {
				ownership.ownerFlags[i] |= kOwnerStructural;
				ownership.structuralDepth[i] = depth;
			}
		}

		SavePanelOwners(a_menu, ownership);
		return ownership;
	}
	// Changes visibility while saving the original value.
	bool SetOwnedVisible(Value& a_object, bool a_visible)
	{
		if (!a_object.IsAnyObject()) {
			return false;
		}
		Value saved;
		const bool owned = a_object.GetMember(kWorkshopSavedVisible.data(), &saved) && saved.IsBoolean();
		bool changed = false;
		if (!a_visible) {
			if (!owned) {
				bool visible = true;
				if (ReadBool(a_object, "visible", visible)) {
					a_object.SetMember(kWorkshopSavedVisible.data(), Value(visible));
				}
			}
			return SetBool(a_object, "visible", false);
		}
		if (owned) {
			changed = SetBool(a_object, "visible", saved.GetBoolean()) || changed;
			a_object.SetMember(kWorkshopSavedVisible.data(), Value());
		}
		changed = SetBool(a_object, "visible", true) || changed;
		return changed;
	}
	// Stops controlling visibility without restoring an outdated value.
	void ReleaseOwnedVisible(Value& a_object)
	{
		Value saved;
		if (a_object.IsAnyObject() && a_object.GetMember(kWorkshopSavedVisible.data(), &saved) && saved.IsBoolean()) {
			a_object.SetMember(kWorkshopSavedVisible.data(), Value());
		}
	}
	// Changes display visibility without changing ownership.
	bool SetShownVisible(Value& a_object, bool a_visible)
	{
		ReleaseOwnedVisible(a_object);
		return SetBool(a_object, "visible", a_visible);
	}
	// Changes alpha while saving the original value.
	bool SetOwnedAlpha(Value& a_object, bool a_visible)
	{
		if (!a_object.IsAnyObject()) {
			return false;
		}
		Value value;
		double alpha = 0.0;
		if (!a_object.GetMember("alpha", &value) || !ReadNumber(value, alpha)) {
			return false;
		}
		double savedAlpha = 0.0;
		const bool owned = a_object.GetMember(kWorkshopSavedAlpha.data(), &value) && ReadNumber(value, savedAlpha);
		if (!a_visible) {
			if (!owned && !a_object.SetMember(kWorkshopSavedAlpha.data(), Value(alpha))) {
				return false;
			}
			return SetNumber(a_object, "alpha", 0.0);
		}
		if (!owned) {
			return false;
		}
		const bool changed = alpha != savedAlpha && SetNumber(a_object, "alpha", savedAlpha);
		a_object.SetMember(kWorkshopSavedAlpha.data(), Value());
		return changed;
	}
	// Stops controlling alpha without restoring an outdated value.
	void ReleaseOwnedAlpha(Value& a_object)
	{
		Value value;
		double savedAlpha = 0.0;
		if (!a_object.IsAnyObject() || !a_object.GetMember(kWorkshopSavedAlpha.data(), &value) || !ReadNumber(value, savedAlpha)) {
			return;
		}
		double alpha = 0.0;
		Value current;
		if (a_object.GetMember("alpha", &current) && ReadNumber(current, alpha) && alpha != savedAlpha) {
			SetNumber(a_object, "alpha", savedAlpha);
		}
		a_object.SetMember(kWorkshopSavedAlpha.data(), Value());
	}
	// Takes control of visibility for one Workshop card.
	bool SetCardVisibility(Value& a_base, std::size_t a_slot, std::uint32_t a_flags, bool a_visible)
	{
		if (a_slot >= kWorkshopRequirementSlots) {
			return false;
		}
		bool changed = false;
		const auto apply = [&](std::string_view a_name) {
			Value owner;
			if (a_base.GetMember(a_name.data(), &owner) && owner.IsDisplayObject()) {
				changed = SetOwnedAlpha(owner, a_visible) || changed;
			}
		};
		if ((a_flags & kOwnerShadow) != 0) {
			apply(kWorkshopFallUIShadowOwners[a_slot]);
		}
		if ((a_flags & kOwnerReal) != 0) {
			apply(kWorkshopFallUIAuthoredOwners[a_slot]);
		}
		return changed;
	}
	// Releases visibility control for one Workshop card.
	void ReleaseCardOwnership(Value& a_base, std::size_t a_slot, std::uint32_t a_flags)
	{
		if (a_slot >= kWorkshopRequirementSlots) {
			return;
		}
		const auto release = [&](std::string_view a_name) {
			Value owner;
			if (a_base.GetMember(a_name.data(), &owner) && owner.IsDisplayObject()) {
				ReleaseOwnedAlpha(owner);
			}
		};
		if ((a_flags & kOwnerShadow) != 0) {
			release(kWorkshopFallUIShadowOwners[a_slot]);
		}
		if ((a_flags & kOwnerReal) != 0) {
			release(kWorkshopFallUIAuthoredOwners[a_slot]);
		}
	}
	// Sets all PCF-controlled visibility for one Workshop slot.
	bool SetSlotVisibility(RE::IMenu* a_menu, std::size_t a_slot, bool a_visible)
	{
		auto* workshop = RE::DynamicCast<RE::WorkshopMenu*>(a_menu);
		if (!workshop || !workshop->workshopMenuBase || a_slot >= kWorkshopRequirementSlots) {
			return false;
		}
		auto* base = workshop->workshopMenuBase.get();
		const std::array<RE::BSGFxShaderFXTarget*, kWorkshopRequirementSlots> panels{
			base->perkPanel01.get(), base->perkPanel02.get()
		};
		auto* panel = panels[a_slot];
		if (!panel) {
			return false;
		}
		const auto ownership = FindPanelOwners(a_menu);
		const auto flags = ownership.ownerFlags[a_slot];
		bool changed = false;
		Value baseValue(*base);
		changed = SetCardVisibility(baseValue, a_slot, flags, a_visible) || changed;
		if ((flags & kOwnerStructural) != 0) {
			if (const auto* owner = FindPanelOwner(panel, panels[1 - a_slot], ownership.structuralDepth[a_slot])) {
				Value ownerValue(*owner);
				changed = SetOwnedVisible(ownerValue, a_visible) || changed;
			}
		}
		Value panelValue(*panel);
		changed = SetOwnedVisible(panelValue, a_visible) || changed;
		for (auto* shaderObject : panel->shaderFXObjects) {
			if (!shaderObject) {
				continue;
			}
			Value shaderValue(*shaderObject);
			changed = SetOwnedVisible(shaderValue, a_visible) || changed;
		}
		return changed;
	}
	// Releases all PCF control for one Workshop slot.
	void ReleaseSlotOwnership(RE::IMenu* a_menu, std::size_t a_slot)
	{
		auto* workshop = RE::DynamicCast<RE::WorkshopMenu*>(a_menu);
		if (!workshop || !workshop->workshopMenuBase || a_slot >= kWorkshopRequirementSlots) {
			return;
		}
		auto* base = workshop->workshopMenuBase.get();
		const std::array<RE::BSGFxShaderFXTarget*, kWorkshopRequirementSlots> panels{
			base->perkPanel01.get(), base->perkPanel02.get()
		};
		auto* panel = panels[a_slot];
		if (!panel) {
			return;
		}
		const auto ownership = FindPanelOwners(a_menu);
		const auto flags = ownership.ownerFlags[a_slot];
		Value baseValue(*base);
		ReleaseCardOwnership(baseValue, a_slot, flags);
		if ((flags & kOwnerStructural) != 0) {
			if (const auto* owner = FindPanelOwner(panel, panels[1 - a_slot], ownership.structuralDepth[a_slot])) {
				Value ownerValue(*owner);
				ReleaseOwnedVisible(ownerValue);
			}
		}
		Value panelValue(*panel);
		ReleaseOwnedVisible(panelValue);
		for (auto* shaderObject : panel->shaderFXObjects) {
			if (!shaderObject) {
				continue;
			}
			Value shaderValue(*shaderObject);
			ReleaseOwnedVisible(shaderValue);
		}
		for (const auto* member : { "PerkName_tf", "Requires_tf", "PerkLock_mc", "PerkLoaderClip_mc" }) {
			Value child;
			if (panelValue.GetMember(member, &child) && child.IsAnyObject()) {
				ReleaseOwnedVisible(child);
			}
		}
	}
	// Releases PCF control from all Workshop slots.
	void ReleaseAllSlots(RE::IMenu* a_menu)
	{
		for (std::size_t i = 0; i < kWorkshopRequirementSlots; ++i) {
			ReleaseSlotOwnership(a_menu, i);
		}
	}
	// Builds a key for one Workshop requirement panel.
	bool GetPanelKey(RE::BSGFxShaderFXTarget* a_panel, RE::BSGFxShaderFXTarget* a_sibling, std::uint32_t& a_fingerprint)
	{
		a_fingerprint = kHashOffset;
		if (!a_panel) {
			return false;
		}
		const auto mix = [&a_fingerprint](std::uint32_t a_value) { AddToHash(a_fingerprint, a_value); };
		const auto mixString = [&mix](const char* a_text) {
			if (!a_text) {
				mix(0u);
				return;
			}
			mix(1u);
			for (const auto* character = reinterpret_cast<const unsigned char*>(a_text); *character; ++character) {
				mix(*character);
			}
		};
		const auto mixVisible = [&mix](const Value& a_object) {
			bool visible = false;
			if (ReadBool(a_object, "visible", visible)) {
				mix(visible ? 3u : 2u);
			} else {
				mix(1u);
			}
		};
		const auto mixMember = [&mix, &mixString, &mixVisible](const Value& a_panelValue, const char* a_name, bool a_text) {
			Value member;
			if (!a_panelValue.IsAnyObject() || !a_panelValue.GetMember(a_name, &member) || !member.IsAnyObject()) {
				mix(0u);
				return;
			}
			mix(1u);
			mixVisible(member);
			if (!a_text) {
				return;
			}
			Value text;
			if (member.GetMember("text", &text) && text.IsString()) {
				mixString(text.GetString());
			} else {
				mix(0u);
			}
		};

		Value panel(*a_panel);
		mixVisible(panel);
		if (const auto* owner = GetSlotOwner(a_panel, a_sibling)) {
			Value ownerValue(*owner);
			mixVisible(ownerValue);
		} else {
			mix(0u);
		}
		mix(static_cast<std::uint32_t>(a_panel->shaderFXObjects.size()));
		for (auto* shaderObject : a_panel->shaderFXObjects) {
			if (!shaderObject) {
				mix(0u);
				continue;
			}
			Value shaderValue(*shaderObject);
			mixVisible(shaderValue);
		}
		mixMember(panel, "PerkName_tf", true);
		mixMember(panel, "Requires_tf", true);
		mixMember(panel, "PerkLock_mc", false);
		mixMember(panel, "PerkLoaderClip_mc", false);
		if (a_fingerprint == 0) {
			a_fingerprint = 1;
		}
		return true;
	}
	// Builds a key for the current Workshop display.
	bool GetPresentationKey(RE::IMenu* a_menu, std::uint32_t& a_fingerprint)
	{
		a_fingerprint = kHashOffset;
		auto* workshop = RE::DynamicCast<RE::WorkshopMenu*>(a_menu);
		if (!workshop || !workshop->workshopMenuBase) {
			return false;
		}
		auto* base = workshop->workshopMenuBase.get();
		bool hostVisible = true;
		AddToHash(a_fingerprint, IsHostVisible(a_menu, hostVisible) ? (hostVisible ? 3u : 2u) : 1u);
		const std::array panels{ base->perkPanel01.get(), base->perkPanel02.get() };
		bool foundPanel = false;
		for (std::size_t i = 0; i < panels.size(); ++i) {
			AddToHash(a_fingerprint, static_cast<std::uint32_t>(i + 1));
			std::uint32_t panelFingerprint = 0;
			if (!GetPanelKey(panels[i], panels[1 - i], panelFingerprint)) {
				AddToHash(a_fingerprint, 0u);
				continue;
			}
			foundPanel = true;
			AddToHash(a_fingerprint, panelFingerprint);
		}
		if (!foundPanel) {
			return false;
		}
		if (a_fingerprint == 0) {
			a_fingerprint = 1;
		}
		return true;
	}
	// Saves the key for the current Workshop display.
	void SavePresentation(RE::IMenu* a_menu, const WorkshopSelection& a_selection, WorkshopStatus a_status)
	{
		if (a_status != WorkshopStatus::kApplied && a_status != WorkshopStatus::kAlreadyCurrent) {
			return;
		}
		std::uint32_t displayKey = 0;
		if (!GetPresentationKey(a_menu, displayKey)) {
			return;
		}
		g_workshopDisplayKey.store(displayKey, std::memory_order_relaxed);
		g_workshopPresentationSelectionKey.store(GetSelectionKey(a_selection), std::memory_order_release);
	}
	// Clears saved Workshop display state for a new menu.
	void ResetWorkshopState()
	{
		g_workshopArtwork = {};
		g_workshopAdvanceContext.store(0, std::memory_order_relaxed);
		g_workshopPresentationSelectionKey.store(0, std::memory_order_relaxed);
		g_workshopDisplayKey.store(0, std::memory_order_relaxed);
		g_workshopPresentationVerifyPending.store(false, std::memory_order_release);
		g_workshopPreDisplayPending.store(false, std::memory_order_release);
	}
	// Schedules one final Workshop check before display.
	void QueueFinalCheck(WorkshopStatus a_status)
	{
		if (a_status != WorkshopStatus::kApplied && a_status != WorkshopStatus::kAlreadyCurrent) {
			return;
		}
		g_workshopPreDisplayPending.store(true, std::memory_order_release);
		g_workshopPresentationVerifyPending.store(true, std::memory_order_release);
	}
	// Merges duplicate Workshop requirements that represent the same replacement.
	void MergeDuplicateRequirements(WorkshopSelection& a_selection)
	{
		struct FamilySlot
		{
			PCF::TextManager::RequirementIdentity identity;
			double strength{ 0.0 };
			std::size_t requirementIndex{ 0 };
		};

		const auto sourceCount = a_selection.requirements.size();
		if (sourceCount == 0) {
			return;
		}

		std::vector<WorkshopRequirement> consolidated;
		std::vector<FamilySlot> families;
		consolidated.reserve(sourceCount);
		families.reserve(sourceCount);

		for (const auto& requirement : a_selection.requirements) {
			const auto* presentation = requirement.presentation;
			if ((!requirement.rule && !requirement.custom) || !presentation || !presentation->identity.target) {
				consolidated.push_back(requirement);
				continue;
			}

			auto familySlot = std::find_if(families.begin(), families.end(), [presentation](const FamilySlot& a_slot) {
				return a_slot.identity == presentation->identity;
			});
			if (familySlot == families.end()) {
				families.push_back({ presentation->identity, presentation->strength, consolidated.size() });
				consolidated.push_back(requirement);
				continue;
			}

			const auto slot = familySlot->requirementIndex;
			if (presentation->strength > familySlot->strength) {
				consolidated[slot] = requirement;
				familySlot->strength = presentation->strength;
			}
		}

		a_selection.requirements = std::move(consolidated);
	}
	// Adds one PCF-owned replacement condition set.
	void AddCustomRequirements(const PCF::CustomConditions::ConditionSet& a_set, WorkshopSelection& a_selection)
	{
		a_selection.customOwned = true;
		for (const auto& condition : a_set.conditions) {
			if (!PCF::CustomConditions::IsPlayerFacing(condition)) {
				continue;
			}
			++a_selection.customConditionCount;
			a_selection.requirements.push_back({ nullptr, nullptr, std::addressof(condition.presentation),
				nullptr, nullptr, std::addressof(condition), !PCF::CustomConditions::Evaluate(condition) });
		}
	}

	// Adds effective native requirements from one Workshop recipe.
	void AddNativeRequirements(const RE::BGSConstructibleObject* a_recipe, WorkshopSelection& a_selection)
	{
		if (!a_recipe) {
			return;
		}
		for (auto* item = a_recipe->conditions.head; item; item = item->next) {
			if (!PCF::PerkConditions::IsPositivePerkCheck(item)) {
				continue;
			}
			++a_selection.perkConditionCount;
			auto* source = static_cast<RE::BGSPerk*>(item->data.functionData.param[0]);
			if (!source) {
				continue;
			}
			const auto* rule = PCF::RuleRegistry::Find(source);
			const auto* display = rule ? PCF::RuleRegistry::GetDisplay(*rule) : nullptr;
			const auto* presentation = rule ? PCF::TextManager::GetConfiguredRequirement(source) : nullptr;
			if (rule) {
				++a_selection.matchedRuleCount;
			}
			a_selection.requirements.push_back({ rule, display, presentation, nullptr, source, nullptr, !PCF::PerkConditions::PlayerPasses(source) });
		}
	}

	// Resolves the one effective requirement source for the selected Workshop node.
	// A PCF-owned custom set replaces the logical recipe's native requirement rows;
	// it must never be appended after the generated/source recipe copy.
	void AddEffectiveRequirements(WorkshopSelection& a_selection)
	{
		const auto findCustom = [](const RE::BGSConstructibleObject* a_recipe) -> const PCF::CustomConditions::ConditionSet* {
			if (!a_recipe) {
				return nullptr;
			}
			const auto* set = PCF::CustomConditions::Find(static_cast<const RE::TESForm*>(a_recipe));
			return set && set->owner == PCF::CustomConditions::OwnerKind::kCrafting ? set : nullptr;
		};

		if (const auto* custom = findCustom(a_selection.recipe)) {
			AddCustomRequirements(*custom, a_selection);
			return;
		}
		if (a_selection.sourceRecipe != a_selection.recipe) {
			if (const auto* custom = findCustom(a_selection.sourceRecipe)) {
				AddCustomRequirements(*custom, a_selection);
				return;
			}
		}

		AddNativeRequirements(a_selection.recipe, a_selection);
		if (a_selection.sourceRecipe != a_selection.recipe) {
			AddNativeRequirements(a_selection.sourceRecipe, a_selection);
		}
	}
	// Finds the active Workshop recipe and selection.
	WorkshopStatus FindWorkshopContext(RE::IMenu* a_menu, WorkshopSelection& a_selection)
	{
		auto* workshop = RE::DynamicCast<RE::WorkshopMenu*>(a_menu);
		if (!workshop || !workshop->workshopMenuBase) {
			return WorkshopStatus::kMenuUnavailable;
		}
		a_selection.row = RE::Workshop::GetCurrentRow();
		a_selection.column = 0;
		auto* node = RE::Workshop::GetSelectedWorkshopMenuNode(a_selection.row, a_selection.column);
		if (!node) {
			return WorkshopStatus::kSelectedNodeNull;
		}
		a_selection.recipe = node->recipe;
		a_selection.sourceRecipe = node->sourceFormListRecipe;
		if (!a_selection.recipe && !a_selection.sourceRecipe) {
			return WorkshopStatus::kRecipeUnavailable;
		}
		return WorkshopStatus::kReady;
	}
	// Builds the current Workshop requirement list.
	WorkshopStatus BuildWorkshopRequirements(RE::IMenu* a_menu, WorkshopSelection& a_selection)
	{
		const auto contextStatus = FindWorkshopContext(a_menu, a_selection);
		if (contextStatus != WorkshopStatus::kReady) {
			return contextStatus;
		}
		a_selection.requirements.reserve(kWorkshopRequirementSlots);
		AddEffectiveRequirements(a_selection);
		MergeDuplicateRequirements(a_selection);
		if (g_workshopArtworkInstalled) {
			for (auto& requirement : a_selection.requirements) {
				if (requirement.custom) {
					requirement.artwork = PCF::CustomConditions::GetArtworkPath(*requirement.custom);
				} else if (requirement.display) {
					requirement.artwork = requirement.display->iconPath.empty() ? nullptr : std::addressof(requirement.display->iconPath);
				} else {
					const auto* artwork = PCF::RuleRegistry::FindSWF(requirement.source);
					requirement.artwork = artwork && !artwork->empty() ? artwork : nullptr;
				}
			}
		}
		return WorkshopStatus::kReady;
	}
	// Checks whether the Workshop selection uses PCF display changes.
	bool HasCustomWorkshopDisplay(const WorkshopSelection& a_selection)
	{
		// A custom-owned set must still replace/clear native UI rows even when every
		// configured condition is hidden quest progression logic.
		return a_selection.customOwned || a_selection.matchedRuleCount != 0 || a_selection.customConditionCount != 0 ||
			std::any_of(a_selection.requirements.begin(), a_selection.requirements.end(), [](const auto& requirement) {
				return requirement.artwork != nullptr;
			});
	}
	// Checks whether the Workshop menu has PCF work to apply.
	bool HasWorkshopChanges(RE::IMenu* a_menu)
	{
		if (!PCF::RuleRegistry::Empty() || PCF::CustomConditions::HasCraftingTargets()) {
			return true;
		}
		if (std::any_of(g_workshopArtwork.begin(), g_workshopArtwork.end(), [](const auto& state) {
			return state.managed || state.nativeDirty;
		})) {
			return true;
		}
		WorkshopSelection selection;
		return g_workshopArtworkInstalled && BuildWorkshopRequirements(a_menu, selection) == WorkshopStatus::kReady &&
			HasCustomWorkshopDisplay(selection);
	}
	// Builds a key for one Workshop artwork choice.
	WorkshopArtworkState GetWorkshopArtworkKey(const WorkshopSelection& a_selection, const WorkshopRequirement& a_requirement)
	{
		WorkshopArtworkState result;
		result.selection = GetSelectionKey(a_selection);
		auto* customForm = a_requirement.custom ? PCF::CustomConditions::GetDisplayForm(*a_requirement.custom) : nullptr;
		result.source = a_requirement.source ? a_requirement.source->GetFormID() : (customForm ? customForm->GetFormID() : 0u);
		auto* form = a_requirement.display ? GetDisplayForm(*a_requirement.display) : (customForm ? customForm : a_requirement.source);
		result.display = form ? form->GetFormID() : 0u;
		if (a_requirement.artwork) {
			result.path = *a_requirement.artwork;
		}
		result.managed = a_requirement.rule || a_requirement.custom || !result.path.empty();
		return result;
	}
	// Checks whether two Workshop artwork choices are the same.
	bool SameWorkshopArtwork(const WorkshopArtworkState& a_left, const WorkshopArtworkState& a_right)
	{
		return a_left.managed == a_right.managed && a_left.selection == a_right.selection &&
			a_left.source == a_right.source && a_left.display == a_right.display && a_left.path == a_right.path;
	}
	// Finds the original Workshop row for a PCF requirement.
	bool FindOriginalWorkshopRow(const Value& a_rows, const WorkshopRequirement& a_requirement, Value& a_row)
	{
		std::uint32_t count = 0;
		if (!a_requirement.source || !ReadIndex(a_rows, "length", count) || count > 256) {
			return false;
		}
		const auto sourceID = a_requirement.source->GetFormID();
		const auto rank = PCF::RuleRegistry::GetRank(a_requirement.source);
		const auto baseID = rank.base ? rank.base->GetFormID() : sourceID;
		bool found = false;
		for (std::uint32_t i = 0; i < count; ++i) {
			Value row;
			std::uint32_t formID = 0;
			std::uint32_t rowRank = 0;
			if (!ReadElement(a_rows, i, row) || !ReadIndex(row, "perkID", formID) ||
				!ReadIndex(row, "perkRank", rowRank)) {
				continue;
			}
			const bool exactMatch = formID == sourceID;
			const bool familyMatch = rank.base && formID == baseID;
			const bool rankMatch = !rank.base || rowRank == rank.rank;
			const bool finalMatch = (exactMatch || familyMatch) && rankMatch;
			if (!finalMatch) {
				continue;
			}
			if (found) {
				return false;
			}
			a_row = row;
			found = true;
		}
		return found;
	}
	// Builds the final Workshop perkData rows from the original rows.
	bool BuildWorkshopRows(RE::IMenu* a_menu, const Value& a_nativeRows,
		const WorkshopSelection& a_selection, Value& a_rows)
	{
		a_menu->uiMovie->CreateArray(&a_rows);
		if (!a_rows.IsArray()) {
			return false;
		}
		const auto count = (std::min)(a_selection.requirements.size(), kWorkshopRequirementSlots);
		for (std::size_t i = 0; i < count; ++i) {
			const auto& requirement = a_selection.requirements[i];
			Value row;
			a_menu->uiMovie->CreateObject(&row);
			if (!row.IsAnyObject()) {
				return false;
			}
			if (requirement.custom) {
				auto* form = PCF::CustomConditions::GetDisplayForm(*requirement.custom);
				if (!form || !requirement.presentation ||
					!row.SetMember("perkName", Value(requirement.presentation->rowLabel.c_str())) ||
					!row.SetMember("perkID", Value(static_cast<double>(form->GetFormID()))) ||
					!row.SetMember("perkRank", Value(requirement.presentation->value)) ||
					!row.SetMember("perkLocked", Value(requirement.locked))) {
					return false;
				}
				const auto swfName = requirement.artwork ? *requirement.artwork + ".swf" : std::string{};
				if (!row.SetMember("swfName", Value(swfName.c_str()))) {
					return false;
				}
			} else {
				Value nativeRow;
				if (!FindOriginalWorkshopRow(a_nativeRows, requirement, nativeRow)) {
					return false;
				}
				for (const auto* field : { "perkName", "perkID", "perkRank", "perkLocked", "swfName" }) {
					Value value;
					if (!nativeRow.GetMember(field, &value) || !row.SetMember(field, value)) {
						return false;
					}
				}
			}
			if ((requirement.rule || requirement.custom) && !row.SetMember("perkLocked", Value(requirement.locked))) {
				return false;
			}
			if (requirement.presentation && requirement.presentation->overrideNativeText) {
				if (!row.SetMember("perkName", Value(requirement.presentation->rowLabel.c_str())) ||
					!row.SetMember("perkRank", Value(requirement.presentation->value))) {
					return false;
				}
			}
			if (requirement.artwork) {
				const auto explicitName = *requirement.artwork + ".swf";
				if (!row.SetMember("swfName", Value(explicitName.c_str()))) {
					return false;
				}
			} else if (requirement.display && !row.SetMember("swfName", Value(""))) {
				return false;
			}
			if (!a_rows.PushBack(row)) {
				return false;
			}
		}
		return true;
	}
	// Loads artwork through the Workshop artwork loader.
	bool ShowWorkshopArtwork(Value& a_loader, const std::string& a_path, const std::string& a_alternate)
	{
		SetNumber(a_loader, "clipScale", 0.29);
		SetNumber(a_loader, "clipAlpha", 1.0);
		const std::array<Value, 2> arguments{ Value(a_path.c_str()), Value(a_alternate.c_str()) };
		return a_loader.Invoke("SWFLoadAlt", nullptr, arguments.data(), static_cast<std::uint32_t>(arguments.size()));
	}
	// Restores the original Workshop artwork request.
	bool RestoreOriginalWorkshopArtwork(Value& a_loader, const Value& a_row)
	{
		std::string path;
		ReadText(a_row, "swfName", path);
		if (!path.empty()) {
			const auto extension = path.find(".swf");
			if (extension == std::string::npos) {
				return false;
			}
			path.resize(extension);
			return ShowWorkshopArtwork(a_loader, path, "");
		}
		std::uint32_t formID = 0;
		if (!ReadIndex(a_row, "perkID", formID)) {
			return false;
		}
		return ShowWorkshopArtwork(a_loader, fmt::format("Components/Vaultboys/Perks/WorkshopPerkClip_{:x}", formID),
			fmt::format("Components/Vaultboys/Perks/PerkClip_{:x}", formID));
	}
	// Catches Workshop perkData updates so PCF can apply its display changes.
	bool WorkshopSetPerkData(Value::ObjectInterface* a_interface, void* a_data, const char* a_name,
		const Value& a_nativeRows, bool a_displayObject)
	{
		const auto native = [&]() {
			return g_workshopSetMember ? g_workshopSetMember(a_interface, a_data, a_name, a_nativeRows, a_displayObject) : false;
		};
		if (!g_workshopArtworkInstalled || g_workshopPublishing || !a_interface || !a_name ||
			std::strcmp(a_name, "perkData") != 0 || !a_nativeRows.IsArray()) {
			return native();
		}
		auto* ui = RE::UI::GetSingleton();
		const auto menu = ui ? ui->GetMenu<RE::WorkshopMenu>() : Scaleform::Ptr<RE::WorkshopMenu>();
		if (!menu || !menu->uiMovie || !menu->workshopMenuBase ||
			reinterpret_cast<Scaleform::GFx::Movie*>(a_interface->movieRoot) != menu->uiMovie.get()) {
			return native();
		}
		struct PerkDataGuard
		{
			// Starts a Workshop perkData update.
			PerkDataGuard() { g_workshopPublishing = true; }
			// Finishes a Workshop perkData update.
			~PerkDataGuard() { g_workshopPublishing = false; }
		} guard;
		WorkshopSelection selection;
		const bool resolved = BuildWorkshopRequirements(menu.get(), selection) == WorkshopStatus::kReady;
		const bool managed = resolved && HasCustomWorkshopDisplay(selection);
		const bool releasing = std::any_of(g_workshopArtwork.begin(), g_workshopArtwork.end(), [](const auto& state) {
			return state.managed || state.nativeDirty;
		});
		if (!managed && !releasing) {
			return native();
		}
		std::uint32_t nativeCount = 0;
		bool hostVisible = true;
		if (managed && ReadIndex(a_nativeRows, "length", nativeCount) && nativeCount == 0 &&
			IsHostVisible(menu.get(), hostVisible) && !hostVisible) {
			return native();
		}
		Value rows(a_nativeRows);
		if (managed && !BuildWorkshopRows(menu.get(), a_nativeRows, selection, rows)) {
			static bool warned = false;
			if (!warned) {
				warned = true;
				spdlog::warn("Workshop artwork: native requirement rows could not be matched; retaining native dispatch");
			}
			for (auto& state : g_workshopArtwork) {
				state.requested = false;
				state.nativeDirty = true;
			}
			return native();
		}
		struct RowPlan
		{
			Value row;
			Value panel;
			Value nameField;
			Value loader;
			std::string title;
			WorkshopArtworkState identity;
			bool nativeLoads{ false };
			bool displaced{ false };
		};
		std::array<RowPlan, kWorkshopRequirementSlots> plan;
		std::uint32_t rowCount = 0;
		if (!ReadIndex(rows, "length", rowCount)) {
			return native();
		}
		const auto count = (std::min)(static_cast<std::size_t>(rowCount), plan.size());
		for (std::size_t i = 0; i < count; ++i) {
			auto& slot = plan[i];
			const auto panelName = kPerkPanelNames[i];
			std::string currentTitle;
			if (!a_interface->GetMember(a_data, panelName.data(), &slot.panel, a_displayObject) || !slot.panel.IsAnyObject() ||
				!slot.panel.GetMember("PerkName_tf", &slot.nameField) || !slot.nameField.IsAnyObject() ||
				!slot.panel.GetMember("PerkLoaderClip_mc", &slot.loader) || !slot.loader.IsAnyObject() ||
				!ReadElement(rows, static_cast<std::uint32_t>(i), slot.row) || !ReadText(slot.row, "perkName", slot.title) ||
				!ReadText(slot.nameField, "text", currentTitle)) {
				return native();
			}
			slot.nativeLoads = currentTitle != slot.title;
			if (managed) {
				slot.identity = GetWorkshopArtworkKey(selection, selection.requirements[i]);
			}
			std::uint32_t token = 0;
			slot.displaced = g_workshopArtwork[i].managed && g_workshopArtwork[i].token != 0 &&
				(!ReadIndex(slot.panel, "__PCFWorkshopArtworkToken", token) || token != g_workshopArtwork[i].token);
		}
		for (std::size_t i = 0; i < count; ++i) {
			auto& slot = plan[i];
			if (slot.identity.managed) {
				SetText(slot.nameField, "text", slot.title);
				std::string actual;
				if (!ReadText(slot.nameField, "text", actual) || actual != slot.title) {
					return false;
				}
				slot.nativeLoads = false;
			}
		}
		const bool result = g_workshopSetMember(a_interface, a_data, a_name, rows, a_displayObject);
		if (!result) {
			for (std::size_t i = 0; i < count; ++i) {
				if (plan[i].identity.managed) {
					SetShownVisible(plan[i].loader, false);
				}
			}
			return false;
		}
		for (std::size_t i = 0; i < count; ++i) {
			auto& slot = plan[i];
			auto& previous = g_workshopArtwork[i];
			if (slot.identity.managed) {
				const bool changed = !SameWorkshopArtwork(previous, slot.identity) || previous.nativeDirty || slot.displaced;
				slot.identity.requested = previous.requested;
				slot.identity.token = previous.token;
				if (changed) {
					if (++g_workshopArtworkToken == 0) {
						++g_workshopArtworkToken;
					}
					slot.identity.token = g_workshopArtworkToken;
					SetNumber(slot.panel, "__PCFWorkshopArtworkToken", static_cast<double>(slot.identity.token));
					std::uint32_t token = 0;
					if (!ReadIndex(slot.panel, "__PCFWorkshopArtworkToken", token) || token != slot.identity.token) {
						slot.identity.token = 0;
					}
					slot.identity.requested = slot.identity.token != 0 && !slot.identity.path.empty() &&
						ShowWorkshopArtwork(slot.loader, slot.identity.path, "");
				}
				SetShownVisible(slot.loader, slot.identity.requested && !slot.identity.path.empty());
				previous = std::move(slot.identity);
			} else {
				if (previous.managed || previous.nativeDirty) {
					const bool current = slot.nativeLoads || RestoreOriginalWorkshopArtwork(slot.loader, slot.row);
					SetShownVisible(slot.loader, current);
				}
				previous = {};
			}
		}
		for (std::size_t i = count; i < plan.size(); ++i) {
			const bool dirty = g_workshopArtwork[i].managed || g_workshopArtwork[i].nativeDirty;
			g_workshopArtwork[i] = {};
			g_workshopArtwork[i].nativeDirty = dirty;
		}
		return result;
	}
	// Applies the final PCF display to one Workshop panel.
	bool UpdatePanel(RE::BSGFxShaderFXTarget& a_panelTarget, const WorkshopRequirement& a_requirement,
		const WorkshopSelection& a_selection, std::size_t a_slot)
	{
		if (!a_requirement.rule && !a_requirement.custom && !a_requirement.artwork) {
			return false;
		}
		Value a_panel(a_panelTarget);
		if (!a_panel.IsAnyObject()) {
			return false;
		}
		bool changed = false;
		Value name;
		Value requirementField;
		Value lock;
		if (!a_panel.GetMember("PerkName_tf", &name) || !name.IsAnyObject() ||
			!a_panel.GetMember("Requires_tf", &requirementField) || !requirementField.IsAnyObject()) {
			return false;
		}
		const auto* presentation = a_requirement.presentation;
		changed = SetBool(a_panel, "visible", true) || changed;
		for (const auto* member : { "PerkName_tf", "Requires_tf" }) {
			Value child;
			if (a_panel.GetMember(member, &child) && child.IsAnyObject()) {
				changed = SetShownVisible(child, true) || changed;
			}
		}
		Value loader;
		if (a_panel.GetMember("PerkLoaderClip_mc", &loader) && loader.IsAnyObject()) {
			const auto identity = GetWorkshopArtworkKey(a_selection, a_requirement);
			const auto& state = g_workshopArtwork[a_slot];
			std::uint32_t token = 0;
			const bool visible = state.requested && !identity.path.empty() && SameWorkshopArtwork(state, identity) &&
				ReadIndex(a_panel, "__PCFWorkshopArtworkToken", token) && token == state.token;
			changed = SetShownVisible(loader, visible) || changed;
		}
		if (presentation && presentation->overrideNativeText) {
			if (!presentation->label.empty()) {
				changed = SetText(name, "text", presentation->label) || changed;
			}
			Value existing;
			std::string prefix;
			if (requirementField.GetMember("text", &existing) && existing.IsString()) {
				const char* value = existing.GetString();
				const std::string_view text(value ? value : "");
				const auto colon = text.find(':');
				if (colon != std::string_view::npos) {
					prefix = text.substr(0, colon + 1);
				}
			}
			if (prefix.empty()) {
				changed = SetText(requirementField, "text", presentation->valueText) || changed;
			} else {
				std::string text;
				text.reserve(prefix.size() + presentation->valueText.size() + 1);
				text.append(prefix);
				text.push_back(' ');
				text.append(presentation->valueText);
				changed = SetText(requirementField, "text", text) || changed;
			}
		}
		if (a_panel.GetMember("PerkLock_mc", &lock) && lock.IsAnyObject()) {
			changed = SetShownVisible(lock, a_requirement.locked) || changed;
		}
		return changed;
	}
	// Clears Workshop slots that are no longer needed.
	bool ClearUnusedSlots(RE::IMenu* a_menu, std::size_t a_firstUnused)
	{
		auto* workshop = RE::DynamicCast<RE::WorkshopMenu*>(a_menu);
		if (!workshop || !workshop->workshopMenuBase) {
			return false;
		}
		const std::array<RE::BSGFxShaderFXTarget*, kWorkshopRequirementSlots> panels{
			workshop->workshopMenuBase->perkPanel01.get(), workshop->workshopMenuBase->perkPanel02.get()
		};
		bool changed = false;
		for (std::size_t i = (std::min)(a_firstUnused, panels.size()); i < panels.size(); ++i) {
			if (!panels[i]) {
				continue;
			}
			changed = SetSlotVisibility(a_menu, i, false) || changed;
			Value panel(*panels[i]);
			for (const auto* member : { "PerkName_tf", "Requires_tf", "PerkLock_mc", "PerkLoaderClip_mc" }) {
				Value child;
				if (panel.GetMember(member, &child) && child.IsAnyObject()) {
					changed = SetOwnedVisible(child, false) || changed;
					if (std::string_view(member) == "PerkName_tf" || std::string_view(member) == "Requires_tf") {
						changed = SetText(child, "text", "") || changed;
					}
				}
			}
		}
		return changed;
	}
	// Temporarily hides the PCF Workshop display.
	bool HideWorkshopDisplay(RE::IMenu* a_menu)
	{
		bool changed = false;
		for (std::size_t i = 0; i < kWorkshopRequirementSlots; ++i) {
			changed = SetSlotVisibility(a_menu, i, false) || changed;
		}
		return changed;
	}
	// Shows the prepared requirements on the Workshop cards.
	bool ShowWorkshopDisplay(RE::IMenu* a_menu, const WorkshopSelection& a_selection, std::size_t& a_panels, std::size_t& a_changed)
	{
		auto* workshop = RE::DynamicCast<RE::WorkshopMenu*>(a_menu);
		if (!workshop || !workshop->workshopMenuBase) {
			return false;
		}
		auto* base = workshop->workshopMenuBase.get();
		const std::array<RE::BSGFxShaderFXTarget*, 2> panels{ base->perkPanel01.get(), base->perkPanel02.get() };
		for (std::size_t i = 0; i < panels.size(); ++i) {
			if (panels[i]) {
				++a_panels;
			}
		}
		const auto count = (std::min)(panels.size(), a_selection.requirements.size());
		for (std::size_t i = 0; i < count; ++i) {
			if (!panels[i]) {
				continue;
			}
			bool changed = SetSlotVisibility(a_menu, i, true);
			changed = UpdatePanel(*panels[i], a_selection.requirements[i], a_selection, i) || changed;
			if (changed) {
				++a_changed;
			}
		}
		if (ClearUnusedSlots(a_menu, a_selection.requirements.size())) {
			++a_changed;
		}
		return true;
	}
	// Updates the Workshop display from the current game state.
	WorkshopStatus UpdateWorkshop(RE::IMenu* a_menu)
	{
		WorkshopSelection selection;
		auto status = BuildWorkshopRequirements(a_menu, selection);
		std::size_t panels = 0;
		std::size_t changed = 0;
		if (status != WorkshopStatus::kReady) {
			ReleaseAllSlots(a_menu);
			return status;
		}
		if (selection.requirements.empty()) {
			ReleaseAllSlots(a_menu);
			return WorkshopStatus::kNoPerkRequirement;
		}
		if (!HasCustomWorkshopDisplay(selection)) {
			ReleaseAllSlots(a_menu);
			return WorkshopStatus::kNoRegistryMatch;
		}
		bool hostVisible = true;
		if (IsHostVisible(a_menu, hostVisible) && !hostVisible) {
			status = HideWorkshopDisplay(a_menu) ? WorkshopStatus::kApplied : WorkshopStatus::kAlreadyCurrent;
			SavePresentation(a_menu, selection, status);
			return status;
		}
		if (!ShowWorkshopDisplay(a_menu, selection, panels, changed) || panels == 0) {
			status = WorkshopStatus::kPanelsUnavailable;
		} else {
			status = changed ? WorkshopStatus::kApplied : WorkshopStatus::kAlreadyCurrent;
		}
		SavePresentation(a_menu, selection, status);
		return status;
	}
	// Repairs the Workshop display if the menu layout changes.
	bool CheckWorkshopDisplay(RE::IMenu* a_menu, bool a_final)
	{
		if (!g_workshopPresentationVerifyPending.load(std::memory_order_acquire)) {
			return false;
		}
		const auto finish = [a_final]() {
			if (a_final) {
				g_workshopPresentationVerifyPending.store(false, std::memory_order_release);
			}
		};

		const auto expectedKey = g_workshopDisplayKey.load(std::memory_order_relaxed);
		std::uint32_t displayKey = 0;
		if (expectedKey == 0 || !GetPresentationKey(a_menu, displayKey) ||
			displayKey == expectedKey) {
			finish();
			return false;
		}

		WorkshopSelection selection;
		const auto contextStatus = FindWorkshopContext(a_menu, selection);
		if (contextStatus != WorkshopStatus::kReady) {
			finish();
			return false;
		}

		const auto selectionKey = GetSelectionKey(selection);
		const auto expectedSelectionKey = g_workshopPresentationSelectionKey.load(std::memory_order_acquire);
		if (selectionKey == 0 || expectedSelectionKey == 0 || selectionKey != expectedSelectionKey) {
			finish();
			return false;
		}

		const auto status = UpdateWorkshop(a_menu);
		finish();
		return status == WorkshopStatus::kApplied || status == WorkshopStatus::kAlreadyCurrent;
	}
	// Handles Workshop input that can change the display.
	void WorkshopInputHook(RE::BSInputEventUser* a_user, const RE::ButtonEvent* a_event)
	{
		auto& hook = PCF::UIHookState::GetHooks()[kWorkshopIndex];
		if (!hook.active) {
			if (g_workshopButtonEvent) {
				g_workshopButtonEvent(a_user, a_event);
			}
			return;
		}
		const DepthGuard guard(g_workshopButtonDepth);
		if (!guard.IsOutermost()) {
			g_workshopButtonEvent(a_user, a_event);
			return;
		}

		auto* workshop = a_user ? RE::DynamicCast<RE::WorkshopMenu*>(a_user) : nullptr;
		RE::IMenu* menu = workshop;
		const bool active = menu && HasWorkshopChanges(menu);
		if (active && a_event) {
			g_workshopPresentationVerifyPending.store(true, std::memory_order_release);
		}

		g_workshopButtonEvent(a_user, a_event);

		if (active && a_event) {
			CheckWorkshopDisplay(menu, false);
		}
	}
	// Handles Workshop messages that can change the display.
	RE::UI_MESSAGE_RESULTS WorkshopMessageHook(RE::IMenu* a_menu, RE::UIMessage& a_message)
	{
		auto& hook = PCF::UIHookState::GetHooks()[kWorkshopIndex];
		if (!hook.processMessage) {
			return RE::UI_MESSAGE_RESULTS::kPassOn;
		}
		const DepthGuard guard(g_workshopMessageDepth);
		const auto result = hook.processMessage(a_menu, a_message);
		if (!hook.active) {
			return result;
		}
		if (!guard.IsOutermost() || !a_menu || !HasWorkshopChanges(a_menu)) {
			return result;
		}
		switch (*a_message.type) {
		case RE::UI_MESSAGE_TYPE::kReshow:
		case RE::UI_MESSAGE_TYPE::kScaleformEvent:
		case RE::UI_MESSAGE_TYPE::kUserEvent:
		case RE::UI_MESSAGE_TYPE::kInventoryUpdate:
		case RE::UI_MESSAGE_TYPE::kUpdateController:
			g_workshopPresentationVerifyPending.store(true, std::memory_order_release);
			break;
		default:
			break;
		}
		CheckWorkshopDisplay(a_menu, false);
		return result;
	}
	// Handles Workshop Scaleform calls that can change the display.
	void WorkshopCallHook(RE::IMenu* a_menu, const Params& a_params)
	{
		auto& hook = PCF::UIHookState::GetHooks()[kWorkshopIndex];
		if (!hook.call) {
			return;
		}
		if (!hook.active) {
			hook.call(a_menu, a_params);
			return;
		}
		const DepthGuard guard(g_workshopCallDepth);
		if (!guard.IsOutermost() || g_workshopAdvanceDepth != 0) {
			PCF::UIHookState::GetHooks()[kWorkshopIndex].call(a_menu, a_params);
			return;
		}
		PCF::UIHookState::GetHooks()[kWorkshopIndex].call(a_menu, a_params);
		if (!a_menu || !HasWorkshopChanges(a_menu)) {
			return;
		}
		const auto status = UpdateWorkshop(a_menu);
		QueueFinalCheck(status);
	}
	// Checks the Workshop display while the menu advances.
	void WorkshopAdvanceHook(RE::IMenu* a_menu, float a_delta, std::uint64_t a_time)
	{
		auto& hook = PCF::UIHookState::GetHooks()[kWorkshopIndex];
		if (!hook.advance) {
			return;
		}
		if (!hook.active) {
			hook.advance(a_menu, a_delta, a_time);
			return;
		}
		const DepthGuard guard(g_workshopAdvanceDepth);
		if (!guard.IsOutermost()) {
			PCF::UIHookState::GetHooks()[kWorkshopIndex].advance(a_menu, a_delta, a_time);
			return;
		}
		if (!a_menu || !HasWorkshopChanges(a_menu)) {
			PCF::UIHookState::GetHooks()[kWorkshopIndex].advance(a_menu, a_delta, a_time);
			return;
		}
		if (!a_menu->hasDoneFirstAdvanceMovie) {
			ResetWorkshopState();
		}

		WorkshopSelection before;
		const auto beforeStatus = FindWorkshopContext(a_menu, before);
		const auto beforeKey = GetContextKey(a_menu, before, beforeStatus);
		const auto previousKey = g_workshopAdvanceContext.load(std::memory_order_relaxed);
		const bool selectionChanged = beforeKey != previousKey;
		if (selectionChanged) {
			const auto status = UpdateWorkshop(a_menu);
			QueueFinalCheck(status);
		}

		PCF::UIHookState::GetHooks()[kWorkshopIndex].advance(a_menu, a_delta, a_time);

		WorkshopSelection after;
		const auto afterStatus = FindWorkshopContext(a_menu, after);
		const auto afterKey = GetContextKey(a_menu, after, afterStatus);
		if (selectionChanged || afterKey != beforeKey || afterKey != previousKey) {
			const auto status = UpdateWorkshop(a_menu);
			QueueFinalCheck(status);
		}
		CheckWorkshopDisplay(a_menu, false);
		g_workshopAdvanceContext.store(afterKey, std::memory_order_relaxed);
	}
	// Finishes the Workshop display before it is shown.
	void WorkshopDisplayHook(RE::IMenu* a_menu)
	{
		auto& hook = PCF::UIHookState::GetHooks()[kWorkshopIndex];
		if (!hook.preDisplay) {
			return;
		}
		if (!hook.active) {
			hook.preDisplay(a_menu);
			return;
		}
		const DepthGuard guard(g_workshopPreDisplayDepth);
		if (!guard.IsOutermost()) {
			PCF::UIHookState::GetHooks()[kWorkshopIndex].preDisplay(a_menu);
			return;
		}
		if (a_menu && HasWorkshopChanges(a_menu) &&
			g_workshopPreDisplayPending.exchange(false, std::memory_order_acq_rel)) {
			UpdateWorkshop(a_menu);
		}
		PCF::UIHookState::GetHooks()[kWorkshopIndex].preDisplay(a_menu);
		if (a_menu && HasWorkshopChanges(a_menu)) {
			CheckWorkshopDisplay(a_menu, true);
		}
	}
	// Installs and verifies one Workshop vtable callback while preserving its native chain.
	template <class Function>
	bool InstallWorkshopVtableHook(std::uintptr_t a_vtable, std::size_t a_slot, Function a_replacement, Function& a_original)
	{
		const auto replacement = reinterpret_cast<std::uintptr_t>(a_replacement);
		const auto current = PCF::NativeHooks::ReadVtableSlot(a_vtable, a_slot);
		if (current == replacement) {
			return a_original != nullptr;
		}
		if (!current) {
			return false;
		}
		REL::Relocation<std::uintptr_t> vtable{ a_vtable };
		const auto previous = vtable.WriteVirtualCall(a_slot, reinterpret_cast<std::uintptr_t>(a_replacement));
		a_original = reinterpret_cast<Function>(previous);
		return a_original != nullptr && PCF::NativeHooks::IsVtableSlotSet(a_vtable, a_slot, replacement);
	}

	// Installs the Workshop menu and input hooks.
	bool InstallWorkshopHooks(std::uintptr_t a_vtable, std::uintptr_t a_inputVtable)
	{
		auto& hook = PCF::UIHookState::GetHooks()[kWorkshopIndex];
		return InstallWorkshopVtableHook(a_inputVtable, kButtonEventSlot, &WorkshopInputHook, g_workshopButtonEvent) &&
			InstallWorkshopVtableHook(a_vtable, kCallSlot, &WorkshopCallHook, hook.call) &&
			InstallWorkshopVtableHook(a_vtable, kProcessMessageSlot, &WorkshopMessageHook, hook.processMessage) &&
			InstallWorkshopVtableHook(a_vtable, kAdvanceSlot, &WorkshopAdvanceHook, hook.advance) &&
			InstallWorkshopVtableHook(a_vtable, kPreDisplaySlot, &WorkshopDisplayHook, hook.preDisplay);
	}
	// Finds the Workshop call that writes perkData.
	std::uintptr_t FindPerkDataCall(std::uintptr_t a_owner, std::uintptr_t a_append, std::uintptr_t a_setMember)
	{
		const auto text = REL::Module::GetSingleton()->GetSection(REL::MODULE_SECTION_NAME_TEXT);
		const auto data = REL::Module::GetSingleton()->GetSection(REL::MODULE_SECTION_NAME_RDATA);
		if (a_owner < text.GetAddress() || a_owner - text.GetAddress() >= text.GetSize()) {
			return 0;
		}
		const auto available = (std::min)(text.GetSize() - (a_owner - text.GetAddress()), std::size_t{ 65536 });
		ZydisDecoder decoder;
		if (!ZYAN_SUCCESS(ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64))) {
			return 0;
		}
		std::uintptr_t match = 0;
		std::uint32_t pending = 0;
		bool appended = false;
		for (std::size_t offset = 0; offset < available;) {
			ZydisDecodedInstruction instruction;
			ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
			const auto address = a_owner + offset;
			if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, reinterpret_cast<const void*>(address), available - offset,
				&instruction, operands)) || instruction.length == 0) {
				return 0;
			}
			if (instruction.mnemonic == ZYDIS_MNEMONIC_RET) {
				return appended ? match : 0;
			}
			if (pending != 0) {
				--pending;
			}
			for (std::uint8_t i = 0; i < instruction.operand_count_visible; ++i) {
				if (operands[i].type == ZYDIS_OPERAND_TYPE_REGISTER &&
					(operands[i].reg.value == ZYDIS_REGISTER_R8 || operands[i].reg.value == ZYDIS_REGISTER_R8D) &&
					(operands[i].actions & ZYDIS_OPERAND_ACTION_MASK_WRITE) != 0) {
					pending = 0;
				}
			}
			ZyanU64 target = 0;
			if (instruction.mnemonic == ZYDIS_MNEMONIC_LEA && instruction.operand_count_visible == 2 &&
				operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER && operands[0].reg.value == ZYDIS_REGISTER_R8 &&
				operands[1].type == ZYDIS_OPERAND_TYPE_MEMORY && operands[1].mem.base == ZYDIS_REGISTER_RIP &&
				ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&instruction, &operands[1], address, &target)) &&
				target >= data.GetAddress() && target - data.GetAddress() < data.GetSize() &&
				data.GetSize() - (target - data.GetAddress()) >= sizeof("perkData") &&
				std::memcmp(reinterpret_cast<const void*>(target), "perkData", sizeof("perkData")) == 0) {
				pending = 16;
			}
			if (instruction.mnemonic == ZYDIS_MNEMONIC_CALL) {
				const bool direct = instruction.length == 5 && operands[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE &&
					operands[0].imm.is_relative &&
					ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&instruction, &operands[0], address, &target));
				if (direct && target == a_append) {
					appended = true;
				}
				if (direct && target == a_setMember && pending != 0 && appended) {
					if (match != 0) {
						return 0;
					}
					match = address;
				}
				pending = 0;
			} else if (instruction.meta.category == ZYDIS_CATEGORY_COND_BR || instruction.meta.category == ZYDIS_CATEGORY_UNCOND_BR) {
				pending = 0;
			}
			offset += instruction.length;
		}
		return 0;
	}

	using PerkRankFunction = std::uint8_t (*)(RE::Actor*, RE::BGSPerk*);
	PerkRankFunction g_workshopPerkRank{ nullptr };
	RE::TESForm* const* g_workshopPerkField{ nullptr };
	std::atomic<bool> g_workshopPerkHookInstalled{ false };

	// Checks a native Workshop perk requirement through the configured replacement rule.
	std::uint8_t WorkshopPerkRankHook(RE::Actor* a_actor, RE::BGSPerk* a_perk)
	{
		const auto rank = g_workshopPerkRank(a_actor, a_perk);
		if (!g_workshopPerkHookInstalled.load(std::memory_order_acquire) || !g_workshopPerkField ||
			!a_perk || a_perk != *g_workshopPerkField || a_actor != RE::PlayerCharacter::GetSingleton() ||
			!PCF::RuleRegistry::Find(a_perk)) {
			return rank;
		}
		return PCF::PerkConditions::PlayerPasses(a_perk) ? (std::max)(rank, std::uint8_t{ 1 }) : std::uint8_t{ 0 };
	}

	// Checks that an address range is readable within the loaded game image.
	bool IsInGameImage(std::uintptr_t a_address, std::size_t a_size)
	{
		const auto base = reinterpret_cast<std::uintptr_t>(::GetModuleHandleW(nullptr));
		if (!base || !a_size || a_address < base) {
			return false;
		}
		const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
		const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
		const std::size_t size = nt->OptionalHeader.SizeOfImage;
		if (a_size > size || a_address - base > size - a_size) {
			return false;
		}
		MEMORY_BASIC_INFORMATION info{};
		if (!::VirtualQuery(reinterpret_cast<const void*>(a_address), &info, sizeof(info)) ||
			info.State != MEM_COMMIT || info.AllocationBase != reinterpret_cast<void*>(base) ||
			(info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0 ||
			(info.Protect & 0xFF) == PAGE_EXECUTE) {
			return false;
		}
		const auto region = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
		return a_address >= region && a_size <= info.RegionSize && a_address - region <= info.RegionSize - a_size;
	}

	// Gets the end of a native function from its chained unwind entries.
	std::uintptr_t GetFunctionEnd(std::uintptr_t a_function)
	{
		constexpr std::uint8_t kChainInfo = 0x04;
		constexpr std::size_t kMaxEntries = 64;
		const auto base = reinterpret_cast<std::uintptr_t>(::GetModuleHandleW(nullptr));
		if (!base || !IsInGameImage(a_function, 1)) {
			return 0;
		}
		const auto getRoot = [base](const RUNTIME_FUNCTION* a_entry) {
			for (std::size_t i = 0; a_entry && i < kMaxEntries; ++i) {
				if (!IsInGameImage(reinterpret_cast<std::uintptr_t>(a_entry), sizeof(*a_entry)) ||
					a_entry->BeginAddress >= a_entry->EndAddress ||
					!IsInGameImage(base + a_entry->BeginAddress, a_entry->EndAddress - a_entry->BeginAddress) ||
					!IsInGameImage(base + a_entry->UnwindData, 4)) {
					return std::uintptr_t{ 0 };
				}
				const auto* unwind = reinterpret_cast<const std::uint8_t*>(base + a_entry->UnwindData);
				if (((unwind[0] >> 3) & kChainInfo) == 0) {
					return base + a_entry->BeginAddress;
				}
				a_entry = reinterpret_cast<const RUNTIME_FUNCTION*>(unwind + 4 + ((unwind[2] + 1) & ~1) * 2);
			}
			return std::uintptr_t{ 0 };
		};
		std::uintptr_t end = a_function;
		for (std::size_t i = 0; i < kMaxEntries; ++i) {
			DWORD64 imageBase = 0;
			const auto* entry = ::RtlLookupFunctionEntry(end, &imageBase, nullptr);
			if (!entry) {
				return end == a_function ? 0 : end;
			}
			const auto root = imageBase == base ? getRoot(entry) : 0;
			if (!root) {
				return 0;
			}
			if (root != a_function) {
				return end == a_function ? 0 : end;
			}
			if (base + entry->BeginAddress != end || base + entry->EndAddress <= end) {
				return 0;
			}
			end = base + entry->EndAddress;
		}
		return 0;
	}

	// Finds the single player perk-rank check in one native Workshop function and the global its perk is read from.
	std::uintptr_t FindPerkRankCall(std::uintptr_t a_function, std::uintptr_t a_perkRank, std::uintptr_t& a_perkField)
	{
		constexpr std::int64_t kNoSlot = INT64_MIN;
		enum class Kind : std::uint8_t { kUnknown, kGlobal, kStack };
		struct Value
		{
			constexpr Value(Kind a_kind = Kind::kUnknown, std::int64_t a_value = 0, std::int64_t a_slot = INT64_MIN) noexcept :
				kind(a_kind), value(a_value), slot(a_slot)
			{}

			Kind kind;
			std::int64_t value;
			std::int64_t slot;
		};
		a_perkField = 0;
		const auto text = REL::Module::GetSingleton()->GetSection(REL::MODULE_SECTION_NAME_TEXT);
		const auto end = GetFunctionEnd(a_function);
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player || a_function < text.GetAddress() || end <= a_function || end - text.GetAddress() > text.GetSize()) {
			return 0;
		}
		ZydisDecoder decoder;
		if (!ZYAN_SUCCESS(ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64))) {
			return 0;
		}
		ZydisDecodedInstruction instruction;
		ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
		const auto decode = [&](std::uintptr_t a_address) {
			return ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, reinterpret_cast<const void*>(a_address), end - a_address,
				&instruction, operands)) && instruction.length != 0;
		};
		const auto indexOf = [](ZydisRegister a_register) {
			const auto full = ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, a_register);
			return full >= ZYDIS_REGISTER_RAX && full <= ZYDIS_REGISTER_R15 ? static_cast<std::size_t>(full - ZYDIS_REGISTER_RAX) : std::size_t{ 16 };
		};
		const auto isBranch = [&]() {
			return instruction.meta.category == ZYDIS_CATEGORY_COND_BR || instruction.meta.category == ZYDIS_CATEGORY_UNCOND_BR;
		};

		// Branch targets end straight-line tracking; frame registers set by the prologue survive them.
		std::vector<std::uintptr_t> targets;
		std::array<std::uint32_t, 16> bodyWrites{};
		bool body = false;
		for (auto address = a_function; address < end; address += instruction.length) {
			if (!decode(address)) {
				return 0;
			}
			ZyanU64 target = 0;
			if (isBranch()) {
				if (instruction.operand_count_visible == 0 || operands[0].type != ZYDIS_OPERAND_TYPE_IMMEDIATE) {
					return 0;
				}
				if (ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&instruction, &operands[0], address, &target))) {
					targets.push_back(static_cast<std::uintptr_t>(target));
				}
			}
			for (std::uint8_t i = 0; body && instruction.mnemonic != ZYDIS_MNEMONIC_POP && i < instruction.operand_count; ++i) {
				if (operands[i].type == ZYDIS_OPERAND_TYPE_REGISTER && (operands[i].actions & ZYDIS_OPERAND_ACTION_MASK_WRITE) != 0) {
					if (const auto index = indexOf(operands[i].reg.value); index < bodyWrites.size()) {
						++bodyWrites[index];
					}
				}
			}
			body = body || isBranch() || instruction.mnemonic == ZYDIS_MNEMONIC_CALL || instruction.mnemonic == ZYDIS_MNEMONIC_RET;
		}
		std::ranges::sort(targets);

		constexpr std::size_t rdx = ZYDIS_REGISTER_RDX - ZYDIS_REGISTER_RAX;
		constexpr std::size_t rsp = ZYDIS_REGISTER_RSP - ZYDIS_REGISTER_RAX;
		constexpr std::array<ZydisRegister, 7> volatiles{ ZYDIS_REGISTER_RAX, ZYDIS_REGISTER_RCX, ZYDIS_REGISTER_RDX,
			ZYDIS_REGISTER_R8, ZYDIS_REGISTER_R9, ZYDIS_REGISTER_R10, ZYDIS_REGISTER_R11 };
		std::array<Value, 16> registers{};
		registers[rsp] = { Kind::kStack, 0 };
		std::optional<std::array<Value, 16>> frame;
		std::vector<std::pair<std::int64_t, std::uintptr_t>> stores;
		std::vector<std::pair<std::int64_t, std::int64_t>> clobbered;
		bool unboundedStackWrite = false;
		bool restart = false;
		std::uintptr_t match = 0;
		std::int64_t matchSlot = kNoSlot;
		std::uint32_t stage = 0;
		bool expectTest = false;
		const auto isStackBase = [&](const ZydisDecodedOperand& a_operand) {
			const auto base = indexOf(a_operand.mem.base);
			return base < registers.size() && registers[base].kind == Kind::kStack;
		};
		const auto stackAddress = [&](const ZydisDecodedOperand& a_operand) -> std::optional<std::int64_t> {
			const auto base = indexOf(a_operand.mem.base);
			if (base >= registers.size() || registers[base].kind != Kind::kStack) {
				return std::nullopt;
			}
			if (a_operand.mem.index != ZYDIS_REGISTER_NONE) {
				return std::nullopt;
			}
			return registers[base].value + a_operand.mem.disp.value;
		};
		for (auto address = a_function; address < end; address += instruction.length) {
			if (!decode(address)) {
				return 0;
			}
			if (std::exchange(restart, false) || std::ranges::binary_search(targets, address)) {
				for (std::size_t i = 0; i < registers.size(); ++i) {
					const bool keep = frame && (i == rsp || ((*frame)[i].kind == Kind::kStack && bodyWrites[i] == 0));
					registers[i] = keep ? (*frame)[i] : (i == rsp ? registers[i] : Value{});
				}
				stage = 0;
			}
			const auto isRegister = [&](std::uint8_t a_index, ZydisRegister a_register) {
				return a_index < instruction.operand_count_visible && operands[a_index].type == ZYDIS_OPERAND_TYPE_REGISTER && operands[a_index].reg.value == a_register;
			};
			if (std::exchange(expectTest, false) &&
				(instruction.mnemonic != ZYDIS_MNEMONIC_TEST || !isRegister(0, ZYDIS_REGISTER_AL) || !isRegister(1, ZYDIS_REGISTER_AL))) {
				return 0;
			}
			ZyanU64 target = 0;
			const bool isCall = instruction.mnemonic == ZYDIS_MNEMONIC_CALL;
			const bool isMove = instruction.mnemonic == ZYDIS_MNEMONIC_MOV && instruction.operand_count_visible == 2;
			const bool loadsPlayer = isMove && isRegister(0, ZYDIS_REGISTER_RCX) &&
				operands[1].type == ZYDIS_OPERAND_TYPE_MEMORY && operands[1].mem.base == ZYDIS_REGISTER_RIP &&
				ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&instruction, &operands[1], address, &target)) &&
				IsInGameImage(static_cast<std::uintptr_t>(target), sizeof(void*)) &&
				*reinterpret_cast<RE::PlayerCharacter* const*>(target) == player;
			const bool loadsPerk = isMove && isRegister(0, ZYDIS_REGISTER_RDX) && operands[1].type == ZYDIS_OPERAND_TYPE_REGISTER &&
				ZydisRegisterGetWidth(ZYDIS_MACHINE_MODE_LONG_64, operands[1].reg.value) == 64;
			const bool callsPerkRank = isCall && instruction.length == 5 && instruction.operand_count_visible != 0 &&
				operands[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE && operands[0].imm.is_relative &&
				ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&instruction, &operands[0], address, &target)) && target == a_perkRank;
			const auto previous = std::exchange(stage, 0u);
			if (loadsPlayer) {
				stage = 1;
			} else if (previous == 1 && loadsPerk) {
				stage = 2;
			} else if (callsPerkRank) {
				const auto& perk = registers[rdx];
				if (previous != 2 || match || perk.kind != Kind::kGlobal) {
					return 0;
				}
				match = address;
				matchSlot = perk.slot;
				a_perkField = static_cast<std::uintptr_t>(perk.value);
				expectTest = true;
			}

			// Track 64-bit values loaded from RIP-relative globals through registers and 8-byte stack slots.
			const bool wide = instruction.operand_count_visible != 0 && operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER && operands[0].size == 64;
			const auto destination = wide ? indexOf(operands[0].reg.value) : std::size_t{ 16 };
			std::optional<Value> tracked;
			if (isMove && wide && destination < registers.size()) {
				if (operands[1].type == ZYDIS_OPERAND_TYPE_REGISTER && operands[1].size == 64) {
					const auto source = indexOf(operands[1].reg.value);
					if (source < registers.size()) {
						tracked = registers[source];
					}
				} else if (operands[1].type == ZYDIS_OPERAND_TYPE_MEMORY && operands[1].mem.base == ZYDIS_REGISTER_RIP &&
						   operands[1].mem.index == ZYDIS_REGISTER_NONE && operands[1].size == 64 &&
						   ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&instruction, &operands[1], address, &target))) {
					tracked = Value{ Kind::kGlobal, static_cast<std::int64_t>(target) };
				} else if (operands[1].type == ZYDIS_OPERAND_TYPE_MEMORY && operands[1].size == 64) {
					if (const auto slot = stackAddress(operands[1])) {
						tracked = Value{ Kind::kUnknown, 0, *slot };
						for (const auto& [offset, source] : stores) {
							if (offset == *slot && source) {
								tracked = Value{ Kind::kGlobal, static_cast<std::int64_t>(source), *slot };
							}
						}
					}
				}
			} else if (instruction.mnemonic == ZYDIS_MNEMONIC_LEA && instruction.operand_count_visible == 2 &&
				wide && destination < registers.size() && operands[1].type == ZYDIS_OPERAND_TYPE_MEMORY) {
				unboundedStackWrite = unboundedStackWrite || (isStackBase(operands[1]) && operands[1].mem.index != ZYDIS_REGISTER_NONE);
				if (const auto slot = stackAddress(operands[1])) {
					tracked = Value{ Kind::kStack, *slot };
					if (destination != rsp) {
						clobbered.emplace_back(*slot, *slot + 8);
					}
				}
			}
			for (std::uint8_t i = 0; i < instruction.operand_count; ++i) {
				const auto& operand = operands[i];
				if ((operand.actions & ZYDIS_OPERAND_ACTION_MASK_WRITE) == 0) {
					continue;
				}
				if (operand.type == ZYDIS_OPERAND_TYPE_MEMORY) {
					unboundedStackWrite = unboundedStackWrite || (isStackBase(operand) && operand.mem.index != ZYDIS_REGISTER_NONE);
					if (const auto slot = stackAddress(operand)) {
						if (isMove && i == 0 && operand.size == 64 && operands[1].type == ZYDIS_OPERAND_TYPE_REGISTER) {
							const auto index = indexOf(operands[1].reg.value);
							const auto source = index < registers.size() ? registers[index] : Value{};
							stores.emplace_back(*slot, source.kind == Kind::kGlobal ? static_cast<std::uintptr_t>(source.value) : 0);
						} else {
							clobbered.emplace_back(*slot, *slot + (std::max)(operand.size / 8, 1));
						}
					}
				} else if (operand.type == ZYDIS_OPERAND_TYPE_REGISTER) {
					const auto index = indexOf(operand.reg.value);
					if (index != rsp) {
						if (index < registers.size()) {
							registers[index] = {};
						}
						continue;
					}
					auto& stack = registers[rsp];
					const bool adjusts = (instruction.mnemonic == ZYDIS_MNEMONIC_SUB || instruction.mnemonic == ZYDIS_MNEMONIC_ADD) &&
						i == 0 && instruction.operand_count_visible == 2 && operands[1].type == ZYDIS_OPERAND_TYPE_IMMEDIATE;
					if (instruction.mnemonic == ZYDIS_MNEMONIC_PUSH) {
						stack.value -= 8;
					} else if (instruction.mnemonic == ZYDIS_MNEMONIC_POP) {
						stack.value += 8;
					} else if (adjusts) {
						const auto amount = operands[1].imm.value.s;
						stack.value += instruction.mnemonic == ZYDIS_MNEMONIC_SUB ? -amount : amount;
					} else if (!isCall && instruction.mnemonic != ZYDIS_MNEMONIC_RET && (!tracked || tracked->kind != Kind::kStack)) {
						return 0;
					}
				}
			}
			if (tracked) {
				registers[destination] = *tracked;
			}
			if (isCall) {
				clobbered.emplace_back(registers[rsp].value, registers[rsp].value + 32);
				for (const auto value : volatiles) {
					registers[indexOf(value)] = {};
				}
			}
			if (!frame && (isCall || isBranch() || instruction.mnemonic == ZYDIS_MNEMONIC_RET)) {
				frame = registers;
				for (const auto value : volatiles) {
					(*frame)[indexOf(value)] = {};
				}
			}
			restart = instruction.mnemonic == ZYDIS_MNEMONIC_RET || instruction.meta.category == ZYDIS_CATEGORY_UNCOND_BR;
		}
		if (expectTest || !match || !IsInGameImage(a_perkField, sizeof(void*))) {
			return 0;
		}
		if (matchSlot != kNoSlot) {
			if (unboundedStackWrite) {
				return 0;
			}
			for (const auto& [offset, source] : stores) {
				if (offset > matchSlot - 8 && offset < matchSlot + 8 && (offset != matchSlot || source != a_perkField)) {
					return 0;
				}
			}
			for (const auto& [first, last] : clobbered) {
				if (first < matchSlot + 8 && last > matchSlot) {
					return 0;
				}
			}
		}
		return match;
	}

	// Gets the live form field of the native WorkshopLocalLeaderPerk_DO default object.
	RE::TESForm* const* GetWorkshopPerkField(std::uintptr_t a_field)
	{
		// BGSDefaultObject layout read from the 1.11.240 constructor: form +0x20, form type +0x28, name +0x30.
		constexpr std::uintptr_t formOffset = 0x20;
		constexpr std::uintptr_t typeOffset = 0x28;
		constexpr std::uintptr_t nameOffset = 0x30;
		const auto object = a_field - formOffset;
		if (a_field < formOffset || !IsInGameImage(object, nameOffset + sizeof(RE::BSFixedString))) {
			return nullptr;
		}
		const auto* defaultObject = reinterpret_cast<const RE::TESForm*>(object);
		if (defaultObject->GetFormType() != RE::FormType::kDefaultObject ||
			*reinterpret_cast<const std::uint8_t*>(object + typeOffset) != static_cast<std::uint8_t>(RE::FormType::kPerk)) {
			return nullptr;
		}
		const char* name = reinterpret_cast<const RE::BSFixedString*>(object + nameOffset)->c_str();
		auto* const* field = reinterpret_cast<RE::TESForm* const*>(a_field);
		if (!name || _stricmp(name, "WorkshopLocalLeaderPerk_DO") != 0 || !*field || !(*field)->As<RE::BGSPerk>()) {
			return nullptr;
		}
		return field;
	}

	// Routes the native Workshop supply line perk checks through the configured replacement rules.
	bool InstallWorkshopPerkHook(std::uintptr_t a_buttonEvent)
	{
		constexpr std::size_t absoluteJumpSize = 14;
		if (g_workshopPerkHookInstalled.load(std::memory_order_acquire)) {
			return true;
		}
		const auto fail = [](std::string_view a_reason) {
			spdlog::warn("Workshop supply line perk check unavailable ({}); replacement inactive", a_reason);
			return false;
		};
		std::array<std::uintptr_t, 2> calls{};
		std::array<std::array<std::uint8_t, PCF::NativeHooks::kRelativeJumpSize>, 2> originals{};
		std::size_t attempted = 0;
		const auto restore = [&]() {
			bool restored = true;
			while (attempted != 0) {
				const auto i = --attempted;
				try {
					if (std::memcmp(reinterpret_cast<const void*>(calls[i]), originals[i].data(), originals[i].size()) != 0 &&
						!PCF::NativeHooks::WriteVerified(calls[i], std::span<const std::uint8_t>{ originals[i] })) {
						restored = false;
					}
				} catch (...) {
					restored = false;
				}
			}
			if (!restored) {
				spdlog::error("Workshop perk requirement hook rollback could not be verified");
			}
		};
		try {
			const auto perkRank = PCF::EngineIDs::ActorGetPerkRank.GetAddress();
			const auto updateButtons = PCF::EngineIDs::WorkshopMenuUpdateButtonText.GetAddress();
			if (!perkRank || !updateButtons) {
				return fail("Address Library ID unavailable");
			}
			std::array<std::uintptr_t, 2> fields{};
			calls = { FindPerkRankCall(a_buttonEvent, perkRank, fields[0]), FindPerkRankCall(updateButtons, perkRank, fields[1]) };
			if (!calls[0] || !calls[1] || calls[0] == calls[1]) {
				return fail("native call site not recognized");
			}
			const auto* field = fields[0] == fields[1] ? GetWorkshopPerkField(fields[0]) : nullptr;
			if (!field) {
				return fail("perk argument is not WorkshopLocalLeaderPerk_DO");
			}
			const auto& trampolineSlot = REL::GetTrampoline();
			if (!*trampolineSlot) {
				return fail("trampoline unavailable");
			}
			auto& trampoline = **trampolineSlot;
			if (trampoline.IsEmpty()) {
				constexpr std::size_t size = 64;
				const auto api = F4SE::GetTrampolineInterface();
				auto* memory = api->AllocateFromBranchPool(size);
				if (!memory) {
					return fail("branch-pool allocation failed");
				}
				trampoline.Init(static_cast<std::byte*>(memory), size);
			}
			if (trampoline.GetFreeSize() < absoluteJumpSize * calls.size()) {
				return fail("insufficient trampoline space");
			}
			for (std::size_t i = 0; i < calls.size(); ++i) {
				if (!IsInGameImage(calls[i], originals[i].size()) || PCF::NativeHooks::ReadRelativeCallTarget(calls[i]) != perkRank) {
					return fail("native call target changed");
				}
				std::memcpy(originals[i].data(), reinterpret_cast<const void*>(calls[i]), originals[i].size());
			}
			g_workshopPerkRank = reinterpret_cast<PerkRankFunction>(perkRank);
			g_workshopPerkField = field;
			const auto replacement = reinterpret_cast<std::uintptr_t>(&WorkshopPerkRankHook);
			for (std::size_t i = 0; i < calls.size(); ++i) {
				if (std::memcmp(reinterpret_cast<const void*>(calls[i]), originals[i].data(), originals[i].size()) != 0) {
					restore();
					return fail("native call site changed");
				}
				// Mark the attempt before WriteCall in case it throws after writing.
				++attempted;
				const auto previous = trampoline.WriteCall<5>(calls[i], WorkshopPerkRankHook);
				if (previous != perkRank || !PCF::NativeHooks::IsRelativeCallTo(calls[i], replacement)) {
					restore();
					return fail("call-site write verification failed");
				}
			}
			g_workshopPerkHookInstalled.store(true, std::memory_order_release);
			return true;
		} catch (...) {
			restore();
			return fail("installation error");
		}
	}

	// Installs the hook used to control Workshop artwork.
	bool InstallWorkshopArtworkHook()
	{
		constexpr std::size_t absoluteJumpSize = 14;
		try {
			const auto owner = PCF::EngineIDs::WorkshopPublishRequirements.GetAddress();
			const auto append = PCF::EngineIDs::WorkshopAppendPerkRow.GetAddress();
			const auto setMember = PCF::EngineIDs::GFxSetMember.GetAddress();
			if (!owner || !append || !setMember) {
				return false;
			}
			const auto call = FindPerkDataCall(owner, append, setMember);
			if (!call) {
				return false;
			}
			auto& trampoline = **REL::GetTrampoline();
			if (trampoline.IsEmpty()) {
				constexpr std::size_t size = 64;
				const auto api = F4SE::GetTrampolineInterface();
				auto* memory = api->AllocateFromBranchPool(size);
				if (!memory) {
					return false;
				}
				trampoline.Init(static_cast<std::byte*>(memory), size);
			}
			if (trampoline.GetFreeSize() < absoluteJumpSize) {
				return false;
			}
			const auto expectedSetMember = setMember;
			g_workshopSetMember = reinterpret_cast<WorkshopSetMemberFunction>(expectedSetMember);
			const auto previous = trampoline.WriteCall<5>(call, WorkshopSetPerkData);
			if (previous) {
				g_workshopSetMember = reinterpret_cast<WorkshopSetMemberFunction>(previous);
			}
			const auto replacement = reinterpret_cast<std::uintptr_t>(&WorkshopSetPerkData);
			if (previous == expectedSetMember && PCF::NativeHooks::IsRelativeCallTo(call, replacement)) {
				return true;
			}
			if (previous && PCF::NativeHooks::IsRelativeCallTo(call, replacement)) {
				trampoline.WriteCall<5>(call, previous);
				if (!PCF::NativeHooks::IsRelativeCallTo(call, previous)) {
					spdlog::warn("Workshop artwork hook rollback could not be verified; PCF artwork behavior remains inactive");
				}
			}
			return false;
		} catch (...) {
			return false;
		}
	}

}


namespace PCF::WorkshopMenu
{
	// Installs the Workshop hooks and optional artwork hook.
	bool Install(std::uintptr_t a_vtable, std::uintptr_t a_inputVtable)
	{
		g_workshopArtworkInstalled = InstallWorkshopArtworkHook();
		if (!g_workshopArtworkInstalled) {
			spdlog::warn("Workshop artwork hook unavailable; retaining Workshop text presentation");
		}
		if (!PCF::RuleRegistry::Empty()) {
			if (InstallWorkshopPerkHook(PCF::NativeHooks::ReadVtableSlot(a_inputVtable, kButtonEventSlot))) {
				spdlog::info("Workshop supply line perk check active");
			}
		}
		return InstallWorkshopHooks(a_vtable, a_inputVtable);
	}
}
