// Port settings: protore.ini in the working directory (on Android: the app's external files directory, next to
// game/), one `key = value` per line, `#` or `;` starts a comment. An environment variable PROTORE_<KEY> (upper
// case) overrides the file.
#include <cctype>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>

#include "guest.h"

namespace native {

namespace {

std::map<std::string, std::string> Load() {
    std::map<std::string, std::string> m;
    FILE* f = fopen("protore.ini", "r");
    if (!f) return m;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        std::string s(line);
        size_t cut = s.find_first_of("#;\r\n");
        if (cut != std::string::npos) s.resize(cut);
        size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        auto trim = [](std::string v) {
            while (!v.empty() && isspace((unsigned char)v.back())) v.pop_back();
            size_t i = 0;
            while (i < v.size() && isspace((unsigned char)v[i])) ++i;
            return v.substr(i);
        };
        std::string key = trim(s.substr(0, eq));
        for (char& ch : key) ch = char(tolower((unsigned char)ch));
        m[key] = trim(s.substr(eq + 1));
    }
    fclose(f);
    return m;
}

}  // namespace

std::string Setting(const char* key, const char* def) {
    std::string env = "PROTORE_";
    for (const char* k = key; *k; ++k) env += char(toupper((unsigned char)*k));
    if (const char* v = getenv(env.c_str())) return v;
    static std::once_flag once;
    static std::map<std::string, std::string> file;
    std::call_once(once, [] { file = Load(); });
    auto it = file.find(key);
    return it != file.end() ? it->second : def;
}

int SettingInt(const char* key, int def) {
    std::string v = Setting(key, "");
    return v.empty() ? def : atoi(v.c_str());
}

}  // namespace native
