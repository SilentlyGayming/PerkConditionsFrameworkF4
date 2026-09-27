// Perk Conditions Framework
// SilentlyGayming
// Config.cpp

#include "PCH.h"
#include "Config.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <fstream>
#include <iterator>
#include <limits>
#include <sstream>
#include <string_view>
#include <system_error>
#include <toml.hpp>

namespace
{
	// Removes extra spaces around configuration text.
	std::string_view Trim(std::string_view a_text)
	{
		const auto first = a_text.find_first_not_of(" \t\r\n");
		return first == std::string_view::npos ? std::string_view{} :
			a_text.substr(first, a_text.find_last_not_of(" \t\r\n") - first + 1);
	}

	// Splits a configuration line into separate fields.
	std::vector<std::string_view> SplitFields(std::string_view a_text)
	{
		std::vector<std::string_view> fields;
		fields.reserve(8);
		while (true) {
			const auto separator = a_text.find('|');
			fields.push_back(Trim(a_text.substr(0, separator)));
			if (separator == std::string_view::npos) {
				return fields;
			}
			a_text.remove_prefix(separator + 1);
		}
	}

	// Reads quoted text from a configuration line.
	bool ReadQuotedText(std::string_view a_text, std::string& a_value)
	{
		a_text = Trim(a_text);
		if (a_text.size() < 2 || a_text.front() != '"' || a_text.back() != '"') {
			return false;
		}
		a_text.remove_prefix(1);
		a_text.remove_suffix(1);
		std::string decoded;
		decoded.reserve(a_text.size());
		for (std::size_t i = 0; i < a_text.size(); ++i) {
			const auto character = a_text[i];
			if (character == '\\' && i + 1 < a_text.size() && (a_text[i + 1] == '"' || a_text[i + 1] == '\\')) {
				decoded.push_back(a_text[++i]);
			} else {
				decoded.push_back(character);
			}
		}
		const auto trimmed = Trim(decoded);
		if (trimmed.empty()) {
			return false;
		}
		a_value.assign(trimmed);
		return true;
	}

	// Reads quoted text without discarding intentional leading, trailing, or empty content.
	bool ReadQuotedTextExact(std::string_view a_text, std::string& a_value)
	{
		a_text = Trim(a_text);
		if (a_text.size() < 2 || a_text.front() != '"' || a_text.back() != '"') {
			return false;
		}
		a_text.remove_prefix(1);
		a_text.remove_suffix(1);
		std::string decoded;
		decoded.reserve(a_text.size());
		for (std::size_t i = 0; i < a_text.size(); ++i) {
			const auto character = a_text[i];
			if (character == '\\' && i + 1 < a_text.size() && (a_text[i + 1] == '"' || a_text[i + 1] == '\\')) {
				decoded.push_back(a_text[++i]);
			} else {
				decoded.push_back(character);
			}
		}
		a_value = std::move(decoded);
		return true;
	}

	// Returns the unmodified text after a fixed number of field separators.
	std::string_view GetFieldTail(std::string_view a_text, std::size_t a_separatorCount)
	{
		for (std::size_t i = 0; i < a_separatorCount; ++i) {
			const auto separator = a_text.find('|');
			if (separator == std::string_view::npos) {
				return {};
			}
			a_text.remove_prefix(separator + 1);
		}
		return a_text;
	}

	// Reads a positive whole number.
	bool ReadPositiveNumber(std::string_view a_text, std::uint32_t& a_value)
	{
		if (a_text.empty()) {
			return false;
		}
		const auto [end, error] = std::from_chars(a_text.data(), a_text.data() + a_text.size(), a_value);
		return error == std::errc{} && end == a_text.data() + a_text.size() && a_value != 0;
	}

	// Reads a number used for value checks.
	bool ReadCheckValue(std::string_view a_text, float& a_value)
	{
		if (a_text.empty()) {
			return false;
		}
		const auto [end, error] = std::from_chars(a_text.data(), a_text.data() + a_text.size(), a_value);
		return error == std::errc{} && end == a_text.data() + a_text.size() && std::isfinite(a_value);
	}

	// Converts configuration keywords to uppercase.
	std::string ToUpper(std::string_view a_text)
	{
		std::string result(a_text);
		std::ranges::transform(result, result.begin(), [](unsigned char a_character) {
			return static_cast<char>(std::toupper(a_character));
		});
		return result;
	}

