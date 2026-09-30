// Perk Conditions Framework
// SilentlyGayming
// NativeHooks.h

#pragma once

#include "F4SE/Runtimes.hpp"
#include "REL/Relocation.hpp"

#ifdef _MSC_VER
#	pragma warning(push, 0)
#endif
#include <Zydis/Zydis.h>
#ifdef _MSC_VER
#	pragma warning(pop)
#endif

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace PCF::NativeHooks
{
	inline constexpr std::size_t kRelativeJumpSize = 5;
	inline constexpr std::size_t kAbsoluteJumpSize = 14;
	inline constexpr std::size_t kDecodeLimit = 32;

	// Rejects instructions that would need relocation when copied into a trampoline.
	inline bool HasUnsafeOperand(const ZydisDecodedInstruction& a_instruction, const ZydisDecodedOperand* a_operands)
	{
		for (std::uint8_t i = 0; i < a_instruction.operand_count_visible; ++i) {
			const auto& operand = a_operands[i];
			if (operand.type == ZYDIS_OPERAND_TYPE_MEMORY && operand.mem.base == ZYDIS_REGISTER_RIP) {
				return true;
			}
			if (operand.type == ZYDIS_OPERAND_TYPE_IMMEDIATE && operand.imm.is_relative) {
				return true;
			}
		}
		return false;
	}

	// Finds a whole-instruction prefix that can be copied safely into a trampoline.
	inline std::size_t FindSafeOverwriteLength(std::uintptr_t a_address, std::size_t a_minimum = kRelativeJumpSize)
	{
		if (!a_address || a_minimum < kRelativeJumpSize || a_minimum > kDecodeLimit) {
			return 0;
		}
		ZydisDecoder decoder;
		if (!ZYAN_SUCCESS(ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64))) {
			return 0;
		}
		const auto* code = reinterpret_cast<const ZyanU8*>(a_address);
		std::size_t length = 0;
		while (length < a_minimum) {
			if (length >= kDecodeLimit) {
				return 0;
			}
			ZydisDecodedInstruction instruction;
			ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
			if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, code + length, kDecodeLimit - length,
				std::addressof(instruction), operands)) || !instruction.length || HasUnsafeOperand(instruction, operands)) {
				return 0;
			}
			length += instruction.length;
		}
		return length <= kDecodeLimit ? length : 0;
	}

	// Writes an absolute x64 jump into already-owned executable trampoline memory.
	inline void WriteAbsoluteJump(void* a_destination, std::uintptr_t a_target)
	{
		std::array<std::uint8_t, kAbsoluteJumpSize> jump{};
		jump[0] = 0xFF;
		jump[1] = 0x25;
		std::memcpy(jump.data() + 6, std::addressof(a_target), sizeof(a_target));
		std::memcpy(a_destination, jump.data(), jump.size());
	}

	// Builds a rel32 jump while preserving every decoded byte in the overwrite span.
	inline bool MakeRelativeJumpPatch(std::uintptr_t a_source, std::uintptr_t a_target, std::size_t a_overwriteLength,
		std::vector<std::uint8_t>& a_patch)
	{
		if (a_overwriteLength < kRelativeJumpSize) {
			return false;
		}
		const auto nextInstruction = a_source + kRelativeJumpSize;
		const auto displacement = static_cast<std::intptr_t>(a_target) - static_cast<std::intptr_t>(nextInstruction);
		if (displacement < (std::numeric_limits<std::int32_t>::min)() ||
			displacement > (std::numeric_limits<std::int32_t>::max)()) {
			return false;
		}
		a_patch.assign(a_overwriteLength, REL::NOP);
		a_patch[0] = 0xE9;
		const auto relative = static_cast<std::int32_t>(displacement);
		std::memcpy(a_patch.data() + 1, std::addressof(relative), sizeof(relative));
		return true;
	}

	// Writes native bytes and confirms that executable memory contains the requested patch.
	inline bool WriteVerified(std::uintptr_t a_destination, std::span<const std::uint8_t> a_bytes)
	{
		if (!a_destination || a_bytes.empty()) {
			return false;
		}
		REL::WriteSafe(a_destination, a_bytes.data(), a_bytes.size());
		return std::memcmp(reinterpret_cast<const void*>(a_destination), a_bytes.data(), a_bytes.size()) == 0;
	}

	inline bool WriteVerified(std::uintptr_t a_destination, const std::vector<std::uint8_t>& a_bytes)
	{
		return WriteVerified(a_destination, std::span<const std::uint8_t>{ a_bytes.data(), a_bytes.size() });
	}

	// Reads the effective destination of one rel32 CALL instruction.
	inline std::uintptr_t ReadRelativeCallTarget(std::uintptr_t a_callsite)
	{
		if (!a_callsite || *reinterpret_cast<const std::uint8_t*>(a_callsite) != 0xE8) {
			return 0;
		}
		std::int32_t relative = 0;
		std::memcpy(std::addressof(relative), reinterpret_cast<const void*>(a_callsite + 1), sizeof(relative));
		return static_cast<std::uintptr_t>(
			static_cast<std::intptr_t>(a_callsite + kRelativeJumpSize) + static_cast<std::intptr_t>(relative));
	}

	// Verifies a rel32 CALL, including the absolute-jump relay emitted by F4SE's 5-byte trampoline helper.
	inline bool IsRelativeCallTo(std::uintptr_t a_callsite, std::uintptr_t a_expected)
	{
		const auto target = ReadRelativeCallTarget(a_callsite);
		if (!target || !a_expected) {
			return false;
		}
		if (target == a_expected) {
			return true;
		}
		const auto* relay = reinterpret_cast<const std::uint8_t*>(target);
		if (relay[0] != 0xFF || relay[1] != 0x25) {
			return false;
		}
		std::int32_t displacement = 0;
		std::memcpy(std::addressof(displacement), relay + 2, sizeof(displacement));
		if (displacement != 0) {
			return false;
		}
		std::uintptr_t relayTarget = 0;
		std::memcpy(std::addressof(relayTarget), relay + 6, sizeof(relayTarget));
		return relayTarget == a_expected;
	}

	// Reads and verifies one native vtable slot.
	inline std::uintptr_t ReadVtableSlot(std::uintptr_t a_vtable, std::size_t a_slot)
	{
		if (!a_vtable) {
			return 0;
		}
		return reinterpret_cast<const std::uintptr_t*>(a_vtable)[a_slot];
	}

	inline bool IsVtableSlotSet(std::uintptr_t a_vtable, std::size_t a_slot, std::uintptr_t a_expected)
	{
		return a_expected != 0 && ReadVtableSlot(a_vtable, a_slot) == a_expected;
	}
}

