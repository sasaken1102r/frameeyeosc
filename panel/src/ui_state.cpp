// The panel's own state between starts (see ui_state.h).
#include "ui_state.h"

#include "json.h"
#include "file_io.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <utility>
#include <vector>

namespace ui_state {

namespace {

/** The members the sub-tabs are kept in. */
constexpr const char* kPageKey = "advanced_page";
constexpr const char* kLidsPageKey = "lids_page";
constexpr const char* kGazePageKey = "gaze_page";
/** Whether the gaze scale's left / right and up / down are linked (true / false). */
constexpr const char* kGazeScaleLinkKeys[2] = {"gaze_scale_link_x", "gaze_scale_link_y"};
/** The up gains the last whole eye fits measured, oldest first (an array of numbers). */
constexpr const char* kUpGainsKey = "fit_up_gains";
/** "Get test versions" on the Version page (true / false; missing: off). */
constexpr const char* kUpdateBetaKey = "update_beta";

/**
 * Make a folder and the ones above it.
 * @param path the folder
 * @return true if it is there now
 */
bool makeDirs(const std::string& path) {
    struct stat st {};
    if (::stat(path.c_str(), &st) == 0) return S_ISDIR(st.st_mode);
    const size_t slash = path.find_last_of('/');
    if (slash != std::string::npos && slash > 0 && !makeDirs(path.substr(0, slash))) return false;
    return ::mkdir(path.c_str(), 0700) == 0 || errno == EEXIST;
}

/**
 * The file as an object (empty when missing or broken).
 * @param path the file
 * @param broken if not null: set to whether the file is there but not such an object (too big, not JSON, not an
 *               object)
 * @return the object
 */
JsonValue readObject(const std::string& path, bool* broken = nullptr) {
    JsonValue root;
    root.type = JsonValue::Type::Object;
    if (broken != nullptr) *broken = false;
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) return root;
    std::string text;
    const bool read = file_io::read(file, text, 64 * 1024);
    std::fclose(file);
    JsonValue parsed;
    std::string error;
    if (read && parseJson(text, parsed, error) && parsed.isObject()) return parsed;
    if (broken != nullptr) *broken = true;
    return root;
}

/**
 * Set members of the file (the others are kept; written through a temporary file and rename). A file that is there
 * but can't be read is first moved aside to `<path>.broken` (replacing an older one) and said so on stderr, since
 * the new file has only these members.
 * @param path the file (its folder is made if missing)
 * @param members the members and their values
 * @param error why it failed
 * @return true if written
 */
bool writeMembers(const std::string& path, const std::vector<std::pair<const char*, JsonValue>>& members,
                  std::string& error) {
    const size_t slash = path.find_last_of('/');
    if (slash != std::string::npos && !makeDirs(path.substr(0, slash))) {
        error = "can't create " + path.substr(0, slash) + ": " + std::strerror(errno);
        return false;
    }
    bool broken = false;
    JsonValue root = readObject(path, &broken);
    if (broken) {
        // The other members it held (the up gains, the sub-tabs, the locks) are gone; the file is kept to look at
        const std::string aside = path + ".broken";
        std::remove(aside.c_str());
        if (std::rename(path.c_str(), aside.c_str()) == 0) {
            std::fprintf(stderr, "[panel] %s couldn't be read; moved to %s and written again\n", path.c_str(),
                         aside.c_str());
        } else {
            std::fprintf(stderr, "[panel] %s couldn't be read (nor moved to %s: %s); written again\n", path.c_str(),
                         aside.c_str(), std::strerror(errno));
        }
    }
    for (const auto& [key, value] : members) root.set(key, value);
    const std::string text = writeJson(root);
    const std::string temp = path + ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out << text;
        out.close();
        if (!out) {
            error = "can't write " + temp;
            std::remove(temp.c_str());
            return false;
        }
    }
    if (std::rename(temp.c_str(), path.c_str()) != 0) {
        error = "can't rename " + temp + ": " + std::strerror(errno);
        std::remove(temp.c_str());
        return false;
    }
    return true;
}

/**
 * Set one string member of the file (as writeMembers).
 * @param path the file
 * @param key the member
 * @param value its text
 * @param error why it failed
 * @return true if written
 */
bool writeMember(const std::string& path, const char* key, const char* value, std::string& error) {
    return writeMembers(path, {{key, JsonValue::makeString(value)}}, error);
}

}  // namespace

std::string defaultPath() {
    const char* xdg = std::getenv("XDG_STATE_HOME");
    std::string base;
    if (xdg != nullptr && xdg[0] == '/') {
        base = xdg;
    } else {
        const char* home = std::getenv("HOME");
        base = std::string(home != nullptr ? home : ".") + "/.local/state";
    }
    return base + "/frameeyeosc/panel.json";
}

