// Typing on the panel's keypad: the target PC's IPv4 address (the Basic tab's "Enter IP") and a port (the main
// output's port row, "Other apps"' step 3). The keypad's editing and checking what was typed. A host name can only be
// set in config.json. Nothing here draws or talks to OpenVR, so it is tested on its own (text_test.cpp).
#pragma once

#include <string>

namespace host_entry {

/** What is wrong with a typed address. */
enum class HostError {
    None,   ///< an IPv4 address
    Empty,  ///< nothing typed
    Ipv4,   ///< not four numbers 0-255 separated by dots
};

/** What is wrong with a typed port. */
enum class PortError {
    None,   ///< a port 1-65535
    Empty,  ///< nothing typed
    Range,  ///< 0, or above 65535
};

/** Which keypad: the address's has '.', the port's only digits. */
enum class Keypad { Host, Port };

/** The keypad's backspace key (the others are '0'-'9' and, on the address's, '.'). */
constexpr int kBackspace = 8;
/** The address's keypad takes at most this many characters ("255.255.255.255"). */
constexpr size_t kKeypadMax = 15;
/** The port's keypad takes at most this many digits ("65535"). */
constexpr size_t kPortKeypadMax = 5;

/**
 * Check a typed address.
 * @param text what was typed
 * @return what is wrong, or HostError::None
 * @example
 * checkHost("192.168.1.20") // HostError::None
 * checkHost("192.168.1") // HostError::Ipv4
 */
HostError checkHost(const std::string& text);

/**
 * Check a typed port.
 * @param text what was typed (digits only, as the keypad types)
 * @param port set to the port when it is one
 * @return what is wrong, or PortError::None
 * @example
 * checkPort("11112", port) // PortError::None, port 11112
 * checkPort("70000", port) // PortError::Range
 */
PortError checkPort(const std::string& text, int& port);

/**
 * One keypad press.
 * @param text the text so far
 * @param key '0'-'9', '.' (the address's only) or kBackspace
 * @param keypad which keypad
 * @return the new text (unchanged if the key does not fit; on the port's, a digit replaces a lone "0")
 */
std::string keypadInput(const std::string& text, int key, Keypad keypad = Keypad::Host);

}  // namespace host_entry
