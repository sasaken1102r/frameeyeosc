// The Output tab's "Other apps": reading, checking and writing config.json's extra_outputs (see extras.h).
#include "extras.h"

#include "host_entry.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace extras {

namespace {

/** The keys an entry may have (frameeyeosc's EXTRA_KEYS): any other one is a newer version's. */
constexpr const char* kKeys[6] = {"name", "enabled", "format", "host", "port", "lids"};
/** A name is only shown: printable ASCII, at most this many characters (frameeyeosc's MAX_NAME_CHARS). */
constexpr size_t kMaxNameChars = 16;
constexpr const char* kListKey = "extra_outputs";

/**
 * @param key a key of an entry
 * @return whether this version knows it
 */
bool knownKey(const std::string& key) {
    return std::any_of(std::begin(kKeys), std::end(kKeys), [&key](const char* known) { return key == known; });
}

/**
 * @param format as written
 * @return whether it is one of the two formats
 */
bool knownFormat(const std::string& format) {
    return format == kFeo1 || format == kTrackingEye;
}

/**
 * Whether a host could be used as frameeyeosc reads it: "main", "auto", an IP address, or a host name (not empty, no
 * port, no white space). An IPv6 address has colons: only hex digits, colons, dots and a scope (%) then.
 * @param host as written
 * @return true if it could
 */
bool hostOk(const std::string& host) {
    if (host.empty()) return false;
    for (const char c : host) {
        if (std::isspace(static_cast<unsigned char>(c))) return false;
    }
    if (host.find(':') == std::string::npos) return true;
    const size_t scope = host.find('%');
    return host.substr(0, scope).find_first_not_of("0123456789abcdefABCDEF:.") == std::string::npos;
}

/**
 * A member of an object, to change it in place.
 * @param object the object
 * @param key the key
 * @return the member's value, or nullptr
 */
JsonValue* member(JsonValue& object, const std::string& key) {
    for (auto& [name, value] : object.members) {
        if (name == key) return &value;
    }
    return nullptr;
}

/**
 * Take a member out of an object (the others keep their order).
 * @param object the object
 * @param key the key
 */
void erase(JsonValue& object, const std::string& key) {
    auto& members = object.members;
    members.erase(std::remove_if(members.begin(), members.end(), [&key](const auto& m) { return m.first == key; }),
                  members.end());
}

/**
 * config.json's list, to change it: made when missing or null.
 * @param root config.json's object
 * @param make make it when missing (else nullptr then)
 * @param error why there is none to change
 * @return the list, or nullptr (not a list)
 */
JsonValue* listOf(JsonValue& root, bool make, std::string& error) {
    JsonValue* list = member(root, kListKey);
    if (list == nullptr || list->isNull()) {
        if (!make) {
            error = "extra_outputs has no entries";
            return nullptr;
        }
        JsonValue empty;
        empty.type = JsonValue::Type::Array;
        root.set(kListKey, empty);
        return member(root, kListKey);
    }
    if (!list->isArray()) {
        error = "extra_outputs in config.json is not a list";
        return nullptr;
    }
    return list;
}

/**
 * The entry at a place, checked against how it was when its flow opened.
 * @param list the list
 * @param index the place
 * @param original writeJson of it then ("" = don't check)
 * @param error why not
 * @return the entry, or nullptr
 */
JsonValue* entryAt(JsonValue& list, int index, const std::string& original, std::string& error) {
    if (index < 0 || index >= static_cast<int>(list.items.size())) {
        error = "the entry is gone from config.json";
        return nullptr;
    }
    JsonValue& item = list.items[static_cast<size_t>(index)];
    if (!original.empty() && writeJson(item) != original) {
        error = "the entry was changed in config.json meanwhile";
        return nullptr;
    }
    return &item;
}

/**
 * Read one entry of config.json as frameeyeosc does (config.rs, parse_extra), saying why it can't be used.
 * @param item the entry
 * @param index its place
 * @return it
 */
Entry readEntry(const JsonValue& item, int index) {
    Entry entry;
    entry.index = index;
    entry.name = defaultName(index);
    if (!item.isObject()) {
        entry.problem = Problem::Invalid;
        return entry;
    }
    bool nameBad = false;
    if (const JsonValue* name = item.get("name"); name != nullptr && name->isString() && !name->text.empty()) {
        const std::string& text = name->text;
        const bool printable = std::all_of(text.begin(), text.end(), [](char c) { return c >= 0x20 && c < 0x7f; });
        if (printable && text.size() <= kMaxNameChars) {
            entry.name = text;
        } else {
            nameBad = true;
        }
    } else if (name != nullptr && !name->isNull() && !name->isString()) {
        nameBad = true;
    }
    if (const JsonValue* v = item.get("enabled"); v != nullptr && v->isBool()) entry.enabled = v->boolean;
    if (const JsonValue* v = item.get("format"); v != nullptr && v->isString()) entry.format = v->text;
    if (const JsonValue* v = item.get("host"); v != nullptr && v->isString()) entry.host = v->text;
    if (const JsonValue* v = item.get("port");
        v != nullptr && v->isNumber() && v->number >= 1 && v->number <= 65535 && v->number == std::floor(v->number)) {
        entry.port = static_cast<int>(v->number);
    }
    if (const JsonValue* v = item.get("lids"); v != nullptr && v->isBool()) entry.lids = v->boolean;
    // In frameeyeosc's order: a key it doesn't know first (most likely a newer version's), then the values
    for (const auto& [key, value] : item.members) {
        if (!knownKey(key)) {
            entry.problem = Problem::Newer;
            return entry;
        }
    }
    const JsonValue* format = item.get("format");
    if (format == nullptr || !format->isString()) {
        entry.problem = Problem::Invalid;
    } else if (!knownFormat(format->text)) {
        entry.problem = Problem::Newer;
    } else if (format->text != kTrackingEye && item.get("lids") != nullptr) {
        entry.problem = Problem::Invalid;
    }
    if (entry.problem != Problem::None) return entry;
    for (const char* flag : {"enabled", "lids"}) {
        const JsonValue* v = item.get(flag);
        if (v != nullptr && !v->isBool()) entry.problem = Problem::Invalid;
    }
    const JsonValue* host = item.get("host");
    if (host == nullptr || !host->isString() || !hostOk(host->text)) entry.problem = Problem::Invalid;
    if (entry.port == 0 || nameBad) entry.problem = Problem::Invalid;
    return entry;
}

/**
 * Read one entry of the settings in effect (status.json): frameeyeosc's own reading, its "error" saying why it can't
 * be used.
 * @param item the entry
 * @param index its place
 * @return it
 */
Entry readEffectiveEntry(const JsonValue& item, int index) {
    Entry entry;
    entry.index = index;
    entry.name = defaultName(index);
    if (!item.isObject()) {
        entry.problem = Problem::Invalid;
        return entry;
    }
    if (const JsonValue* v = item.get("name"); v != nullptr && v->isString() && !v->text.empty()) {
        entry.name = validUtf8(v->text);
    }
    if (const JsonValue* v = item.get("enabled"); v != nullptr && v->isBool()) entry.enabled = v->boolean;
    if (const JsonValue* v = item.get("format"); v != nullptr && v->isString()) entry.format = v->text;
    if (const JsonValue* v = item.get("host"); v != nullptr && v->isString()) entry.host = validUtf8(v->text);
    if (const JsonValue* v = item.get("port"); v != nullptr && v->isNumber() && v->number >= 1 && v->number <= 65535) {
        entry.port = static_cast<int>(v->number);
    }
    if (const JsonValue* v = item.get("lids"); v != nullptr && v->isBool()) entry.lids = v->boolean;
    if (const JsonValue* v = item.get("error"); v != nullptr && v->isString() && !v->text.empty()) {
        entry.error = validUtf8(v->text);
        entry.problem = entry.error.find("newer") != std::string::npos ? Problem::Newer : Problem::Invalid;
    } else if (!knownFormat(entry.format)) {
        entry.problem = Problem::Invalid;
    }
    return entry;
}

}  // namespace

