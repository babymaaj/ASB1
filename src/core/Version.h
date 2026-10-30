// "v0.5.1-alpha" -> {0,5,1}. Used by the update check (GitHub release tag vs. this build).
#pragma once
#include <array>
#include <cctype>
#include <string>

namespace asb {

inline std::array<int, 3> parseVersion(const std::string& s)
{
    std::array<int, 3> v{0, 0, 0};
    size_t i = 0, part = 0;
    while (i < s.size() && !std::isdigit(static_cast<unsigned char>(s[i]))) ++i;    // skip "v"
    for (; i < s.size() && part < 3; ++i) {
        const char c = s[i];
        if (std::isdigit(static_cast<unsigned char>(c))) v[part] = v[part] * 10 + (c - '0');
        else if (c == '.') ++part;
        else break;                                                                  // "-alpha" etc.
    }
    return v;
}

inline bool isNewerVersion(const std::string& candidate, const std::string& current)
{
    return parseVersion(candidate) > parseVersion(current);
}

// Minimal JSON string field lookup ("key": "value") — enough for GitHub's release JSON, no dependency.
inline std::string jsonStringField(const std::string& json, const std::string& key)
{
    const std::string k = "\"" + key + "\"";
    size_t p = json.find(k);
    if (p == std::string::npos) return {};
    p = json.find(':', p + k.size());
    if (p == std::string::npos) return {};
    p = json.find('"', p + 1);
    if (p == std::string::npos) return {};
    std::string out;
    for (size_t i = p + 1; i < json.size(); ++i) {
        if (json[i] == '\\' && i + 1 < json.size()) { out += json[++i]; continue; }
        if (json[i] == '"') return out;
        out += json[i];
    }
    return {};
}

} // namespace asb
