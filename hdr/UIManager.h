// Perk Conditions Framework
// SilentlyGayming
// UIManager.h

#pragma once

#include "PCH.h"

#include <array>
#include <cstddef>
#include <string_view>

namespace PCF::UIHookState
{
	using Params = Scaleform::GFx::FunctionHandler::Params;
	using CallFunction = void (*)(RE::IMenu*, const Params&);
	using ProcessMessageFunction = RE::UI_MESSAGE_RESULTS (*)(RE::IMenu*, RE::UIMessage&);
	using AdvanceFunction = void (*)(RE::IMenu*, float, std::uint64_t);
	using PreDisplayFunction = void (*)(RE::IMenu*);

	inline constexpr std::size_t kExamineIndex = 0;
	inline constexpr std::size_t kCookingIndex = 1;
	inline constexpr std::size_t kPowerArmorIndex = 2;
	inline constexpr std::size_t kRobotIndex = 3;
	inline constexpr std::size_t kWorkshopIndex = 4;
	inline constexpr std::size_t kMenuCount = 5;

	struct MenuHook
	{
		const REL::IId* vtableID;
		std::string_view name;
		CallFunction call{ nullptr };
		ProcessMessageFunction processMessage{ nullptr };
		AdvanceFunction advance{ nullptr };
		PreDisplayFunction preDisplay{ nullptr };
		bool active{ false };
		bool warned{ false };
	};

	inline std::array<MenuHook, kMenuCount> g_hooks{{
		{ &RE::VTABLE::ExamineMenu[0], "ExamineMenu" },
		{ &RE::VTABLE::CookingMenu[0], "CookingMenu" },
		{ &RE::VTABLE::PowerArmorModMenu[0], "PowerArmorModMenu" },
		{ &RE::VTABLE::RobotModMenu[0], "RobotModMenu" },
		{ &RE::VTABLE::WorkshopMenu[0], "WorkshopMenu" }
	}};

	// Returns the shared UI hook table.
	[[nodiscard]] inline std::array<MenuHook, kMenuCount>& GetHooks() noexcept
	{
		return g_hooks;
	}
}

namespace PCF::UIManager
{
	[[nodiscard]] bool Install();
}