	// Reads a comparison operator.
	bool ReadComparison(std::string_view a_text, PCF::ComparisonOp& a_value)
	{
		if (a_text == "=") {
			a_value = PCF::ComparisonOp::kEqual;
		} else if (a_text == ">") {
			a_value = PCF::ComparisonOp::kGreater;
		} else if (a_text == "<") {
			a_value = PCF::ComparisonOp::kLess;
		} else if (a_text == ">=") {
			a_value = PCF::ComparisonOp::kGreaterEqual;
		} else if (a_text == "<=") {
			a_value = PCF::ComparisonOp::kLessEqual;
		} else {
			return false;
		}
		return true;
	}

	// Reads the configured rule mode.
	bool ReadMode(std::string_view a_text, PCF::RuleMode& a_value)
	{
		const auto keyword = ToUpper(a_text);
		if (keyword == "REPLACE") {
			a_value = PCF::RuleMode::kReplace;
		} else if (keyword == "OR") {
			a_value = PCF::RuleMode::kOr;
		} else {
			return false;
		}
		return true;
	}

	// Reads the display mode and reports outdated values.
	bool ReadDisplay(std::string_view a_text, PCF::DisplayMode& a_value, std::string& a_error)
	{
		const auto keyword = ToUpper(a_text);
		if (keyword == "ORIGINAL") {
			a_value = PCF::DisplayMode::kOriginal;
			return true;
		}
		if (keyword == "CUSTOM") {
			a_value = PCF::DisplayMode::kCustom;
			return true;
		}
		if (keyword == "VANILLA") {
			a_error = "Display value 'VANILLA' is no longer valid; use ORIGINAL";
		} else if (keyword == "1" || keyword == "2") {
			a_error = "Numeric display values are no longer supported; use ORIGINAL or CUSTOM";
		} else {
			a_error = "Invalid display; expected ORIGINAL or CUSTOM";
		}
		return false;
	}

	// Checks the rule settings and chooses the display mode.
	bool ReadRuleSettings(std::string_view a_modeText, std::string_view a_displayText,
		PCF::RuleMode& a_mode, PCF::DisplayMode& a_display, std::string& a_error)
	{
		if (!ReadMode(a_modeText, a_mode)) {
			const auto keyword = ToUpper(a_modeText);
			if (keyword == "1" || keyword == "2") {
				a_error = "Numeric display values are no longer supported; add REPLACE or OR mode and use ORIGINAL or CUSTOM display";
			} else if (keyword == "VANILLA") {
				a_error = "Display value 'VANILLA' is no longer valid; add REPLACE or OR mode and use ORIGINAL";
			} else {
				a_error = "Invalid mode; expected REPLACE or OR";
			}
			return false;
		}
		const bool displaySpecified = !a_displayText.empty();
		a_display = PCF::DisplayMode::kCustom;
		if (displaySpecified && !ReadDisplay(a_displayText, a_display, a_error)) {
			return false;
		}
		PCF::DisplayMode effective = PCF::DisplayMode::kCustom;
		if (!PCF::ChooseDisplayMode(a_mode, displaySpecified, a_display, effective)) {
			a_error = a_mode == PCF::RuleMode::kOr ?
				"OR rules require display ORIGINAL or CUSTOM" :
				"REPLACE rules use CUSTOM presentation; remove ORIGINAL";
			return false;
		}
		a_display = effective;
		return true;
	}

	// Wraps a configuration line so TOML can read it.
	std::string QuoteForToml(std::string_view a_text)
	{
		std::string result;
		result.reserve(a_text.size() + 2);
		result.push_back('"');
		for (const auto ch : a_text) {
			if (ch == '"' || ch == '\\') {
				result.push_back('\\');
			}
			result.push_back(ch);
		}
		result.push_back('"');
		return result;
	}

	// Converts PCF configuration text into valid TOML.
	std::string BuildToml(std::string_view a_text)
	{
		if (a_text.starts_with("\xEF\xBB\xBF")) {
			a_text.remove_prefix(3);
		}
		std::istringstream input{ std::string(a_text) };
		std::string output;
		output.reserve(a_text.size());
		std::string line;
		while (std::getline(input, line)) {
			const auto text = Trim(line);
			if (text.starts_with(';')) {
				output.push_back('#');
				output.append(text.substr(1));
			} else if (!text.empty() && text.front() != '[' && text.front() != '#' &&
				text.front() != '"' && text.front() != '\'' && text.find('|') != std::string_view::npos) {
				output.append(QuoteForToml(text));
				output.append(" = true");
			} else {
				output.append(line);
			}
			output.push_back('\n');
		}
		return output;
	}