std::string defaultName(int index) {
    return "App " + std::to_string(index + 1);
}

List readList(const JsonValue* value, bool effective) {
    List list;
    if (value == nullptr || value->isNull()) return list;
    if (!value->isArray()) {
        list.isList = false;
        return list;
    }
    for (size_t i = 0; i < value->items.size(); ++i) {
        const int index = static_cast<int>(i);
        Entry entry = effective ? readEffectiveEntry(value->items[i], index) : readEntry(value->items[i], index);
        if (index >= kMax && entry.problem == Problem::None) entry.problem = Problem::Limit;
        list.entries.push_back(std::move(entry));
    }
    return list;
}

bool isIpv4(const std::string& text) {
    return host_entry::checkHost(text) == host_entry::HostError::None;
}

Net classify(const std::string& text) {
    if (!isIpv4(text)) return Net::Invalid;
    int octets[4] = {0, 0, 0, 0};
    size_t start = 0;
    for (int& octet : octets) {
        const size_t dot = text.find('.', start);
        octet = std::stoi(text.substr(start, dot == std::string::npos ? std::string::npos : dot - start));
        start = dot + 1;
    }
    const int a = octets[0];
    const int b = octets[1];
    if (text == "255.255.255.255") return Net::Broadcast;
    if (a >= 224 && a <= 239) return Net::Multicast;
    if (a == 0) return Net::Unspecified;
    if (a == 127 || a == 10 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168) || (a == 169 && b == 254)) {
        return Net::Lan;
    }
    if (a == 100 && b >= 64 && b <= 127) return Net::Vpn;
    return Net::Public;
}

