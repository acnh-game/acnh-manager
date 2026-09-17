#pragma once

/* 极简 JSON(仅覆盖本项目自有数据:安装清单与 state.json)。
   刻意不依赖 libnx 或第三方库,便于主机侧直接跑测试。
   支持:对象 / 数组 / 字符串(含 \" \\ \/ \b \f \n \r \t \uXXXX)/ 数字 / true / false / null。
   不支持:注释、尾随逗号、NaN/Infinity、超长数字的精度保证(>2^53 会丢精度)。
   对象保持插入顺序,便于"写回时键顺序稳定"。 */

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

    /* 缺键 / 类型不符时返回 nullptr。 */
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

    /* 取标量:类型不符时返回 fallback。 */
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

/* 解析失败时返回 false 并给出首个错误的位置说明。 */
bool Parse(std::string_view text, Value *out, std::string *error);

/* 紧凑输出(不换行、不排序);字符串按 JSON 规则转义。 */
std::string Dump(const Value &value);

/* 数字的整数读取:非整数或超出范围返回 fallback。 */
std::int64_t IntOr(const Value &value, std::int64_t fallback);

}  // namespace acnh_manager::json