namespace PCF::EngineIDs
{
	inline constexpr F4SE::VariantId BGSMessageGetConvertedDescription{ 8331, 2203353, 2203353 };
	inline constexpr F4SE::VariantId TESDescriptionGetDescription{ 523613, 2193019, 2193019 };
	inline constexpr F4SE::VariantId PipboyPerksMenuUpdateData{ 783380, 2224224, 2224224 };
	inline constexpr F4SE::VariantId TESTopicInfoLoad{ 918452, 2208420, 2208420 };
	inline constexpr F4SE::VariantId TESTopicInfoInitItemImpl{ 1153916, 2208421, 2208421 };
	inline constexpr F4SE::VariantId LocalizedSubrecordLoad{ 952518, 2194235, 2194235 };
	inline constexpr F4SE::VariantId TESResponseTextLoad{ 142679, 2208286, 2208286 };
	inline constexpr F4SE::VariantId DialoguePromptFallback{ 163538, 2208435, 2208435 };
	inline constexpr F4SE::VariantId DialoguePromptGetter{ 1435199, 2208446, 2208446 };
	inline constexpr F4SE::VariantId DialoguePromptSetter{ 105445, 2208447, 2208447 };
	inline constexpr F4SE::VariantId DialoguePromptInsert{ 807487, 2208487, 2208487 };
	inline constexpr F4SE::VariantId WorkshopPublishRequirements{ 931840, 2225003, 2225003 };
	inline constexpr F4SE::VariantId WorkshopAppendPerkRow{ 1150661, 2225058, 2225058 };
	inline constexpr F4SE::VariantId TESConditionIsTrue{ 1275731, 2211989, 2211989 };
	inline constexpr F4SE::VariantId TESConditionIsTrueContext{ 743921, 2211990, 2211990 };
	inline constexpr F4SE::VariantId TESConditionIsTrueForAllButFunction{ 1182457, 2211991, 2211991 };
	inline constexpr F4SE::VariantId WorkbenchChoiceRequirements{ 1484640, 2223051, 2223051 };
	inline constexpr F4SE::VariantId GFxSetMember{ 1360149, 2286589, 2286589 };
}

