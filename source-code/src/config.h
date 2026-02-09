#ifndef CONFIG_H
#define CONFIG_H

#include <string>
#include <map>
#include <vector>

struct Config {
    // Layout
    int gap_size = 10;
    int padding = 20;
    int bar_height = 42;
    int border_width = 2;

    // Physics
    double spring_tension = 180.0;
    double spring_friction = 14.0;

    // Colors & Visuals
    std::string active_border_color = "#cba6f7";
    std::string inactive_border_color = "#585b70";
    double inactive_opacity = 0.8;

    // Behavior
    std::string bar_position = "top";
    bool enable_bar = true;

    // Keybinds
    std::map<std::string, std::string> keybinds;

    static Config load(const std::string& path);
};

#endif // CONFIG_H
