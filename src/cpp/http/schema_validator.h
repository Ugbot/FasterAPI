/**
 * Schema Validator
 *
 * High-performance JSON schema validation in C++.
 * Faster than Pydantic - validates in ~100ns to 1μs per request.
 *
 * Features:
 * - Type validation (string, int, float, bool, array, object)
 * - Type coercion (e.g., "123" → 123, "true" → true)
 * - Required/optional fields
 * - Nested objects and arrays
 * - Default values
 * - Detailed validation errors (FastAPI 422 format)
 * - Zero-copy validation where possible
 * - Pre-compiled schemas for maximum performance
 *
 * Python integration:
 * - Pydantic models converted to C++ schemas at registration time
 * - All validation happens in C++ during request handling
 * - No Python calls in hot path
 */

#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <variant>
#include <optional>
#include <memory>

namespace fasterapi {
namespace http {

/**
 * JSON value types supported by the validator.
 */
enum class SchemaType {
    STRING,
    INTEGER,
    FLOAT,
    BOOLEAN,
    ARRAY,
    OBJECT,
    NULL_TYPE,
    ANY  // Accept any type
};

/**
 * Convert SchemaType to string for error messages.
 */
const char* schema_type_to_string(SchemaType type) noexcept;

/**
 * Validation error location and message.
 */
struct ValidationError {
    std::vector<std::string> loc;  // Error location (e.g., ["body", "user", "age"])
    std::string msg;               // Error message
    std::string type;              // Error type (e.g., "type_error.integer")

    ValidationError() = default;
    ValidationError(std::vector<std::string> l, std::string m, std::string t)
        : loc(std::move(l)), msg(std::move(m)), type(std::move(t)) {}
};

/**
 * Validation result.
 */
struct ValidationResult {
    bool valid = true;
    std::vector<ValidationError> errors;

    void add_error(std::vector<std::string> loc, std::string msg, std::string type) {
        valid = false;
        errors.emplace_back(std::move(loc), std::move(msg), std::move(type));
    }

    // Check if validation succeeded
    operator bool() const noexcept { return valid; }
};

/**
 * Default value for a field.
 *
 * Can hold different types to represent default values.
 */
using DefaultValue = std::variant<
    std::monostate,  // No default
    std::string,
    int64_t,
    double,
    bool
>;

// Forward declaration
class Schema;

/**
 * Field definition in a schema.
 */
struct Field {
    std::string name;
    SchemaType type;
    bool required = true;
    DefaultValue default_value;

    // For nested objects
    std::shared_ptr<Schema> object_schema;

    // For arrays
    SchemaType array_item_type = SchemaType::ANY;
    std::shared_ptr<Schema> array_item_schema;  // For arrays of objects

    // Validation constraints (optional)
    std::optional<int64_t> min_value;
    std::optional<int64_t> max_value;
    std::optional<size_t> min_length;
    std::optional<size_t> max_length;

    Field() = default;
    Field(std::string n, SchemaType t, bool req = true)
        : name(std::move(n)), type(t), required(req) {}
};

/**
 * Schema definition for an object.
 *
 * Represents the structure of a Pydantic model or JSON object.
 */
class Schema {
public:
    Schema() = default;
    explicit Schema(std::string name) : name_(std::move(name)) {}

    /**
     * Add a field to the schema.
     */
    void add_field(Field field);

    /**
     * Add a simple field (convenience method).
     */
    void add_field(std::string name, SchemaType type, bool required = true);

    /**
     * Validate JSON string against this schema.
     *
     * @param json_str JSON string to validate
     * @return ValidationResult with errors if validation failed
     */
    ValidationResult validate_json(const std::string& json_str) const noexcept;

    /**
     * Validate and coerce JSON string, returning validated JSON.
     *
     * Performs type coercion where appropriate:
     * - "123" → 123 for integer fields
     * - "true" → true for boolean fields
     * - etc.
     *
     * @param json_str Input JSON string
     * @param output_json Output JSON string (validated and coerced)
     * @return ValidationResult
     */
    ValidationResult validate_and_coerce(
        const std::string& json_str,
        std::string& output_json
    ) const noexcept;

