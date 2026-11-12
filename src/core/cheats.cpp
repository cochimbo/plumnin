#include "cheats.h"

#include "i18n.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>

namespace plumnin {

namespace {

std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    size_t e = s.find_last_not_of(" \t\r\n");
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

}  // namespace

std::string normalizeCheat(const std::string& code) {
    std::string s;
    for (char c : code)
        if (!std::isspace(static_cast<unsigned char>(c))) s += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

bool decodeCheatCodes(const std::string& code, const CheatDecoder& decode, std::vector<CheatPatch>* out,
                      std::string* error) {
    out->clear();
    const std::string s = normalizeCheat(code);
    size_t start = 0;
    while (start <= s.size()) {
        size_t plus = s.find('+', start);
        if (plus == std::string::npos) plus = s.size();
        if (plus > start) {
            CheatPatch p;
            if (!decode || !decode(s.substr(start, plus - start), &p, error)) {
                if (!decode && error) *error = tr("Esta consola todavía no tiene trucos.");
                out->clear();
                return false;
            }
            out->push_back(p);
        }
        start = plus + 1;
    }
    if (out->empty()) {
        if (error) *error = tr("Escribe un código.");
        return false;
    }
    return true;
}

std::string formatCheats(const std::vector<Cheat>& cheats) {
    std::ostringstream out;
    out << "# Trucos de plumnin para este juego. Una línea por código: si o no (activado),\n"
           "# el código (Game Genie o Pro Action Replay) y un nombre.\n";
    for (const Cheat& c : cheats) out << (c.enabled ? "si " : "no ") << c.code << (c.name.empty() ? "" : " ") << c.name << "\n";
    return out.str();
}

std::vector<Cheat> parseCheats(const std::string& text) {
    std::vector<Cheat> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        std::istringstream words(line);
        std::string first, code;
        words >> first;
        Cheat c;
        if (first == "si" || first == "sí" || first == "no") {
            c.enabled = first != "no";
            words >> code;
        } else {
            code = first;
        }
        if (code.empty()) continue;
        c.code = normalizeCheat(code);
        std::string rest;
        std::getline(words, rest);
        c.name = trim(rest);
        out.push_back(std::move(c));
    }
    return out;
}

std::vector<Cheat> parseChtFile(const std::string& text) {
    // cheat0_desc = "Infinite lives"
    // cheat0_code = "HADT-BEW6+AJ0T-CA9Y"
    std::vector<std::pair<int, Cheat>> found;
    auto entry = [&](int n) -> Cheat& {
        for (auto& f : found)
            if (f.first == n) return f.second;
        found.push_back({n, Cheat{}});
        found.back().second.enabled = false;
        return found.back().second;
    };
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.compare(0, 5, "cheat") != 0) continue;
        size_t i = 5;
        int n = 0;
        while (i < line.size() && std::isdigit(static_cast<unsigned char>(line[i]))) n = n * 10 + (line[i++] - '0');
        if (i == 5 || i >= line.size() || line[i] != '_') continue;
        const size_t equals = line.find('=', i);
        if (equals == std::string::npos) continue;
        const std::string key = trim(line.substr(i + 1, equals - i - 1));
        if (key != "desc" && key != "code") continue;
        std::string value = trim(line.substr(equals + 1));
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
        if (key == "desc") entry(n).name = trim(value);
        else entry(n).code = normalizeCheat(value);
    }
    std::stable_sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<Cheat> out;
    for (auto& f : found)
        if (!f.second.code.empty()) out.push_back(std::move(f.second));
    return out;
}

}  // namespace plumnin