	// Cleans up an interface artwork path.
	std::string CleanPath(std::string_view a_path)
	{
		std::string path(Trim(a_path));
		std::replace(path.begin(), path.end(), '\\', '/');
		auto startsWithInsensitive = [&path](std::string_view a_prefix) {
			if (path.size() < a_prefix.size()) {
				return false;
			}
			for (std::size_t i = 0; i < a_prefix.size(); ++i) {
				if (std::tolower(static_cast<unsigned char>(path[i])) !=
					std::tolower(static_cast<unsigned char>(a_prefix[i]))) {
					return false;
				}
			}
			return true;
		};
		if (startsWithInsensitive("Data/Interface/")) {
			path.erase(0, 15);
		} else if (startsWithInsensitive("Interface/")) {
			path.erase(0, 10);
		}
		return path;
	}

	struct ConfigIssue
	{
		std::size_t line{ 0 };
		std::string message;
	};

	struct ConfigResult
	{
		PCF::LoadedConfig config;
		std::vector<ConfigIssue> issues;
		bool valid{ true };
	};

	// Reads PCF configuration data.
	ConfigResult ReadConfig(std::string_view a_text, const std::filesystem::path& a_file)
	{
		ConfigResult result;
		toml::table document;
		try {
			document = toml::parse(BuildToml(a_text));
		} catch (const toml::parse_error& error) {
			result.valid = false;
			result.issues.push_back({ error.source().begin.line, std::string(error.description()) });
			return result;
		}

		for (const auto& [sectionName, section] : document) {
			const auto name = sectionName.str();
			const bool perkSection = name == "Perks";
			const bool actorValueSection = name == "ActorValues";
			const bool globalValueSection = name == "GlobalValues";
			const bool thresholdSection = actorValueSection || globalValueSection;
			const bool conditionsSection = name == "Conditions";
			const bool namesSection = name == "Names";
			const bool descriptionsSection = name == "Descriptions";
			const bool requirementLabelsSection = name == "RequirementLabels";
			const bool swfSection = name == "SWF";
			const auto* table = section.as_table();
			if (!table || (!perkSection && !thresholdSection && !conditionsSection && !namesSection && !descriptionsSection && !requirementLabelsSection && !swfSection)) {
				result.issues.push_back({ section.source().begin.line, "Unknown section: " + std::string(name) });
				continue;
			}

			for (const auto& [key, value] : *table) {
				const auto line = static_cast<std::size_t>(key.source().begin.line);
				const auto enabled = value.value_exact<bool>();
				if (enabled && !*enabled) {
					continue;
				}

				auto fields = SplitFields(key.str());
				const auto pathValue = swfSection ? value.value<std::string>() : std::nullopt;
				if (!enabled && !pathValue) {
					const auto expectation = namesSection ? "Expected true for a [Names] assignment" :
						descriptionsSection ? "Expected true for a [Descriptions] rule" :
						requirementLabelsSection ? "Expected true for a [RequirementLabels] assignment" :
						conditionsSection ? "Expected true/false for a [Conditions] rule" :
						"Expected true/false, or a path string in [SWF]";
					result.issues.push_back({ line, expectation });
					continue;
				}

				if (conditionsSection) {
					PCF::RawCustomCondition rule;
					rule.file = a_file;
					rule.line = line;

					// Existing six-field form-value rows remain unchanged. A final OR token is optional
					// and means this row is combined with the following row using native CTDA OR semantics.
					const bool typedQuest = fields.size() >= 3 &&
						(ToUpper(fields[2]) == "QUESTSTAGE" || ToUpper(fields[2]) == "QUESTSTAGEDONE");
					const auto readJoin = [&](std::size_t a_index) {
						if (fields.size() <= a_index) {
							return true;
						}
						if (ToUpper(fields[a_index]) != "OR") {
							result.issues.push_back({ line, "Invalid [Conditions] join; expected OR" });
							return false;
						}
						rule.orWithNext = true;
						return true;
					};

					if (!typedQuest && (fields.size() == 6 || fields.size() == 7)) {
						rule.type = PCF::CustomConditionType::kFormValue;
						if (std::any_of(fields.begin(), fields.begin() + 4, [](std::string_view a_field) { return a_field.empty(); }) ||
							!ReadComparison(fields[4], rule.comparison) || !ReadCheckValue(fields[5], rule.requiredValue) || !readJoin(6)) {
							result.issues.push_back({ line,
								"Expected targetPlugin|targetFormID|valuePlugin|valueFormID|comparison|requiredValue[|OR]" });
							continue;
						}
						rule.targetPlugin = fields[0];
						rule.targetReference = fields[1];
						rule.conditionPlugin = fields[2];
						rule.conditionReference = fields[3];
						result.config.customConditions.push_back(std::move(rule));
						continue;
					}

					if (typedQuest && (fields.size() == 7 || fields.size() == 8)) {
						const auto kind = ToUpper(fields[2]);
						rule.type = kind == "QUESTSTAGE" ? PCF::CustomConditionType::kQuestStage : PCF::CustomConditionType::kQuestStageDone;
						if (fields[0].empty() || fields[1].empty() || fields[3].empty() || fields[4].empty() ||
							!ReadComparison(fields[5], rule.comparison) || !ReadCheckValue(fields[6], rule.requiredValue) || !readJoin(7)) {
							result.issues.push_back({ line,
								"Expected targetPlugin|targetFormID|QuestStage/QuestStageDone|questPlugin|questFormID|comparison|stage[|OR]" });
							continue;
						}
						if (rule.type == PCF::CustomConditionType::kQuestStageDone && rule.comparison != PCF::ComparisonOp::kEqual) {
							result.issues.push_back({ line, "QuestStageDone requires '=' because it tests whether the exact stage has completed" });
							continue;
						}
						rule.targetPlugin = fields[0];
						rule.targetReference = fields[1];
						rule.conditionPlugin = fields[3];
						rule.conditionReference = fields[4];
						result.config.customConditions.push_back(std::move(rule));
						continue;
					}

					result.issues.push_back({ line,
						"Expected a 6-field form-value or 7-field typed quest condition, optionally followed by |OR" });
					continue;
				}

				if (namesSection) {
					std::string displayName;
					if (fields.size() != 3 || fields[0].empty() || fields[1].empty() || !ReadQuotedText(fields[2], displayName)) {
						result.issues.push_back({ line, "Expected plugin|reference|\"non-empty display name\"" });
						continue;
					}
					result.config.nameAssignments.push_back({ std::string(fields[0]), std::string(fields[1]), std::move(displayName), a_file, line });
					continue;
				}

				if (requirementLabelsSection) {
					std::string label;
					if (fields.size() < 5 || std::any_of(fields.begin(), fields.begin() + 4, [](std::string_view a_field) { return a_field.empty(); }) ||
						!ReadQuotedText(GetFieldTail(key.str(), 4), label)) {
						result.issues.push_back({ line,
							"Expected targetPlugin|targetFormID|conditionPlugin|conditionFormID|\"non-empty label\"" });
						continue;
					}
					result.config.requirementLabels.push_back({ std::string(fields[0]), std::string(fields[1]),
						std::string(fields[2]), std::string(fields[3]), std::move(label), a_file, line });
					continue;
				}

				if (descriptionsSection) {
					std::string description;

					// Three-field rows replace the description of an Actor Value directly.
					if (fields.size() >= 3 && !fields[0].empty() && !fields[1].empty() && Trim(fields[2]).starts_with('\"')) {
						if (!ReadQuotedTextExact(GetFieldTail(key.str(), 2), description)) {
							result.issues.push_back({ line, "Expected actorValuePlugin|actorValueFormID|\"description\"" });
							continue;
						}
						if (description.size() >= (std::numeric_limits<std::uint16_t>::max)()) {
							result.issues.push_back({ line, "Description exceeds the native 16-bit string limit" });
							continue;
						}
						result.config.actorValueDescriptions.push_back({ std::string(fields[0]), std::string(fields[1]),
							std::move(description), a_file, line });
						continue;
					}

					PCF::ComparisonOp comparison;
					float requiredValue = 0.0F;
					if (fields.size() < 7 || std::any_of(fields.begin(), fields.begin() + 4, [](std::string_view a_field) { return a_field.empty(); }) ||
						!ReadComparison(fields[4], comparison) || !ReadCheckValue(fields[5], requiredValue) ||
						!ReadQuotedText(GetFieldTail(key.str(), 6), description)) {
						result.issues.push_back({ line, "Expected sourcePlugin|sourceForm|conditionPlugin|conditionForm|comparison|requiredValue|\"non-empty description\" or actorValuePlugin|actorValueFormID|\"description\"" });
						continue;
					}
					if (description.size() >= (std::numeric_limits<std::uint16_t>::max)()) {
						result.issues.push_back({ line, "Description exceeds the native 16-bit string limit" });
						continue;
					}
					result.config.descriptionRules.push_back({ comparison, std::string(fields[0]), std::string(fields[1]),
						std::string(fields[2]), std::string(fields[3]), requiredValue, std::move(description), a_file, line });
					continue;
				}

				if (swfSection) {
					if (fields.size() != (pathValue ? 2u : 3u) || fields[0].empty() || fields[1].empty()) {
						result.issues.push_back({ line, "Expected plugin|reference|path = true or plugin|reference = path" });
						continue;
					}
					const auto path = CleanPath(pathValue ? std::string_view(*pathValue) : fields[2]);
					if (path.empty()) {
						result.issues.push_back({ line, "Empty SWF path" });
						continue;
					}
					result.config.swfAssignments.push_back({ std::string(fields[0]), std::string(fields[1]), path, a_file, line });
					continue;
				}

				if (thresholdSection && fields.size() >= 6 && !fields[4].empty()) {
					PCF::ComparisonOp comparison;
					const auto legacyDisplay = ToUpper(fields[5]);
					if (!ReadComparison(fields[4], comparison) &&
						(legacyDisplay == "1" || legacyDisplay == "2" || legacyDisplay == "VANILLA" ||
							legacyDisplay == "ORIGINAL" || legacyDisplay == "CUSTOM")) {
						result.issues.push_back({ line, "Legacy ActorValue/GlobalValue row; add an explicit comparison and REPLACE/OR mode. Numeric/VANILLA display values are retired" });
						continue;
					}
				}

				const auto minimum = thresholdSection ? 7u : 6u;
				if (fields.size() < minimum || fields.size() > minimum + 1 ||
					std::any_of(fields.begin(), fields.begin() + 4, [](std::string_view a_field) { return a_field.empty(); })) {
					result.issues.push_back({ line, thresholdSection ?
						"Expected sourcePlugin|sourceForm|valuePlugin|valueForm|comparison|requiredValue|mode|display?" :
						"Expected sourcePlugin|sourceForm|replacementPlugin|replacementForm|replacementRank|mode|display?" });
					continue;
				}

				PCF::RawPerkRule rule;
				rule.file = a_file;
				rule.line = line;
				rule.type = actorValueSection ? PCF::AlternativeType::kActorValue :
					globalValueSection ? PCF::AlternativeType::kGlobalValue : PCF::AlternativeType::kPerk;
				rule.sourcePlugin = fields[0];
				rule.sourceReference = fields[1];
				rule.alternativePlugin = fields[2];
				rule.alternativeReference = fields[3];

				std::string error;
				const auto modeField = thresholdSection ? 6u : 5u;
				const auto displayText = fields.size() > modeField + 1 ? fields[modeField + 1] : std::string_view{};
				if (!ReadRuleSettings(fields[modeField], displayText, rule.mode, rule.display, error)) {
					result.issues.push_back({ line, std::move(error) });
					continue;
				}

				if (thresholdSection) {
					if (!ReadComparison(fields[4], rule.comparison)) {
						result.issues.push_back({ line, "Invalid comparison operator; expected =, >, <, >=, or <=" });
						continue;
					}
					if (!ReadCheckValue(fields[5], rule.requiredValue)) {
						result.issues.push_back({ line, "Invalid finite requiredValue" });
						continue;
					}
				} else if (!fields[4].empty() && (!ReadPositiveNumber(fields[4], rule.rank) || rule.rank > 255)) {
					result.issues.push_back({ line, "Invalid replacement rank; expected 1..255 or blank for rank 1" });
					continue;
				}
				result.config.rules.push_back(std::move(rule));
			}
		}

		std::ranges::sort(result.config.rules, {}, &PCF::RawPerkRule::line);
		std::ranges::sort(result.config.customConditions, {}, &PCF::RawCustomCondition::line);
		std::ranges::sort(result.config.descriptionRules, {}, &PCF::RawDescriptionRule::line);
		std::ranges::sort(result.config.actorValueDescriptions, {}, &PCF::RawActorValueDescriptionAssignment::line);
		std::ranges::sort(result.config.nameAssignments, {}, &PCF::RawNameAssignment::line);
		std::ranges::sort(result.config.swfAssignments, {}, &PCF::RawSWFAssignment::line);
		return result;
	}

