/**
 * Schema Validator Implementation
 */

#include "schema_validator.h"
#include "core/logger.h"
#include <simdjson.h>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <charconv>
#include <unordered_set>

namespace fasterapi {
namespace http {

// ============================================================================
// Helper Functions
// ============================================================================

const char* schema_type_to_string(SchemaType type) noexcept {
    switch (type) {
        case SchemaType::STRING: return "string";
        case SchemaType::INTEGER: return "integer";
        case SchemaType::FLOAT: return "number";
        case SchemaType::BOOLEAN: return "boolean";
        case SchemaType::ARRAY: return "array";
        case SchemaType::OBJECT: return "object";
        case SchemaType::NULL_TYPE: return "null";
        case SchemaType::ANY: return "any";
    }
    return "unknown";
}

// ============================================================================
// Schema Implementation
// ============================================================================

void Schema::add_field(Field field) {
    field_index_[field.name] = fields_.size();
    fields_.push_back(std::move(field));
}

void Schema::add_field(std::string name, SchemaType type, bool required) {
    add_field(Field(std::move(name), type, required));
}

bool Schema::has_field(const std::string& field_name) const noexcept {
    return field_index_.find(field_name) != field_index_.end();
}

const Field* Schema::get_field(const std::string& field_name) const noexcept {
    auto it = field_index_.find(field_name);
    if (it == field_index_.end()) {
        return nullptr;
    }
    return &fields_[it->second];
}

ValidationResult Schema::validate_json(const std::string& json_str) const noexcept {
    ValidationResult result;

    // Parse JSON using simdjson
    simdjson::ondemand::parser parser;
    simdjson::padded_string padded_json(json_str);

    simdjson::ondemand::document doc;
    auto error = parser.iterate(padded_json).get(doc);

    if (error) {
        result.add_error(
            {"body"},
            "Invalid JSON",
            "value_error.jsondecode"
        );
        return result;
    }

    // Get root object
    simdjson::ondemand::object obj;
    error = doc.get_object().get(obj);
    if (error) {
        result.add_error(
            {"body"},
            "Expected object, got " + std::string(simdjson::error_message(error)),
            "type_error.object"
        );
        return result;
    }

    // Track which fields we've seen
    std::unordered_set<std::string> seen_fields;

    // Validate each field in the JSON
    for (auto field_pair : obj) {
        std::string_view key;
        auto key_error = field_pair.unescaped_key().get(key);
        if (key_error) {
            continue;
        }

        std::string field_name(key);
        seen_fields.insert(field_name);

        // Check if this field exists in schema
        const Field* field_def = get_field(field_name);
        if (!field_def) {
            // Unknown field - could warn but FastAPI allows extra fields by default
            continue;
        }

        // Validate the field value
        std::vector<std::string> location = {"body", field_name};
        auto value = field_pair.value();
        validate_field_impl(*field_def, &value, location, result);
    }

    // Check for required fields that are missing
    for (const auto& field : fields_) {
        if (field.required && seen_fields.find(field.name) == seen_fields.end()) {
            result.add_error(
                {"body", field.name},
                "field required",
                "value_error.missing"
            );
        }
    }

    return result;
}

void Schema::validate_field_impl(
    const Field& field,
    void* value_ptr,
    std::vector<std::string>& location,
    ValidationResult& result
) const noexcept {
    auto& value = *static_cast<simdjson::ondemand::value*>(value_ptr);
    // Check type
    simdjson::ondemand::json_type json_type;
    auto type_error = value.type().get(json_type);
    if (type_error) {
        result.add_error(
            location,
            "Cannot determine value type",
            "value_error.type"
        );
        return;
    }

    bool type_valid = false;

    switch (field.type) {
        case SchemaType::STRING:
            type_valid = (json_type == simdjson::ondemand::json_type::string);
            if (!type_valid) {
                result.add_error(
                    location,
                    "value is not a valid string",
                    "type_error.string"
                );
            }
            break;

        case SchemaType::INTEGER:
            type_valid = (json_type == simdjson::ondemand::json_type::number);
            if (type_valid) {
                // Check if it's actually an integer
                int64_t int_val;
                auto int_error = value.get_int64().get(int_val);
                if (int_error) {
                    result.add_error(
                        location,
                        "value is not a valid integer",
                        "type_error.integer"
                    );
                    type_valid = false;
                }
            } else {
                result.add_error(
                    location,
                    "value is not a valid integer",
                    "type_error.integer"
                );
            }
            break;

        case SchemaType::FLOAT:
            type_valid = (json_type == simdjson::ondemand::json_type::number);
            if (!type_valid) {
                result.add_error(
                    location,
                    "value is not a valid number",
                    "type_error.float"
                );
            }
            break;

        case SchemaType::BOOLEAN:
            type_valid = (json_type == simdjson::ondemand::json_type::boolean);
            if (!type_valid) {
                result.add_error(
                    location,
                    "value is not a valid boolean",
                    "type_error.bool"
                );
            }
            break;

        case SchemaType::ARRAY:
            type_valid = (json_type == simdjson::ondemand::json_type::array);
            if (!type_valid) {
                result.add_error(
                    location,
                    "value is not a valid array",
                    "type_error.list"
                );
            } else if (field.array_item_schema) {
                // Validate array items
                simdjson::ondemand::array arr;
                auto arr_error = value.get_array().get(arr);
                if (!arr_error) {
                    size_t index = 0;
                    for (auto item : arr) {
                        location.push_back(std::to_string(index));
                        // TODO: Validate nested objects
                        location.pop_back();
                        index++;
                    }
                }
            }
            break;

        case SchemaType::OBJECT:
            type_valid = (json_type == simdjson::ondemand::json_type::object);
            if (!type_valid) {
                result.add_error(
                    location,
                    "value is not a valid object",
                    "type_error.dict"
                );
            } else if (field.object_schema) {
                // Validate nested object
                // TODO: Recursive validation
            }
            break;

        case SchemaType::NULL_TYPE:
            type_valid = (json_type == simdjson::ondemand::json_type::null);
            if (!type_valid) {
                result.add_error(
                    location,
                    "value is not null",
                    "type_error.none"
                );
            }
            break;

        case SchemaType::ANY:
            type_valid = true;  // Accept anything
            break;
    }
}

ValidationResult Schema::validate_and_coerce(
    const std::string& json_str,
    std::string& output_json
) const noexcept {
    // For now, just validate without coercion
    // TODO: Implement proper coercion
    auto result = validate_json(json_str);
    if (result.valid) {
        output_json = json_str;
    }
    return result;
}

// ============================================================================
// SchemaRegistry Implementation
// ============================================================================

SchemaRegistry& SchemaRegistry::instance() {
    static SchemaRegistry registry;
    return registry;
}

void SchemaRegistry::register_schema(const std::string& name, std::shared_ptr<Schema> schema) {
    schemas_[name] = std::move(schema);
    LOG_DEBUG("Schema", "Registered schema: %s", name.c_str());
}

std::shared_ptr<Schema> SchemaRegistry::get_schema(const std::string& name) const {
    auto it = schemas_.find(name);
    if (it != schemas_.end()) {
        return it->second;
    }
    return nullptr;
}

bool SchemaRegistry::has_schema(const std::string& name) const {
    return schemas_.find(name) != schemas_.end();
}

void SchemaRegistry::clear() {
    schemas_.clear();
}

// ============================================================================
// TypeValidator Implementation
// ============================================================================

std::optional<int64_t> TypeValidator::parse_int(const std::string& str) noexcept {
    if (str.empty()) {
        return std::nullopt;
    }

    int64_t value;
    auto result = std::from_chars(str.data(), str.data() + str.size(), value);

    if (result.ec == std::errc() && result.ptr == str.data() + str.size()) {
        return value;
    }

    return std::nullopt;
}

std::optional<double> TypeValidator::parse_float(const std::string& str) noexcept {
    if (str.empty()) {
        return std::nullopt;
    }

    // Use manual parsing instead of std::stod (no exceptions)
    char* end;
    double value = std::strtod(str.c_str(), &end);

    // Check if we parsed the entire string
    if (end == str.c_str() + str.size()) {
        return value;
    }

    return std::nullopt;
}

std::optional<bool> TypeValidator::parse_bool(const std::string& str) noexcept {
    std::string lower = str;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    if (lower == "true" || lower == "1") {
        return true;
    } else if (lower == "false" || lower == "0") {
        return false;
    }

    return std::nullopt;
}

bool TypeValidator::is_integer(const std::string& str) noexcept {
    return parse_int(str).has_value();
}

bool TypeValidator::is_float(const std::string& str) noexcept {
    return parse_float(str).has_value();
}

} // namespace http
} // namespace fasterapi
