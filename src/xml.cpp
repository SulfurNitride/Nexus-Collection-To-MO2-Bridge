#include "xml.h"
#include <iostream>

namespace TinyXML {

std::shared_ptr<Element> Parser::parse(const std::string& xml) {
    Parser p(xml);
    return p.parseElement();
}

void Parser::skipWhitespace() {
    while (pos < str.size() && isspace(str[pos])) pos++;
}

std::shared_ptr<Element> Parser::parseElement() {
    while (true) {
        skipWhitespace();
        if (pos >= str.size()) return nullptr;

        // Skip Comments <!-- ... -->
        if (str.substr(pos, 4) == "<!--") {
            pos = str.find("-->", pos);
            if (pos == std::string::npos) return nullptr;
            pos += 3;
            continue;
        }

        // Skip XML Declaration <?xml ... ?>
        if (str.substr(pos, 2) == "<?") {
            pos = str.find("?>", pos);
            if (pos == std::string::npos) return nullptr;
            pos += 2;
            continue;
        }

        // Skip CDATA (treat as text? complicated. just skip or fail? 
        // FOMOD usually doesn't have CDATA at element level, only content. 
        // If we are here, we expect <TagName.
        
        if (str[pos] != '<') return nullptr;
        break; 
    }

    pos++; // skip <

    // Read Tag Name
    size_t start = pos;
    while (pos < str.size() && !isspace(str[pos]) && str[pos] != '>' && str[pos] != '/') pos++;
    std::string tagName = str.substr(start, pos - start);

    auto el = std::make_shared<Element>();
    el->name = tagName;

    // Attributes
    el->attributes = parseAttributes();

    skipWhitespace();
    
    // Self-closing? <tag />
    if (pos < str.size() && str[pos] == '/') {
        pos++; // skip /
        if (pos < str.size() && str[pos] == '>') pos++;
        return el;
    }

    if (pos < str.size() && str[pos] == '>') pos++; // skip >

    // Children or Text
    while (pos < str.size()) {
        skipWhitespace();
        
        // Check for closing tag </tag>
        if (pos + 1 < str.size() && str[pos] == '<' && str[pos+1] == '/') {
            pos += 2;
            while (pos < str.size() && str[pos] != '>') pos++;
            pos++;
            return el;
        } 
        
        // Check for comment inside content (Need to skip it and continue)
        if (str.substr(pos, 4) == "<!--") {
            pos = str.find("-->", pos);
            if (pos == std::string::npos) break;
            pos += 3;
            continue;
        }

        if (pos < str.size() && str[pos] == '<') {
            // Child
            auto child = parseElement();
            if (child) el->children.push_back(child);
        } else {
            // Text
            size_t tStart = pos;
            while (pos < str.size() && str[pos] != '<') pos++;
            el->text = str.substr(tStart, pos - tStart);
        }
    }
    return el;
}

std::map<std::string, std::string> Parser::parseAttributes() {
    std::map<std::string, std::string> attrs;
    while (pos < str.size()) {
        skipWhitespace();
        if (str[pos] == '>' || str[pos] == '/') break;

        size_t start = pos;
        while (pos < str.size() && !isspace(str[pos]) && str[pos] != '=') pos++;
        std::string key = str.substr(start, pos - start);

        skipWhitespace();
        if (pos < str.size() && str[pos] == '=') {
            pos++; // skip =
            skipWhitespace();
            if (pos < str.size() && (str[pos] == '"' || str[pos] == '\'')) {
                char quote = str[pos];
                pos++;
                size_t vStart = pos;
                while (pos < str.size() && str[pos] != quote) pos++;
                std::string val = str.substr(vStart, pos - vStart);
                pos++; // skip quote
                attrs[key] = val;
            }
        }
    }
    return attrs;
}

} // namespace TinyXML