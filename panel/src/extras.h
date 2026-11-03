// The Output tab's "Other apps": up to three more destinations for the eye data next to the main one
// (config.json's extra_outputs, read by frameeyeosc 0.10 on), and lan_only. What the panel reads from the list, the
// add / change flow's draft and what it checks (the address kind, the same destination twice), and writing the list
// back. config.json's list is changed in place: an entry the panel doesn't touch is written back exactly as it was,
// and the keys it doesn't know (a newer frameeyeosc's) are kept in the one it changes. Nothing here draws, so it is
// tested on its own (model_test.cpp).
#pragma once

#include "json.h"

#include <string>
#include <vector>

namespace extras {

/** At most this many entries are used (frameeyeosc's MAX_EXTRAS); the panel offers "Add" below it. */
constexpr int kMax = 3;
/** The two formats (permanent names). */
constexpr const char* kFeo1 = "feo1";
constexpr const char* kTrackingEye = "tracking_eye";
/** The port each format starts at (next to the FrameEyeOSC module's 11112, and the one gaze tools tend to use). */
constexpr int kPortFeo1 = 11113;
constexpr int kPortTrackingEye = 9015;
/** "host": follow the main output's PC, the Steam Link PC, or this Frame. */
constexpr const char* kHostMain = "main";
constexpr const char* kHostAuto = "auto";
constexpr const char* kHostFrame = "127.0.0.1";

/** Why the panel itself can't use an entry as written (frameeyeosc's own reason comes in status.json). */
enum class Problem {
    None,
    Newer,    ///< a key or format this version doesn't know (a newer frameeyeosc wrote it)
    Invalid,  ///< a value of the wrong kind or out of range
    Limit,    ///< after the first kMax
};

/** One extra_outputs entry as config.json (or the settings in effect) has it. */
struct Entry {
    int index = 0;             ///< its place in the list, from 0
    std::string name;          ///< as written, or "App N" (N = index + 1) without one
    bool enabled = true;
    std::string format;        ///< as written ("" when missing or not a string)
    std::string host;          ///< as written
    int port = 0;              ///< 0 when missing or not 1..65535
    bool lids = false;
    Problem problem = Problem::None;
    std::string error;         ///< the settings in effect's "error" (frameeyeosc's words), if read from them
};

/** The list as read. */
struct List {
    bool isList = true;          ///< missing, null or an array (anything else: no entries, and the panel can't add)
    std::vector<Entry> entries;  ///< every entry, the unusable ones too
};

/**
 * Read extra_outputs.
 * @param value the list (config.json's, or status.json's effective one while it is locked); nullptr = missing
 * @param effective from status.json's effective settings (each entry has frameeyeosc's "error" there)
 * @return the entries
 */
List readList(const JsonValue* value, bool effective = false);

/**
 * The name shown for the entry at a place without its own.
 * @param index from 0
 * @return "App 1" for 0
 */
std::string defaultName(int index);

/** What kind of address an IPv4 address is, as frameeyeosc's address check (src/dest.rs) decides it. */
enum class Net {
    Invalid,      ///< not (yet) four numbers 0-255 (host_entry::checkHost)
    Lan,          ///< 127/8, 10/8, 172.16/12, 192.168/16, 169.254/16
    Vpn,          ///< 100.64/10: allowed, marked VPN
    Public,       ///< anything else: refused while lan_only is on, marked PUBLIC while it is off
    Broadcast,    ///< 255.255.255.255: never
    Multicast,    ///< 224/4: never
    Unspecified,  ///< 0/8: never
};

/**
 * @param text an IPv4 address as typed
 * @return its kind
 * @example
 * classify("192.168.1.20") // Net::Lan
 * classify("100.101.5.7")  // Net::Vpn
 */
Net classify(const std::string& text);

/**
 * @param text a host as written ("main", "auto", an IP address or a host name)
 * @return whether it is an IPv4 address the keypad could have typed
 */
bool isIpv4(const std::string& text);

/** Which host the add / change flow's second step has chosen. */
enum class HostChoice {
    Main,   ///< "main": the main output's PC (Steam Link's while that is automatic)
    Auto,   ///< "auto": the Steam Link PC
    Frame,  ///< "127.0.0.1": an app on the Frame
    Ip,     ///< an IPv4 address typed on the keypad
    Keep,   ///< as config.json has it (a host name or an IPv6 address, which the keypad can't type)
};

/** The add / change flow's choices so far. */
struct Draft {
    int edit = -1;            ///< the entry being changed (its index), -1 while adding
    std::string original;     ///< ...that entry as it was when the flow opened (writeJson), to refuse a stale save
    std::string name;         ///< shown in the summary ("App N" for a new one)
    std::string format;       ///< kFeo1 / kTrackingEye; "" until chosen
    HostChoice host = HostChoice::Main;
    std::string ip;           ///< typed (HostChoice::Ip)
    std::string kept;         ///< the host as written (HostChoice::Keep)
    int port = kPortFeo1;
    bool lids = false;        ///< tracking_eye only
};

/**
 * A new draft for "Add".
 * @param count how many entries the list has now
 * @return the draft (no format chosen yet, the main output's PC, feo1's port)
 */
Draft newDraft(int count);

/**
 * A draft for "Change" on an entry.
 * @param entry the entry
 * @param original the entry as config.json has it (rawEntry; kept to notice another change before saving)
 * @return the draft (an unknown format: none chosen)
 */
Draft draftOf(const Entry& entry, const std::string& original);

/**
 * Choose a format: a different one starts from its own port (as the mock: the same one keeps the port).
 * @param draft the draft
 * @param format kFeo1 or kTrackingEye
 */
void chooseFormat(Draft& draft, const std::string& format);

/**
 * The port a format starts at.
 * @param format kFeo1 or kTrackingEye
 * @return its port
 */
int defaultPort(const std::string& format);

/**
 * @param draft the draft
 * @return the "host" it writes
 */
std::string draftHost(const Draft& draft);

/** What the host step says about the chosen host, and whether "Next" can be pressed. */
enum class HostCheck {
    Ok,           ///< main, auto, the Frame or a kept host (frameeyeosc checks its address once it looks it up)
    Empty,        ///< nothing typed yet
    Typing,       ///< not an address yet
    Lan,
    Vpn,
    Public,       ///< allowed (lan_only off), marked PUBLIC
    PublicBlocked,  ///< refused while lan_only is on
    Broadcast,
    Multicast,
    Unspecified,
};

/**
 * Check the draft's host.
 * @param draft the draft
 * @param lanOnly lan_only as shown
 * @return what the step says
 */
HostCheck checkHost(const Draft& draft, bool lanOnly);

/**
 * @param check what checkHost said
 * @return whether "Next" can be pressed
 */
bool hostUsable(HostCheck check);

/** The other destinations an extra must not repeat, as far as the panel can tell. */
struct Taken {
    std::string mainHost;   ///< the main output's address ("auto" while the Steam Link PC isn't known)
    int mainPort = 0;
    std::string steamLink;  ///< the Steam Link PC's address ("" = not known)
    std::string pupilHost;  ///< where the pupils go straight to VRChat ("" = nowhere)
    int pupilPort = 0;
};

/** A destination the draft repeats. */
enum class Clash { None, Main, Pupils, Duplicate };

/**
 * The address a host is compared by: "main" the main output's, "auto" the Steam Link PC's ("auto" while unknown), else
 * itself (lower case).
 * @param host as written
 * @param taken the main output and the Steam Link PC
 * @return the address to compare
 */
std::string hostKey(const std::string& host, const Taken& taken);

/**
 * Whether the draft repeats the main output's destination, the pupils' or another turned-on entry's (frameeyeosc
 * refuses such an extra: "same as main", "duplicate").
 * @param draft the draft
 * @param list the list now (the entry being changed is left out)
 * @param taken the other destinations
 * @param other where to write the entry it repeats (Clash::Duplicate)
 * @return what it repeats
 */
Clash findClash(const Draft& draft, const List& list, const Taken& taken, int& other);

/**
 * Write the draft into config.json's root: a new entry at the end (enabled, its format, host, port, and lids for
 * tracking_eye; no name, so it is "App N"), or the entry being changed (those keys set, lids taken out for feo1,
 * turned on; every other key, a newer frameeyeosc's too, kept where it is). The other entries are left exactly as they
 * are.
 * @param root config.json's object
 * @param draft the draft
 * @param error why it can't (not a list, no room, the entry changed or gone meanwhile)
 * @return true if written into root
 */
bool applyDraft(JsonValue& root, const Draft& draft, std::string& error);

/**
 * Take an entry out of the list (the others left exactly as they are).
 * @param root config.json's object
 * @param index the entry
 * @param original the entry as it was when its flow opened ("" = don't check)
 * @param error why it can't
 * @return true if taken out
 */
bool removeEntry(JsonValue& root, int index, const std::string& original, std::string& error);

/**
 * Turn an entry on or off ("enabled"; everything else left as it is).
 * @param root config.json's object
 * @param index the entry
 * @param on on
 * @param error why it can't
 * @return true if written into root
 */
bool setEnabled(JsonValue& root, int index, bool on, std::string& error);

/**
 * The entry as config.json has it, written out (Draft::original).
 * @param root config.json's object
 * @param index the entry
 * @return its JSON, or "" when there is no such entry
 */
std::string rawEntry(const JsonValue& root, int index);

}  // namespace extras
