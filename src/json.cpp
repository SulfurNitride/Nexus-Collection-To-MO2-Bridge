#include "json.h"
#include <iostream>
#include <charconv>

namespace TinyJson {

std::optional<Value> Parser::parse(const std::string& json) {
    Parser p(json);
    p.skipWhitespace();
    return p.parseValue();
}

void Parser::skipWhitespace() {
    while (pos < str.size() && (str[pos] == ' ' || str[pos] == '\t' || str[pos] == '\n' || str[pos] == '\r')) {
        pos++;
    }
}

std::optional<Value> Parser::parseValue() {
    skipWhitespace();
    if (pos >= str.size()) return std::nullopt;

    char c = str[pos];
    if (c == '{') return parseObject();
    if (c == '[') return parseArray();
    if (c == '"') {
        auto s = parseString();
        if (s) return Value(*s);
    }
    if (c == 't' || c == 'f') {
        auto b = parseBool();
        if (b) return Value(*b);
    }
    if (c == 'n') {
        if (parseNull()) return Value();
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        auto n = parseNumber();
        if (n) return Value(*n);
    }
    
    return std::nullopt;
}

std::optional<Value> Parser::parseObject() {
    Object obj;
    pos++; // skip '{'
    skipWhitespace();
    
    if (pos < str.size() && str[pos] == '}') {
        pos++;
        return Value(obj);
    }

    while (pos < str.size()) {
        skipWhitespace();
        if (str[pos] != '"') return std::nullopt;
        auto key = parseString();
        if (!key) return std::nullopt;
        
        skipWhitespace();
        if (pos >= str.size() || str[pos] != ':') return std::nullopt;
        pos++; // skip ':'
        
        auto val = parseValue();
        if (!val) return std::nullopt;
        
        obj[*key] = *val;
        
        skipWhitespace();
        if (pos < str.size() && str[pos] == '}') {
            pos++;
            return Value(obj);
        }
        if (pos < str.size() && str[pos] == ',') {
            pos++;
        } else {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

std::optional<Value> Parser::parseArray() {
    Array arr;
    pos++; // skip '['
    skipWhitespace();
    
    if (pos < str.size() && str[pos] == ']') {
        pos++;
        return Value(arr);
    }

    while (pos < str.size()) {
        auto val = parseValue();
        if (!val) return std::nullopt;
        arr.push_back(*val);
        
        skipWhitespace();
        if (pos < str.size() && str[pos] == ']') {
            pos++;
            return Value(arr);
        }
        if (pos < str.size() && str[pos] == ',') {
            pos++;
        } else {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

std::optional<std::string> Parser::parseString() {
    if (str[pos] != '"') return std::nullopt;
    pos++;
    std::string res;
    while (pos < str.size()) {
        char c = str[pos];
        if (c == '"') {
            pos++;
            return res;
        }
        if (c == '\\') {
            pos++;
            if (pos >= str.size()) return std::nullopt;
            char esc = str[pos];
            if (esc == '"') res += '"';
            else if (esc == '\\') res += '\\';
            else if (esc == '/') res += '/';
            else if (esc == '\b') res += '\b';
            else if (esc == '\f') res += '\f';
            else if (esc == '\n') res += '\n';
            else if (esc == '\r') res += '\r';
            else if (esc == '\t') res += '\t';
            else if (esc == 'u') {
                // Handle \uXXXX
                if (pos + 4 < str.size()) {
                    std::string hex = str.substr(pos + 1, 4);
                    pos += 4;
                    try {
                        int code = std::stoi(hex, nullptr, 16);
                        if (code < 128) {
                            res += static_cast<char>(code);
                        } else {
                            // Basic placeholder for non-ASCII
                            res += '?'; 
                        }
                    } catch (...) {}
                }
            }
        } else {
            res += c;
        }
        pos++;
    }
    return std::nullopt;
}

std::optional<double> Parser::parseNumber() {
    size_t start = pos;
    if (pos < str.size() && str[pos] == '-') pos++;
    while (pos < str.size() && (str[pos] >= '0' && str[pos] <= '9')) pos++;
    if (pos < str.size() && str[pos] == '.') {
        pos++;
        while (pos < str.size() && (str[pos] >= '0' && str[pos] <= '9')) pos++;
    }
    // exponent...
    if (pos < str.size() && (str[pos] == 'e' || str[pos] == 'E')) {
        pos++;
        if (pos < str.size() && (str[pos] == '+' || str[pos] == '-')) pos++;
        while (pos < str.size() && (str[pos] >= '0' && str[pos] <= '9')) pos++;
    }
    
    std::string numStr = str.substr(start, pos - start);
    try {
        return std::stod(numStr);
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<bool> Parser::parseBool() {
    if (str.substr(pos, 4) == "true") {
        pos += 4;
        return true;
    }
    if (str.substr(pos, 5) == "false") {
        pos += 5;
        return false;
    }
    return std::nullopt;
}

std::optional<std::nullptr_t> Parser::parseNull() {
    if (str.substr(pos, 4) == "null") {
        pos += 4;
        return nullptr;
    }
    return std::nullopt;
}

}
