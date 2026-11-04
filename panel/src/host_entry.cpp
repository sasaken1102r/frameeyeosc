// Typing the target PC's IPv4 address or a port on the panel's keypad.
#include "host_entry.h"

namespace host_entry {

HostError checkHost(const std::string& text) {
    if (text.empty()) return HostError::Empty;
    // Four numbers 0-255 of 1-3 digits, separated by dots
    int parts = 0;
    size_t start = 0;
    while (true) {
        const size_t dot = text.find('.', start);
        const std::string part = text.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        // A leading zero is refused: the address lookup reads "010" as octal (8), so it would send elsewhere
        if (part.empty() || part.size() > 3 || part.find_first_not_of("0123456789") != std::string::npos ||
            (part.size() > 1 && part[0] == '0') || std::stoi(part) > 255) {
            return HostError::Ipv4;
        }
        ++parts;
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    return parts == 4 ? HostError::None : HostError::Ipv4;
}

PortError checkPort(const std::string& text, int& port) {
    if (text.empty()) return PortError::Empty;
    // (more digits than a port has, or anything else, can't be one)
    if (text.size() > kPortKeypadMax || text.find_first_not_of("0123456789") != std::string::npos) {
        return PortError::Range;
    }
    const int value = std::stoi(text);
    if (value < 1 || value > 65535) return PortError::Range;
    port = value;
    return PortError::None;
}

std::string keypadInput(const std::string& text, int key, Keypad keypad) {
    if (key == kBackspace) return text.empty() ? text : text.substr(0, text.size() - 1);
    const bool digit = key >= '0' && key <= '9';
    if (keypad == Keypad::Port) {
        if (!digit) return text;
        // (no leading zero: "0" then "8" is 8)
        if (text == "0") return std::string(1, static_cast<char>(key));
        return text.size() >= kPortKeypadMax ? text : text + static_cast<char>(key);
    }
    if ((!digit && key != '.') || text.size() >= kKeypadMax) return text;
    return text + static_cast<char>(key);
}

}  // namespace host_entry