const char* pageName(AdvPage page) {
    switch (page) {
        case AdvPage::Version: return "version";
        case AdvPage::Trouble: return "trouble";
        case AdvPage::Tools: return "tools";
        case AdvPage::Files: return "files";
    }
    return "version";
}

bool parsePage(const std::string& name, AdvPage& page) {
    for (const AdvPage p : {AdvPage::Version, AdvPage::Trouble, AdvPage::Tools, AdvPage::Files}) {
        if (name == pageName(p)) {
            page = p;
            return true;
        }
    }
    return false;
}

AdvPage readPage(const std::string& path) {
    const JsonValue root = readObject(path);
    const JsonValue* value = root.get(kPageKey);
    AdvPage page = AdvPage::Version;
    if (value != nullptr && value->isString()) parsePage(value->text, page);
    return page;
}

bool writePage(const std::string& path, AdvPage page, std::string& error) {
    return writeMember(path, kPageKey, pageName(page), error);
}

const char* lidsPageName(LidsPage page) {
    switch (page) {
        case LidsPage::Look: return "look";
        case LidsPage::Blinks: return "blinks";
        case LidsPage::Fine: return "fine";
        case LidsPage::Brows: return "brows";
    }
    return "look";
}

bool parseLidsPage(const std::string& name, LidsPage& page) {
    for (const LidsPage p : {LidsPage::Look, LidsPage::Blinks, LidsPage::Fine, LidsPage::Brows}) {
        if (name == lidsPageName(p)) {
            page = p;
            return true;
        }
    }
    return false;
}

LidsPage readLidsPage(const std::string& path) {
    const JsonValue root = readObject(path);
    const JsonValue* value = root.get(kLidsPageKey);
    LidsPage page = LidsPage::Look;
    if (value != nullptr && value->isString()) parseLidsPage(value->text, page);
    return page;
}

bool writeLidsPage(const std::string& path, LidsPage page, std::string& error) {
    return writeMember(path, kLidsPageKey, lidsPageName(page), error);
}

const char* gazePageName(GazePage page) {
    switch (page) {
        case GazePage::Motion: return "motion";
        case GazePage::Reach: return "reach";
        case GazePage::Glitch: return "glitch";
    }
    return "motion";
}

bool parseGazePage(const std::string& name, GazePage& page) {
    for (const GazePage p : {GazePage::Motion, GazePage::Reach, GazePage::Glitch}) {
        if (name == gazePageName(p)) {
            page = p;
            return true;
        }
    }
    return false;
}

GazePage readGazePage(const std::string& path) {
    const JsonValue root = readObject(path);
    const JsonValue* value = root.get(kGazePageKey);
    GazePage page = GazePage::Motion;
    if (value != nullptr && value->isString()) parseGazePage(value->text, page);
    return page;
}

bool writeGazePage(const std::string& path, GazePage page, std::string& error) {
    return writeMember(path, kGazePageKey, gazePageName(page), error);
}

void readGazeScaleLinks(const std::string& path, bool linked[2]) {
    const JsonValue root = readObject(path);
    for (int pair = 0; pair < 2; ++pair) {
        const JsonValue* value = root.get(kGazeScaleLinkKeys[pair]);
        linked[pair] = value == nullptr || !value->isBool() || value->boolean;
    }
}

bool writeGazeScaleLinks(const std::string& path, const bool linked[2], std::string& error) {
    return writeMembers(path,
                        {{kGazeScaleLinkKeys[0], JsonValue::makeBool(linked[0])},
                         {kGazeScaleLinkKeys[1], JsonValue::makeBool(linked[1])}},
                        error);
}

std::vector<double> readUpGains(const std::string& path) {
    const JsonValue root = readObject(path);
    const JsonValue* value = root.get(kUpGainsKey);
    std::vector<double> gains;
    if (value == nullptr || value->type != JsonValue::Type::Array) return gains;
    for (const JsonValue& item : value->items) {
        if (item.isNumber() && item.number >= 0.5 && item.number <= 2.0) gains.push_back(item.number);
    }
    return gains;
}

bool writeUpGains(const std::string& path, const std::vector<double>& gains, std::string& error) {
    JsonValue list;
    list.type = JsonValue::Type::Array;
    for (double gain : gains) list.items.push_back(JsonValue::makeNumber(gain));
    return writeMembers(path, {{kUpGainsKey, list}}, error);
}

bool readUpdateBeta(const std::string& path) {
    const JsonValue root = readObject(path);
    const JsonValue* value = root.get(kUpdateBetaKey);
    return value != nullptr && value->isBool() && value->boolean;
}

bool writeUpdateBeta(const std::string& path, bool on, std::string& error) {
    return writeMembers(path, {{kUpdateBetaKey, JsonValue::makeBool(on)}}, error);
}

}  // namespace ui_state
