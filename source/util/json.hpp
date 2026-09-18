#pragma once

/* Minimal JSON: just what this project's own data needs (the install manifest, the install
   record and the settings file).  Lives in `util/` because it is generic infrastructure that
   three modules use, not part of the manifest format.  Deliberately free of libnx and
   third-party code so the host tests can run it directly.
   Supports: object / array / string (with \" \\ \/ \b \f \n \r \t \uXXXX) / number / true / false / null.
   Does not support: comments, trailing commas, NaN/Infinity, and it makes no precision
   promise beyond 2^53.  Objects keep insertion order, so writing back is order-stable. */

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace acnh_manager::json {

enum class Type { Null, Bool, Number, String, Array, Object };

struct Value {
    Type type{Type::Null};
    bool boolean{false};
    double number{0.0};
    std::string string;
    std::vector<Value> array;
    std::vector<std::pair<std::string, Value>> object;

    bool IsNull() const { return type == Type::Null; }
    bool IsBool() const { return type == Type::Bool; }
    bool IsNumber() const { return type == Type::Number; }
    bool IsString() const { return type == Type::String; }
    bool IsArray() const { return type == Type::Array; }
    bool IsObject() const { return type == Type::Object; }

    /* Returns nullptr when the key is missing or has the wrong type. */
    const Value *Find(std::string_view key) const {
        if (type != Type::Object) {
            return nullptr;
        }
        for (const auto &entry : object) {
            if (entry.first == key) {
                return &entry.second;
            }
        }
        return nullptr;
    }

    const Value *At(std::size_t index) const {
        if (type != Type::Array || index >= array.size()) {
            return nullptr;
        }
        return &array[index];
    }

    std::size_t Size() const { return type == Type::Array ? array.size() : 0; }

    /* Scalar accessors: wrong type returns the fallback. */
    const std::string &StringOr(const std::string &fallback) const {
        return type == Type::String ? string : fallback;
    }
    bool BoolOr(bool fallback) const { return type == Type::Bool ? boolean : fallback; }
    double NumberOr(double fallback) const { return type == Type::Number ? number : fallback; }

    static Value MakeNull() { return Value{}; }
    static Value MakeBool(bool v) {
        Value value;
        value.type = Type::Bool;
        value.boolean = v;
        return value;
    }
    static Value MakeNumber(double v) {
        Value value;
        value.type = Type::Number;
        value.number = v;
        return value;
    }
    static Value MakeString(std::string v) {
        Value value;
        value.type = Type::String;
        value.string = std::move(v);
        return value;
    }
};

/* Returns false with the position of the first error when parsing fails. */
bool Parse(std::string_view text, Value *out, std::string *error);

/* Compact output (no newlines, no reordering); strings escaped per JSON rules. */
std::string Dump(const Value &value);

/* Integer accessor: a non-integer or out-of-range value returns the fallback. */
std::int64_t IntOr(const Value &value, std::int64_t fallback);

}  // namespace acnh_manager::json