namespace PCF::PRKFCompatibility
{
	struct Build
	{
		std::string_view name;
		std::array<std::uint16_t, 4> runtime;
		std::uint32_t timestamp;
		std::uint32_t imageSize;
		std::uintptr_t requirementsRVA;
		std::uintptr_t stringAssignRVA;
		std::uintptr_t reqsFieldOffset;
		std::array<std::uint8_t, 14> requirementsPrologue;
		std::array<std::uint8_t, 4> reqsField;
		std::array<std::uint8_t, 5> assignPrologue;
		std::uintptr_t descriptionRVA;
		std::uintptr_t descriptionFormArgRVA;
		std::array<std::uint8_t, 14> descriptionResolver;
		std::array<std::uint8_t, 15> descriptionHelperPrologue;
	};

	inline constexpr std::array<Build, 3> kBuilds{{
		{
			"OG 1.10.163.0", { 1, 10, 163, 0 }, 0x5DE9515B, 0x82000,
			0x14160, 0x7D40, 0x4D,
			{ 0x48, 0x8B, 0xC4, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57 },
			{ 0x4C, 0x8D, 0x71, 0x10 },
			{ 0x48, 0x89, 0x5C, 0x24, 0x08 },
			0x8340, 0x0,
			{},
			{ 0x40, 0x57, 0x48, 0x83, 0xEC, 0x50, 0x48, 0xC7, 0x44, 0x24, 0x30, 0xFE, 0xFF, 0xFF, 0xFF }
		},
		{
			"NG 1.10.984.0", { 1, 10, 984, 0 }, 0x66475DEA, 0x7C000,
			0x11C40, 0x73D0, 0x4C,
			{ 0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56 },
			{ 0x4C, 0x8D, 0x61, 0x10 },
			{ 0x48, 0x89, 0x5C, 0x24, 0x10 },
			0x12B86, 0x12B9B,
			{ 0x4C, 0x8B, 0x0D, 0xD3, 0xEA, 0x05, 0x00, 0x49, 0x81, 0xC1, 0x30, 0xA2, 0x2B, 0x00 },
			{}
		},
		{
			"AE 1.11.240.0", { 1, 11, 240, 0 }, 0x6A9D7778, 0x7C000,
			0x13DF0, 0x79A0, 0x4E,
			{ 0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56 },
			{ 0x4C, 0x8D, 0x71, 0x10 },
			{ 0x48, 0x89, 0x5C, 0x24, 0x10 },
			0x14D34, 0x14D49,
			{ 0x4C, 0x8B, 0x0D, 0x35, 0xC8, 0x05, 0x00, 0x49, 0x81, 0xC1, 0x10, 0xEA, 0x30, 0x00 },
			{}
		},
	}};

	// Selects only a PRKF image whose patch sites were verified from its released DLL.
	[[nodiscard]] inline const Build* FindBuild(std::uint32_t a_timestamp, std::uint32_t a_imageSize) noexcept
	{
		for (const auto& build : kBuilds) {
			if (build.timestamp == a_timestamp && build.imageSize == a_imageSize) {
				return &build;
			}
		}
		return nullptr;
	}
}
