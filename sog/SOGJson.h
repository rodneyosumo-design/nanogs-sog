// SOGJson.h — minimal JSON DOM for meta.json / lod-meta.json. No exceptions; numbers are parsed
// with the classic "C" locale so a process-wide locale can't change the decimal separator.
#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace sog::json {

struct Value {
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::vector<Value> array;
    std::vector<std::pair<std::string, Value>> object;

    bool IsNumber() const { return type == Type::Number; }
    bool IsString() const { return type == Type::String; }
    bool IsArray() const { return type == Type::Array; }
    bool IsObject() const { return type == Type::Object; }
    const Value* Find(const char* key) const;    // object member or null
};

bool Parse(const char* text, size_t length, Value& out, std::string* error);

} // namespace sog::json
