// SOGJson.cpp — see SOGJson.h.
#include "SOGJson.h"

#include <cstring>
#include <locale>
#include <sstream>

namespace sog::json {

const Value* Value::Find(const char* key) const
{
    if (type != Type::Object) {
        return nullptr;
    }
    for (const auto& member : object) {
        if (member.first == key) {
            return &member.second;
        }
    }
    return nullptr;
}

namespace {

class Parser {
public:
    Parser(const char* text, size_t length) : P(text), End(text + length) {}

    bool Document(Value& out, std::string* error)
    {
        SkipSpace();
        if (!ParseValue(out, 0)) {
            return Fail(error);
        }
        SkipSpace();
        if (P != End) {
            Message = "trailing characters";
            return Fail(error);
        }
        return true;
    }

private:
    const char* P;
    const char* End;
    const char* Begin = P;
    std::string Message;

    bool Fail(std::string* error) const
    {
        if (error) {
            *error = "JSON: " + (Message.empty() ? std::string("syntax error") : Message) +
                     " at offset " + std::to_string(P - Begin);
        }
        return false;
    }

    void SkipSpace()
    {
        while (P < End && (*P == ' ' || *P == '\t' || *P == '\n' || *P == '\r')) {
            ++P;
        }
    }

    bool Literal(const char* word)
    {
        const size_t n = std::strlen(word);
        if (size_t(End - P) < n || std::memcmp(P, word, n) != 0) {
            return false;
        }
        P += n;
        return true;
    }

    bool ParseValue(Value& v, int depth)
    {
        if (depth > 64) {
            Message = "nesting too deep";
            return false;
        }
        if (P >= End) {
            Message = "unexpected end";
            return false;
        }
        switch (*P) {
            case '{': return ParseObject(v, depth);
            case '[': return ParseArray(v, depth);
            case '"': v.type = Value::Type::String; return ParseString(v.string);
            case 't': v.type = Value::Type::Bool; v.boolean = true; return Literal("true");
            case 'f': v.type = Value::Type::Bool; v.boolean = false; return Literal("false");
            case 'n': v.type = Value::Type::Null; return Literal("null");
            default:  return ParseNumber(v);
        }
    }

    bool ParseObject(Value& v, int depth)
    {
        v.type = Value::Type::Object;
        ++P;
        SkipSpace();
        if (P < End && *P == '}') {
            ++P;
            return true;
        }
        for (;;) {
            SkipSpace();
            std::string key;
            if (P >= End || *P != '"' || !ParseString(key)) {
                Message = "expected member name";
                return false;
            }
            SkipSpace();
            if (P >= End || *P != ':') {
                Message = "expected ':'";
                return false;
            }
            ++P;
            SkipSpace();
            v.object.emplace_back(std::move(key), Value());
            if (!ParseValue(v.object.back().second, depth + 1)) {
                return false;
            }
            SkipSpace();
            if (P < End && *P == ',') {
                ++P;
                continue;
            }
            if (P < End && *P == '}') {
                ++P;
                return true;
            }
            Message = "expected ',' or '}'";
            return false;
        }
    }

    bool ParseArray(Value& v, int depth)
    {
        v.type = Value::Type::Array;
        ++P;
        SkipSpace();
        if (P < End && *P == ']') {
            ++P;
            return true;
        }
        for (;;) {
            SkipSpace();
            v.array.emplace_back();
            if (!ParseValue(v.array.back(), depth + 1)) {
                return false;
            }
            SkipSpace();
            if (P < End && *P == ',') {
                ++P;
                continue;
            }
            if (P < End && *P == ']') {
                ++P;
                return true;
            }
            Message = "expected ',' or ']'";
            return false;
        }
    }

    static void AppendUtf8(std::string& s, unsigned cp)
    {
        if (cp < 0x80) {
            s += char(cp);
        } else if (cp < 0x800) {
            s += char(0xC0 | (cp >> 6));
            s += char(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            s += char(0xE0 | (cp >> 12));
            s += char(0x80 | ((cp >> 6) & 0x3F));
            s += char(0x80 | (cp & 0x3F));
        } else {
            s += char(0xF0 | (cp >> 18));
            s += char(0x80 | ((cp >> 12) & 0x3F));
            s += char(0x80 | ((cp >> 6) & 0x3F));
            s += char(0x80 | (cp & 0x3F));
        }
    }

    bool Hex4(unsigned& out)
    {
        if (End - P < 4) {
            return false;
        }
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = *P++;
            out <<= 4;
            if (c >= '0' && c <= '9') out |= unsigned(c - '0');
            else if (c >= 'a' && c <= 'f') out |= unsigned(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= unsigned(c - 'A' + 10);
            else return false;
        }
        return true;
    }

    bool ParseString(std::string& s)
    {
        ++P;                                                     // opening quote
        while (P < End) {
            const char c = *P++;
            if (c == '"') {
                return true;
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                Message = "control character in string";
                return false;
            }
            if (c != '\\') {
                s += c;
                continue;
            }
            if (P >= End) {
                break;
            }
            const char e = *P++;
            switch (e) {
                case '"': s += '"'; break;
                case '\\': s += '\\'; break;
                case '/': s += '/'; break;
                case 'b': s += '\b'; break;
                case 'f': s += '\f'; break;
                case 'n': s += '\n'; break;
                case 'r': s += '\r'; break;
                case 't': s += '\t'; break;
                case 'u': {
                    unsigned cp;
                    if (!Hex4(cp)) {
                        Message = "bad \\u escape";
                        return false;
                    }
                    if (cp >= 0xD800 && cp <= 0xDBFF) {            // surrogate pair
                        unsigned lo;
                        if (End - P < 6 || P[0] != '\\' || P[1] != 'u') {
                            Message = "unpaired surrogate";
                            return false;
                        }
                        P += 2;
                        if (!Hex4(lo) || lo < 0xDC00 || lo > 0xDFFF) {
                            Message = "unpaired surrogate";
                            return false;
                        }
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    AppendUtf8(s, cp);
                    break;
                }
                default:
                    Message = "bad escape";
                    return false;
            }
        }
        Message = "unterminated string";
        return false;
    }

    bool ParseNumber(Value& v)
    {
        const char* start = P;
        if (P < End && *P == '-') ++P;
        if (P < End && *P == '0') {
            ++P;
        } else if (P < End && *P >= '1' && *P <= '9') {
            while (P < End && *P >= '0' && *P <= '9') ++P;
        } else {
            Message = "unexpected character";
            return false;
        }
        if (P < End && *P == '.') {
            ++P;
            if (P >= End || *P < '0' || *P > '9') {
                Message = "bad number";
                return false;
            }
            while (P < End && *P >= '0' && *P <= '9') ++P;
        }
        if (P < End && (*P == 'e' || *P == 'E')) {
            ++P;
            if (P < End && (*P == '+' || *P == '-')) ++P;
            if (P >= End || *P < '0' || *P > '9') {
                Message = "bad number";
                return false;
            }
            while (P < End && *P >= '0' && *P <= '9') ++P;
        }
        std::istringstream in(std::string(start, P));
        in.imbue(std::locale::classic());
        in >> v.number;
        if (in.fail()) {
            Message = "bad number";
            return false;
        }
        v.type = Value::Type::Number;
        return true;
    }
};

} // namespace

bool Parse(const char* text, size_t length, Value& out, std::string* error)
{
    out = Value();
    Parser parser(text, length);
    return parser.Document(out, error);
}

} // namespace sog::json
