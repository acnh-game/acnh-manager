#include "json.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace acnh_manager::json {
namespace {

class Parser {
public:
    Parser(std::string_view text, std::string *error) : m_text(text), m_error(error) {}

    bool Run(Value *out) {
        SkipWhitespace();
        if (!ParseValue(out)) {
            return false;
        }
        SkipWhitespace();
        if (m_pos != m_text.size()) {
            return Fail("trailing content after top-level value");
        }
        return true;
    }

private:
    bool Fail(const char *what) {
        if (m_error != nullptr && m_error->empty()) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "%s at byte %zu", what, m_pos);
            *m_error = buf;
        }
        return false;
    }

    bool AtEnd() const { return m_pos >= m_text.size(); }
    char Peek() const { return AtEnd() ? '\0' : m_text[m_pos]; }

    void SkipWhitespace() {
        while (!AtEnd()) {
            const char c = m_text[m_pos];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++m_pos;
            } else {
                break;
            }
        }
    }

    bool Literal(std::string_view word) {
        if (m_text.substr(m_pos, word.size()) != word) {
            return Fail("invalid literal");
        }
        m_pos += word.size();
        return true;
    }

    static void AppendUtf8(std::string *out, std::uint32_t code) {
        if (code < 0x80) {
            out->push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            out->push_back(static_cast<char>(0xC0 | (code >> 6)));
            out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else if (code < 0x10000) {
            out->push_back(static_cast<char>(0xE0 | (code >> 12)));
            out->push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            out->push_back(static_cast<char>(0xF0 | (code >> 18)));
            out->push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
            out->push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }

    bool ParseHex4(std::uint32_t *out) {
        if (m_text.size() - m_pos < 4) {
            return Fail("truncated \\u escape");
        }
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = m_text[m_pos + static_cast<std::size_t>(i)];
            value <<= 4;
            if (c >= '0' && c <= '9') {
                value |= static_cast<std::uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                value |= static_cast<std::uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                value |= static_cast<std::uint32_t>(c - 'A' + 10);
            } else {
                return Fail("bad hex digit in \\u escape");
            }
        }
        m_pos += 4;
        *out = value;
        return true;
    }

    bool ParseString(std::string *out) {
        if (Peek() != '"') {
            return Fail("expected string");
        }
        ++m_pos;
        out->clear();
        while (true) {
            if (AtEnd()) {
                return Fail("unterminated string");
            }
            const char c = m_text[m_pos++];
            if (c == '"') {
                return true;
            }
            if (c != '\\') {
                *out += c;
                continue;
            }
            if (AtEnd()) {
                return Fail("unterminated escape");
            }
            const char esc = m_text[m_pos++];
            switch (esc) {
                case '"': out->push_back('"'); break;
                case '\\': out->push_back('\\'); break;
                case '/': out->push_back('/'); break;
                case 'b': out->push_back('\b'); break;
                case 'f': out->push_back('\f'); break;
                case 'n': out->push_back('\n'); break;
                case 'r': out->push_back('\r'); break;
                case 't': out->push_back('\t'); break;
                case 'u': {
                    std::uint32_t code = 0;
                    if (!ParseHex4(&code)) {
                        return false;
                    }
                    if (code >= 0xD800 && code <= 0xDBFF) {
                        /* 代理对:高代理必须紧跟 \uXXXX 低代理。 */
                        if (m_text.substr(m_pos, 2) != "\\u") {
                            return Fail("lone surrogate");
                        }
                        m_pos += 2;
                        std::uint32_t low = 0;
                        if (!ParseHex4(&low)) {
                            return false;
                        }
                        if (low < 0xDC00 || low > 0xDFFF) {
                            return Fail("bad low surrogate");
                        }
                        code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                    } else if (code >= 0xDC00 && code <= 0xDFFF) {
                        return Fail("lone low surrogate");
                    }
                    AppendUtf8(out, code);
                    break;
                }
                default:
                    return Fail("bad escape");
            }
        }
    }

    bool ParseNumber(Value *out) {
        const std::size_t start = m_pos;
        if (Peek() == '-') {
            ++m_pos;
        }
        if (Peek() == '0') {
            ++m_pos;
        } else if (Peek() >= '1' && Peek() <= '9') {
            while (Peek() >= '0' && Peek() <= '9') {
                ++m_pos;
            }
        } else {
            return Fail("bad number");
        }
        if (Peek() == '.') {
            ++m_pos;
            if (!(Peek() >= '0' && Peek() <= '9')) {
                return Fail("bad fraction");
            }
            while (Peek() >= '0' && Peek() <= '9') {
                ++m_pos;
            }
        }
        if (Peek() == 'e' || Peek() == 'E') {
            ++m_pos;
            if (Peek() == '+' || Peek() == '-') {
                ++m_pos;
            }
            if (!(Peek() >= '0' && Peek() <= '9')) {
                return Fail("bad exponent");
            }
            while (Peek() >= '0' && Peek() <= '9') {
                ++m_pos;
            }
        }
        const std::string token(m_text.substr(start, m_pos - start));
        *out = Value::MakeNumber(std::strtod(token.c_str(), nullptr));
        return true;
    }

    bool ParseArray(Value *out) {
        ++m_pos; /* '[' */
        out->type = Type::Array;
        SkipWhitespace();
        if (Peek() == ']') {
            ++m_pos;
            return true;
        }
        while (true) {
            Value element;
            if (!ParseValue(&element)) {
                return false;
            }
            out->array.push_back(std::move(element));
            SkipWhitespace();
            const char c = Peek();
            if (c == ',') {
                ++m_pos;
                SkipWhitespace();
                continue;
            }
            if (c == ']') {
                ++m_pos;
                return true;
            }
            return Fail("expected ',' or ']'");
        }
    }

    bool ParseObject(Value *out) {
        ++m_pos; /* '{' */
        out->type = Type::Object;
        SkipWhitespace();
        if (Peek() == '}') {
            ++m_pos;
            return true;
        }
        while (true) {
            std::string key;
            if (!ParseString(&key)) {
                return false;
            }
            SkipWhitespace();
            if (Peek() != ':') {
                return Fail("expected ':'");
            }
            ++m_pos;
            SkipWhitespace();
            Value value;
            if (!ParseValue(&value)) {
                return false;
            }
            out->object.emplace_back(std::move(key), std::move(value));
            SkipWhitespace();
            const char c = Peek();
            if (c == ',') {
                ++m_pos;
                SkipWhitespace();
                continue;
            }
            if (c == '}') {
                ++m_pos;
                return true;
            }
            return Fail("expected ',' or '}'");
        }
    }

    bool ParseValue(Value *out) {
        SkipWhitespace();
        switch (Peek()) {
            case '{': return ParseObject(out);
            case '[': return ParseArray(out);
            case '"': {
                out->type = Type::String;
                return ParseString(&out->string);
            }
            case 't':
                if (!Literal("true")) {
                    return false;
                }
                *out = Value::MakeBool(true);
                return true;
            case 'f':
                if (!Literal("false")) {
                    return false;
                }
                *out = Value::MakeBool(false);
                return true;
            case 'n':
                if (!Literal("null")) {
                    return false;
                }
                *out = Value::MakeNull();
                return true;
            default:
                if (Peek() == '-' || (Peek() >= '0' && Peek() <= '9')) {
                    return ParseNumber(out);
                }
                return Fail("unexpected character");
        }
    }

    std::string_view m_text;
    std::string *m_error;
    std::size_t m_pos{0};
};

