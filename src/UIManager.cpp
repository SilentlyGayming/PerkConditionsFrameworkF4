// Perk Conditions Framework
// SilentlyGayming
// UIManager.cpp

#include "PCH.h"
#include "UIManager.h"
#include "CraftingMenus.h"
#include "WorkshopMenu.h"

#include <array>
#include <cstdint>
#include <string>

namespace
{
	constexpr std::size_t kCallSlot = 0x01;
	constexpr std::size_t kProcessMessageSlot = 0x03;
	constexpr std::size_t kAdvanceSlot = 0x04;
	constexpr std::size_t kPreDisplaySlot = 0x05;
	constexpr std::size_t kButtonEventSlot = 0x08;
	constexpr std::size_t kWorkshopIndex = PCF::UIHookState::kWorkshopIndex;
	constexpr std::size_t kCraftingMenuCount = 4;
	bool g_installed{ false };
}

namespace PCF::UIManager
{
	// Checks every required crafting and Workshop game function before installing UI hooks.
	bool Install()
	{
		if (g_installed) {
			return true;
		}
		const auto fail = [](std::string a_reason) {
			spdlog::error("Menu hook system unavailable: {}", a_reason);
			return false;
		};

		std::array<std::uintptr_t, PCF::UIHookState::kMenuCount> vtables{};
		auto& hooks = PCF::UIHookState::GetHooks();
		for (std::size_t i = 0; i < hooks.size(); ++i) {
			auto& hook = hooks[i];
			vtables[i] = REL::Relocation<std::uintptr_t>{ *hook.vtableID }.GetAddress();
			const auto* table = reinterpret_cast<const std::uintptr_t*>(vtables[i]);
			if (i == kWorkshopIndex) {
				if (!table[kCallSlot] || !table[kProcessMessageSlot] || !table[kAdvanceSlot] || !table[kPreDisplaySlot]) {
					return fail("WorkshopMenu Call/ProcessMessage/AdvanceMovie/PreDisplay callbacks");
				}
			} else {
				if (!table[kCallSlot]) {
					return fail(fmt::format("{} native UI Call callback", hook.name));
				}
				if (i == PCF::UIHookState::kCookingIndex && !table[kPreDisplaySlot]) {
					return fail("CookingMenu PreDisplay callback");
				}
			}
		}

		const auto inputVtable =
			REL::Relocation<std::uintptr_t>{ RE::VTABLE::WorkshopMenu[1] }.GetAddress();
		const auto* inputTable = reinterpret_cast<const std::uintptr_t*>(inputVtable);
		if (!inputTable[kButtonEventSlot]) {
			return fail("WorkshopMenu ButtonEvent callback");
		}

		const std::array<std::uintptr_t, kCraftingMenuCount> craftingVtables{
			vtables[PCF::UIHookState::kExamineIndex],
			vtables[PCF::UIHookState::kCookingIndex],
			vtables[PCF::UIHookState::kPowerArmorIndex],
			vtables[PCF::UIHookState::kRobotIndex]
		};
		for (auto& hook : hooks) {
			hook.active = false;
		}
		if (!PCF::CraftingMenus::Install(craftingVtables)) {
			return fail("crafting menu hook write verification failed");
		}
		if (!PCF::WorkshopMenu::Install(vtables[kWorkshopIndex], inputVtable)) {
			return fail("Workshop menu hook write verification failed");
		}
		for (auto& hook : hooks) {
			hook.active = true;
		}
		g_installed = true;
		return true;
	}
}
