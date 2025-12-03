#pragma once

#include <string>
#include <vector>
#include <map>
#include <variant>
#include <memory>
#include <optional>

namespace TinyJson {

enum class Type {
    Null,
    Boolean,
    Number,
    String,
    Array,
    Object
};

struct Value;

using Array = std::vector<Value>;
using Object = std::map<std::string, Value>;

struct Value {
    Type type = Type::Null;
    std::variant<std::monostate, bool, double, std::string, std::shared_ptr<Array>, std::shared_ptr<Object>> data;

    Value() = default;
    Value(bool v) : type(Type::Boolean), data(v) {}
    Value(double v) : type(Type::Number), data(v) {}
    Value(int v) : type(Type::Number), data(static_cast<double>(v)) {}
    Value(const std::string& v) : type(Type::String), data(v) {}
    Value(const char* v) : type(Type::String), data(std::string(v)) {}
    Value(const Array& v) : type(Type::Array), data(std::make_shared<Array>(v)) {}
    Value(const Object& v) : type(Type::Object), data(std::make_shared<Object>(v)) {}

    bool isNull() const { return type == Type::Null; }
    bool isBool() const { return type == Type::Boolean; }
    bool isNumber() const { return type == Type::Number; }
    bool isString() const { return type == Type::String; }
    bool isArray() const { return type == Type::Array; }
    bool isObject() const { return type == Type::Object; }

    bool asBool() const { 
        if (auto* v = std::get_if<bool>(&data)) return *v;
        return false; 
    }
    double asNumber() const { 
        if (auto* v = std::get_if<double>(&data)) return *v;
        return 0.0; 
    }
    int asInt() const { 
        if (auto* v = std::get_if<double>(&data)) return static_cast<int>(*v);
        return 0; 
    }
    std::string asString() const { 
        if (auto* v = std::get_if<std::string>(&data)) return *v;
        return ""; 
    }
    const Array& asArray() const { 
        if (auto* v = std::get_if<std::shared_ptr<Array>>(&data)) return **v;
        static Array emptyArr;
        return emptyArr; 
    }
    const Object& asObject() const { 
        if (auto* v = std::get_if<std::shared_ptr<Object>>(&data)) return **v;
        static Object emptyObj;
        return emptyObj; 
    }
    
    // Helpers
    const Value& operator[](const std::string& key) const {
        if (!isObject()) { static Value nullVal; return nullVal; }
        const auto& obj = asObject();
        auto it = obj.find(key);
        if (it != obj.end()) return it->second;
        static Value nullVal;
        return nullVal;
    }

    const Value& operator[](size_t index) const {
        if (!isArray()) { static Value nullVal; return nullVal; }
        const auto& arr = asArray();
        if (index < arr.size()) return arr[index];
        static Value nullVal;
        return nullVal;
    }
};

class Parser {
public:
    static std::optional<Value> parse(const std::string& json);
private:
    const std::string& str;
    size_t pos = 0;

    Parser(const std::string& s) : str(s) {}
    void skipWhitespace();
    std::optional<Value> parseValue();
    std::optional<Value> parseObject();
    std::optional<Value> parseArray();
    std::optional<std::string> parseString();
    std::optional<double> parseNumber();
    std::optional<bool> parseBool();
    std::optional<std::nullptr_t> parseNull();
};

}