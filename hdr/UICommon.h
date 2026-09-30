// Perk Conditions Framework
// SilentlyGayming
// UICommon.h

#pragma once

#include "PCH.h"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <system_error>

namespace PCF::UICommon
{
	using Value = Scaleform::GFx::Value;

	class DepthGuard
	{
	public:
		explicit DepthGuard(std::uint32_t& a_depth) : depth(a_depth), outermost(depth++ == 0)
		{}
		~DepthGuard()
		{
			--depth;
		}
		[[nodiscard]] bool IsOutermost() const
		{
			return outermost;
		}
		DepthGuard(const DepthGuard&) = delete;
		DepthGuard& operator=(const DepthGuard&) = delete;

	private:
		std::uint32_t& depth;
		bool outermost{ false };
	};

	// Reads a finite number from a Scaleform value.
	inline bool ReadNumber(const Value& a_value, double& a_number)
	{
		if (a_value.IsUInt()) {
			a_number = a_value.GetUInt();
		} else if (a_value.IsInt()) {
			a_number = a_value.GetInt();
		} else if (a_value.IsNumber()) {
			a_number = a_value.GetNumber();
		} else {
			return false;
		}
		return std::isfinite(a_number);
	}

	// Reads a safe unsigned index from a Scaleform member.
	inline bool ReadIndex(const Value& a_object, const char* a_name, std::uint32_t& a_index)
	{
		Value value;
		double number = 0.0;
		if (!a_object.IsAnyObject() || !a_object.GetMember(a_name, &value) || !ReadNumber(value, number) ||
			number < 0.0 || number > (std::numeric_limits<std::uint32_t>::max)() || std::trunc(number) != number) {
			return false;
		}
		a_index = static_cast<std::uint32_t>(number);
		return true;
	}

	// Reads a boolean from a Scaleform member.
	inline bool ReadBool(const Value& a_object, const char* a_name, bool& a_boolean)
	{
		Value value;
		if (!a_object.IsAnyObject() || !a_object.GetMember(a_name, &value) || !value.IsBoolean()) {
			return false;
		}
		a_boolean = value.GetBoolean();
		return true;
	}

	// Reads text from a Scaleform member.
	inline bool ReadText(const Value& a_object, const char* a_name, std::string& a_text)
	{
		Value value;
		if (!a_object.IsAnyObject() || !a_object.GetMember(a_name, &value) || !value.IsString()) {
			return false;
		}
		const char* text = value.GetString();
		if (!text) {
			return false;
		}
		a_text = text;
		return true;
	}

	// Reads an object element from a Scaleform array.
	inline bool ReadElement(const Value& a_array, std::uint32_t a_index, Value& a_value)
	{
		char key[11]{};
		const auto [end, error] = std::to_chars(key, key + 10, a_index);
		if (error != std::errc{}) {
			return false;
		}
		*end = '\0';
		return a_array.IsArray() && a_array.GetMember(key, &a_value) && a_value.IsAnyObject();
	}

	// Sets text only when the Scaleform value changes.
	inline bool SetText(Value& a_object, const char* a_name, const std::string& a_text)
	{
		Value value;
		if (!a_object.IsAnyObject()) {
			return false;
		}
		if (a_object.GetMember(a_name, &value) && value.IsString()) {
			const char* existing = value.GetString();
			if (existing && a_text == existing) {
				return false;
			}
		}
		return a_object.SetMember(a_name, Value(a_text.c_str()));
	}

	// Sets a number only when the Scaleform value changes.
	inline bool SetNumber(Value& a_object, const char* a_name, double a_number)
	{
		Value value;
		double existing = 0.0;
		if (!a_object.IsAnyObject() || (a_object.GetMember(a_name, &value) && ReadNumber(value, existing) && existing == a_number)) {
			return false;
		}
		return a_object.SetMember(a_name, Value(a_number));
	}

	// Ensures a numeric Scaleform property has the required value.
	inline bool EnsureNumber(Value& a_object, const char* a_name, double a_number)
	{
		Value value;
		double existing = 0.0;
		if (!a_object.IsAnyObject()) {
			return false;
		}
		if (a_object.GetMember(a_name, &value) && ReadNumber(value, existing) && existing == a_number) {
			return true;
		}
		return a_object.SetMember(a_name, Value(a_number));
	}

	// Sets a boolean only when the Scaleform value changes.
	inline bool SetBool(Value& a_object, const char* a_name, bool a_boolean)
	{
		Value value;
		if (!a_object.IsAnyObject() || (a_object.GetMember(a_name, &value) && value.IsBoolean() && value.GetBoolean() == a_boolean)) {
			return false;
		}
		return a_object.SetMember(a_name, Value(a_boolean));
	}
}