	// Loads a configuration file and adds its settings.
	bool LoadConfigFile(const std::filesystem::path& a_path, PCF::LoadedConfig& a_config)
	{
		std::ifstream file(a_path, std::ios::binary);
		std::string text;
		std::string readFailure;
		if (!file) {
			readFailure = "cannot open file";
		} else {
			text.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
			if (file.bad()) {
				readFailure = "read failure";
			}
		}
		if (!readFailure.empty()) {
			spdlog::error("Configuration {}: {}", a_path.string(), readFailure);
			return false;
		}

		auto configResult = ReadConfig(text, a_path);
		std::string issues;
		for (const auto& issue : configResult.issues) {
			fmt::format_to(std::back_inserter(issues), "\n- line {}: {}", issue.line, issue.message);
		}
		if (!configResult.valid) {
			spdlog::error("Rejected malformed TOML file {}:{}", a_path.filename().string(), issues);
			return false;
		}
		if (!issues.empty()) {
			spdlog::warn("{}: {} configuration issue(s):{}", a_path.filename().string(), configResult.issues.size(), issues);
		}
		spdlog::info("{}: {} rule(s), {} custom condition(s), {} description rule(s), {} Actor Value description assignment(s), {} requirement label(s), {} name assignment(s), {} SWF assignment(s)", a_path.filename().string(),
			configResult.config.rules.size(), configResult.config.customConditions.size(), configResult.config.descriptionRules.size(),
			configResult.config.actorValueDescriptions.size(), configResult.config.requirementLabels.size(), configResult.config.nameAssignments.size(), configResult.config.swfAssignments.size());

		a_config.rules.insert(a_config.rules.end(), std::make_move_iterator(configResult.config.rules.begin()),
			std::make_move_iterator(configResult.config.rules.end()));
		a_config.customConditions.insert(a_config.customConditions.end(), std::make_move_iterator(configResult.config.customConditions.begin()),
			std::make_move_iterator(configResult.config.customConditions.end()));
		a_config.descriptionRules.insert(a_config.descriptionRules.end(), std::make_move_iterator(configResult.config.descriptionRules.begin()),
			std::make_move_iterator(configResult.config.descriptionRules.end()));
		a_config.actorValueDescriptions.insert(a_config.actorValueDescriptions.end(), std::make_move_iterator(configResult.config.actorValueDescriptions.begin()),
			std::make_move_iterator(configResult.config.actorValueDescriptions.end()));
		a_config.requirementLabels.insert(a_config.requirementLabels.end(), std::make_move_iterator(configResult.config.requirementLabels.begin()),
			std::make_move_iterator(configResult.config.requirementLabels.end()));
		a_config.nameAssignments.insert(a_config.nameAssignments.end(), std::make_move_iterator(configResult.config.nameAssignments.begin()),
			std::make_move_iterator(configResult.config.nameAssignments.end()));
		a_config.swfAssignments.insert(a_config.swfAssignments.end(), std::make_move_iterator(configResult.config.swfAssignments.begin()),
			std::make_move_iterator(configResult.config.swfAssignments.end()));
		return true;
	}
}

namespace PCF::Config
{
	// Loads all PCF configuration files.
	LoadedConfig Load()
	{
		const std::filesystem::path directory = "Data/F4SE/Plugins/PerkConditionsFramework";
		LoadedConfig config;
		std::vector<std::filesystem::path> files;
		std::error_code error;
		std::filesystem::directory_iterator iterator(directory, error);
		const std::filesystem::directory_iterator end;

		while (!error && iterator != end) {
			if (iterator->is_regular_file(error) && !error) {
				auto extension = iterator->path().extension().string();
				std::ranges::transform(extension, extension.begin(), [](unsigned char a_ch) {
					return static_cast<char>(std::tolower(a_ch));
				});
				if (extension == ".toml") {
					files.push_back(iterator->path());
				}
			}
			if (!error) {
				iterator.increment(error);
			}
		}

		if (error) {
			spdlog::warn("Configuration directory {}: {}", directory.string(), error.message());
		}
		std::ranges::sort(files);
		spdlog::info("{} matching TOML file(s) found", files.size());
		config.matchingFileCount = files.size();
		for (const auto& path : files) {
			config.loadedFileCount += LoadConfigFile(path, config) ? 1 : 0;
		}
		return config;
	}
}