int defaultPort(const std::string& format) {
    return format == kTrackingEye ? kPortTrackingEye : kPortFeo1;
}

Draft newDraft(int count) {
    Draft draft;
    draft.name = defaultName(count);
    return draft;
}

Draft draftOf(const Entry& entry, const std::string& original) {
    Draft draft;
    draft.edit = entry.index;
    draft.original = original;
    draft.name = entry.name;
    draft.format = knownFormat(entry.format) ? entry.format : std::string();
    draft.port = entry.port > 0 ? entry.port : defaultPort(draft.format);
    draft.lids = draft.format == kTrackingEye && entry.lids;
    const std::string& host = entry.host;
    if (host == kHostMain || host.empty()) {
        draft.host = HostChoice::Main;
    } else if (host == kHostAuto) {
        draft.host = HostChoice::Auto;
    } else if (host == kHostFrame) {
        draft.host = HostChoice::Frame;
    } else if (isIpv4(host)) {
        draft.host = HostChoice::Ip;
        draft.ip = host;
    } else {
        draft.host = HostChoice::Keep;
        draft.kept = host;
    }
    return draft;
}

void chooseFormat(Draft& draft, const std::string& format) {
    if (!knownFormat(format)) return;
    if (draft.format != format) draft.port = defaultPort(format);
    draft.format = format;
    if (format != kTrackingEye) draft.lids = false;
}

std::string draftHost(const Draft& draft) {
    switch (draft.host) {
        case HostChoice::Main: return kHostMain;
        case HostChoice::Auto: return kHostAuto;
        case HostChoice::Frame: return kHostFrame;
        case HostChoice::Ip: return draft.ip;
        case HostChoice::Keep: return draft.kept;
    }
    return kHostMain;
}

HostCheck checkHost(const Draft& draft, bool lanOnly) {
    if (draft.host == HostChoice::Keep) return draft.kept.empty() ? HostCheck::Empty : HostCheck::Ok;
    if (draft.host != HostChoice::Ip) return HostCheck::Ok;
    if (draft.ip.empty()) return HostCheck::Empty;
    switch (classify(draft.ip)) {
        case Net::Invalid: return HostCheck::Typing;
        case Net::Lan: return HostCheck::Lan;
        case Net::Vpn: return HostCheck::Vpn;
        case Net::Public: return lanOnly ? HostCheck::PublicBlocked : HostCheck::Public;
        case Net::Broadcast: return HostCheck::Broadcast;
        case Net::Multicast: return HostCheck::Multicast;
        case Net::Unspecified: return HostCheck::Unspecified;
    }
    return HostCheck::Typing;
}