void DumpString(const std::string &value, std::string *out) {
    out->push_back('"');
    for (const char c : value) {
        switch (c) {
            case '"': out->append("\\\""); break;
            case '\\': out->append("\\\\"); break;
            case '\n': out->append("\\n"); break;
            case '\r': out->append("\\r"); break;
            case '\t': out->append("\\t"); break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out->append(buf);
                } else {
                    out->push_back(c);
                }
        }
    }
    out->push_back('"');
}

void DumpValue(const Value &value, std::string *out) {
    switch (value.type) {
        case Type::Null:
            out->append("null");
            return;
        case Type::Bool:
            out->append(value.boolean ? "true" : "false");
            return;
        case Type::Number: {
            char buf[32];
            if (value.number == std::floor(value.number) && std::fabs(value.number) < 1e15) {
                std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(value.number));
            } else {
                std::snprintf(buf, sizeof(buf), "%.10g", value.number);
            }
            out->append(buf);
            return;
        }
        case Type::String:
            DumpString(value.string, out);
            return;
        case Type::Array: {
            out->push_back('[');
            for (std::size_t i = 0; i < value.array.size(); ++i) {
                if (i != 0) {
                    out->push_back(',');
                }
                DumpValue(value.array[i], out);
            }
            out->push_back(']');
            return;
        }
        case Type::Object: {
            out->push_back('{');
            for (std::size_t i = 0; i < value.object.size(); ++i) {
                if (i != 0) {
                    out->push_back(',');
                }
                DumpString(value.object[i].first, out);
                out->push_back(':');
                DumpValue(value.object[i].second, out);
            }
            out->push_back('}');
            return;
        }
    }
}

}  // namespace

bool Parse(std::string_view text, Value *out, std::string *error) {
    if (error != nullptr) {
        error->clear();
    }
    if (out == nullptr) {
        return false;
    }
    *out = Value{};
    Parser parser(text, error);
    return parser.Run(out);
}

std::string Dump(const Value &value) {
    std::string out;
    DumpValue(value, &out);
    return out;
}

std::int64_t IntOr(const Value &value, std::int64_t fallback) {
    if (value.type != Type::Number || value.number != std::floor(value.number)) {
        return fallback;
    }
    if (value.number < -9.2e18 || value.number > 9.2e18) {
        return fallback;
    }
    return static_cast<std::int64_t>(value.number);
}

}  // namespace acnh_manager::json
