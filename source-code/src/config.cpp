#include "config.h"
#include <fstream>
#include <iostream>
#include <algorithm>

Config Config::load(const std::string& path) {
    Config config;
    std::ifstream file(path);
    if (!file.is_open()) return config;

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);

        // Trim
        auto trim = [](std::string& s) {
            s.erase(0, s.find_first_not_of(" \t"));
            s.erase(s.find_last_not_of(" \t") + 1);
        };
        trim(key); trim(val);

        if (key == "gap_size") config.gap_size = std::stoi(val);
        else if (key == "padding") config.padding = std::stoi(val);
        else if (key == "bar_height") config.bar_height = std::stoi(val);
        else if (key == "border_width") config.border_width = std::stoi(val);
        else if (key == "spring_tension") config.spring_tension = std::stod(val);
        else if (key == "spring_friction") config.spring_friction = std::stod(val);
        else if (key == "inactive_opacity") config.inactive_opacity = std::stod(val);
        else if (key == "bar_position") config.bar_position = val;
        else if (key.rfind("bindsym ", 0) == 0) {
            std::string action = key.substr(8);
            trim(action);
            config.keybinds[action] = val;
        }
    }
    return config;
}