    /**
     * Get schema name.
     */
    const std::string& get_name() const noexcept { return name_; }

    /**
     * Get fields.
     */
    const std::vector<Field>& get_fields() const noexcept { return fields_; }

    /**
     * Check if field exists.
     */
    bool has_field(const std::string& field_name) const noexcept;

    /**
     * Get field by name.
     */
    const Field* get_field(const std::string& field_name) const noexcept;

private:
    std::string name_;
    std::vector<Field> fields_;
    std::unordered_map<std::string, size_t> field_index_;  // Fast lookup

    // Internal validation methods (implemented in .cpp file)
    // Using void* to avoid including simdjson header here
    void validate_field_impl(
        const Field& field,
        void* value,
        std::vector<std::string>& location,
        ValidationResult& result
    ) const noexcept;
};

/**
 * Global schema registry.
 *
 * Stores schemas registered from Python (Pydantic models).
 * Thread-safe for reads after initialization.
 */
class SchemaRegistry {
public:
    /**
     * Get singleton instance.
     */
    static SchemaRegistry& instance();

    /**
     * Register a schema.
     *
     * Called from Python at route registration time.
     */
    void register_schema(const std::string& name, std::shared_ptr<Schema> schema);

    /**
     * Get schema by name.
     *
     * @return Schema pointer, or nullptr if not found
     */
    std::shared_ptr<Schema> get_schema(const std::string& name) const;

    /**
     * Check if schema exists.
     */
    bool has_schema(const std::string& name) const;

    /**
     * Clear all schemas (for testing).
     */
    void clear();

private:
    SchemaRegistry() = default;
    std::unordered_map<std::string, std::shared_ptr<Schema>> schemas_;
};

/**
 * Schema builder - convenient interface for constructing schemas.
 *
 * Example:
 *   auto schema = SchemaBuilder("User")
 *       .field("id", SchemaType::INTEGER, true)
 *       .field("name", SchemaType::STRING, true)
 *       .field("email", SchemaType::STRING, false)
 *       .build();
 */
class SchemaBuilder {
public:
    explicit SchemaBuilder(std::string name) : schema_(std::make_shared<Schema>(std::move(name))) {}

    /**
     * Add a simple field.
     */
    SchemaBuilder& field(std::string name, SchemaType type, bool required = true) {
        schema_->add_field(std::move(name), type, required);
        return *this;
    }

    /**
     * Add a field with constraints.
     */
    SchemaBuilder& field(Field field) {
        schema_->add_field(std::move(field));
        return *this;
    }

    /**
     * Add a nested object field.
     */
    SchemaBuilder& object_field(
        std::string name,
        std::shared_ptr<Schema> nested_schema,
        bool required = true
    ) {
        Field f(std::move(name), SchemaType::OBJECT, required);
        f.object_schema = std::move(nested_schema);
        schema_->add_field(std::move(f));
        return *this;
    }

    /**
     * Add an array field.
     */
    SchemaBuilder& array_field(
        std::string name,
        SchemaType item_type,
        bool required = true
    ) {
        Field f(std::move(name), SchemaType::ARRAY, required);
        f.array_item_type = item_type;
        schema_->add_field(std::move(f));
        return *this;
    }

    /**
     * Build and return the schema.
     */
    std::shared_ptr<Schema> build() {
        return schema_;
    }

private:
    std::shared_ptr<Schema> schema_;
};

/**
 * Fast type checking and coercion utilities.
 */
class TypeValidator {
public:
    /**
     * Try to parse string as integer.
     */
    static std::optional<int64_t> parse_int(const std::string& str) noexcept;

    /**
     * Try to parse string as float.
     */
    static std::optional<double> parse_float(const std::string& str) noexcept;

    /**
     * Try to parse string as boolean.
     * Accepts: "true", "false", "1", "0" (case-insensitive)
     */
    static std::optional<bool> parse_bool(const std::string& str) noexcept;

    /**
     * Check if string is a valid integer.
     */
    static bool is_integer(const std::string& str) noexcept;

    /**
     * Check if string is a valid float.
     */
    static bool is_float(const std::string& str) noexcept;
};

} // namespace http
} // namespace fasterapi