bool hostUsable(HostCheck check) {
    return check == HostCheck::Ok || check == HostCheck::Lan || check == HostCheck::Vpn || check == HostCheck::Public;
}

std::string hostKey(const std::string& host, const Taken& taken) {
    if (host == kHostMain) return taken.mainHost;
    if (host == kHostAuto) return taken.steamLink.empty() ? std::string(kHostAuto) : taken.steamLink;
    std::string lower = host;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
    return lower;
}

Clash findClash(const Draft& draft, const List& list, const Taken& taken, int& other) {
    other = -1;
    const std::string key = hostKey(draftHost(draft), taken);
    if (key.empty()) return Clash::None;
    if (draft.port == taken.mainPort && key == taken.mainHost) return Clash::Main;
    if (!taken.pupilHost.empty() && draft.port == taken.pupilPort && key == taken.pupilHost) return Clash::Pupils;
    for (const Entry& entry : list.entries) {
        if (entry.index == draft.edit || !entry.enabled || entry.problem != Problem::None) continue;
        if (entry.port == draft.port && hostKey(entry.host, taken) == key) {
            other = entry.index;
            return Clash::Duplicate;
        }
    }
    return Clash::None;
}

bool applyDraft(JsonValue& root, const Draft& draft, std::string& error) {
    if (!knownFormat(draft.format) || draft.port < 1 || draft.port > 65535 || draftHost(draft).empty()) {
        error = "the destination isn't complete";
        return false;
    }
    JsonValue* list = listOf(root, draft.edit < 0, error);
    if (list == nullptr) return false;
    const bool lids = draft.format == kTrackingEye;
    if (draft.edit < 0) {
        if (static_cast<int>(list->items.size()) >= kMax) {
            error = "extra_outputs has 3 entries already";
            return false;
        }
        JsonValue entry;
        entry.type = JsonValue::Type::Object;
        entry.set("enabled", JsonValue::makeBool(true));
        entry.set("format", JsonValue::makeString(draft.format));
        entry.set("host", JsonValue::makeString(draftHost(draft)));
        entry.set("port", JsonValue::makeNumber(draft.port, true));
        if (lids) entry.set("lids", JsonValue::makeBool(draft.lids));
        list->items.push_back(std::move(entry));
        return true;
    }
    JsonValue* entry = entryAt(*list, draft.edit, draft.original, error);
    if (entry == nullptr) return false;
    // (an entry that wasn't an object at all has nothing to keep)
    if (!entry->isObject()) {
        *entry = JsonValue();
        entry->type = JsonValue::Type::Object;
    }
    entry->set("enabled", JsonValue::makeBool(true));
    entry->set("format", JsonValue::makeString(draft.format));
    entry->set("host", JsonValue::makeString(draftHost(draft)));
    entry->set("port", JsonValue::makeNumber(draft.port, true));
    if (lids) {
        entry->set("lids", JsonValue::makeBool(draft.lids));
    } else {
        erase(*entry, "lids");
    }
    return true;
}

bool removeEntry(JsonValue& root, int index, const std::string& original, std::string& error) {
    JsonValue* list = listOf(root, false, error);
    if (list == nullptr || entryAt(*list, index, original, error) == nullptr) return false;
    list->items.erase(list->items.begin() + index);
    return true;
}

bool setEnabled(JsonValue& root, int index, bool on, std::string& error) {
    JsonValue* list = listOf(root, false, error);
    if (list == nullptr) return false;
    JsonValue* entry = entryAt(*list, index, "", error);
    if (entry == nullptr) return false;
    if (!entry->isObject()) {
        error = "the entry is not an object";
        return false;
    }
    entry->set("enabled", JsonValue::makeBool(on));
    return true;
}

std::string rawEntry(const JsonValue& root, int index) {
    const JsonValue* list = root.get(kListKey);
    if (list == nullptr || !list->isArray() || index < 0 || index >= static_cast<int>(list->items.size())) return "";
    return writeJson(list->items[static_cast<size_t>(index)]);
}

}  // namespace extras
