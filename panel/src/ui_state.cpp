// The panel's own state between starts (see ui_state.h).
#include "ui_state.h"

#include "json.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace ui_state {

namespace {

/** The member the sub-tab is kept in. */
constexpr const char* kPageKey = "advanced_page";

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
 * @return the object
 */
JsonValue readObject(const std::string& path) {
    JsonValue root;
    root.type = JsonValue::Type::Object;
    std::ifstream in(path, std::ios::binary);
    if (!in) return root;
    std::ostringstream text;
    text << in.rdbuf();
    JsonValue parsed;
    std::string error;
    if (text.str().size() <= 64 * 1024 && parseJson(text.str(), parsed, error) && parsed.isObject()) return parsed;
    return root;
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
    const size_t slash = path.find_last_of('/');
    if (slash != std::string::npos && !makeDirs(path.substr(0, slash))) {
        error = "can't create " + path.substr(0, slash) + ": " + std::strerror(errno);
        return false;
    }
    JsonValue root = readObject(path);
    root.set(kPageKey, JsonValue::makeString(pageName(page)));
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

}  // namespace ui_state
