#pragma once

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <optional>

namespace TinyXML {

struct Element {
    std::string name;
    std::map<std::string, std::string> attributes;
    std::string text;
    std::vector<std::shared_ptr<Element>> children;

    std::string getAttribute(const std::string& key) const {
        auto it = attributes.find(key);
        return (it != attributes.end()) ? it->second : "";
    }

    std::shared_ptr<Element> findChild(const std::string& name) const {
        for (const auto& child : children) {
            if (child->name == name) return child;
        }
        return nullptr;
    }
    
    std::vector<std::shared_ptr<Element>> findChildren(const std::string& name) const {
        std::vector<std::shared_ptr<Element>> res;
        for (const auto& child : children) {
            if (child->name == name) res.push_back(child);
        }
        return res;
    }
};

class Parser {
public:
    static std::shared_ptr<Element> parse(const std::string& xml);
private:
    const std::string& str;
    size_t pos = 0;
    
    Parser(const std::string& s) : str(s) {}
    void skipWhitespace();
    std::shared_ptr<Element> parseElement();
    std::map<std::string, std::string> parseAttributes();
};

}
