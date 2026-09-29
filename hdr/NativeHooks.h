// Perk Conditions Framework
// SilentlyGayming
// NativeHooks.h

#pragma once

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
