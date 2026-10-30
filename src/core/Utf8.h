// Minimal UTF-8 -> UTF-16 conversion (device names from WASAPI are UTF-8 internally,
// VST3 UI strings are UTF-16). Invalid bytes become U+FFFD. Header-only, no platform deps.
#pragma once
#include <cstddef>
#include <string>

namespace asb {

constexpr char16_t kReplacementChar = char16_t(0xFFFD);

inline std::u16string utf8ToUtf16(const std::string& in)
{
    std::u16string out;
    out.reserve(in.size());
    const auto* s = reinterpret_cast<const unsigned char*>(in.data());
    const size_t n = in.size();
    for (size_t i = 0; i < n;) {
        unsigned char c = s[i];
        char32_t cp; size_t len;
        if (c < 0x80)              { cp = c; len = 1; }
        else if ((c >> 5) == 0x6)  { cp = c & 0x1F; len = 2; }
        else if ((c >> 4) == 0xE)  { cp = c & 0x0F; len = 3; }
        else if ((c >> 3) == 0x1E) { cp = c & 0x07; len = 4; }
        else { out.push_back(u'�'); ++i; continue; }
        if (i + len > n) { out.push_back(u'�'); break; }
        bool ok = true;
        for (size_t k = 1; k < len; ++k) {
            if ((s[i + k] >> 6) != 0x2) { ok = false; break; }
            cp = (cp << 6) | (s[i + k] & 0x3F);
        }
        const bool overlong = (len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000);
        if (!ok || overlong || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) { out.push_back(u'�'); ++i; continue; }
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back(char16_t(0xD800 + (cp >> 10)));
            out.push_back(char16_t(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back(char16_t(cp));
        }
        i += len;
    }
    return out;
}

} // namespace asb
