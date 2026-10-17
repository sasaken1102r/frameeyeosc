// Reading a whole (small) file into a string, with an upper limit. Shared by the readers of status.json, config.json,
// the panel's own state, eyecam-rec's status and its protocol files.
#pragma once

#include <cerrno>
#include <cstdio>
#include <limits>
#include <string>

namespace file_io {

/**
 * Read an open file to its end, in 4 KiB chunks rather than allocating the largest allowed size up front or copying
 * through a stringstream. Also works on files that can't seek (/proc). The caller opens and closes the file.
 * @param file the open file (read from where it is)
 * @param text replaced with what was read
 * @param maxBytes the most it may hold
 * @return false on a read error, or with errno EFBIG when the file holds more than maxBytes
 */
inline bool read(std::FILE* file, std::string& text, size_t maxBytes = std::numeric_limits<size_t>::max()) {
    text.clear();
    char chunk[4096];
    for (;;) {
        const size_t n = std::fread(chunk, 1, sizeof(chunk), file);
        if (n > maxBytes - text.size()) {
            errno = EFBIG;
            return false;
        }
        text.append(chunk, n);
        if (n < sizeof(chunk)) return std::ferror(file) == 0;
    }
}

/**
 * Read the file at `path` (see the other read).
 * @param path the file
 * @param text replaced with what was read
 * @param maxBytes the most it may hold
 * @return false if it could not be opened or read, or with errno EFBIG when it holds more than maxBytes
 */
inline bool read(const std::string& path, std::string& text, size_t maxBytes = std::numeric_limits<size_t>::max()) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) return false;
    const bool ok = read(file, text, maxBytes);
    const int error = errno;
    std::fclose(file);
    errno = error;
    return ok;
}

}  // namespace file_io
