#pragma once

#include <cerrno>
#include <cstdio>
#include <limits>
#include <string>

namespace file_io {

// Read incrementally rather than allocating the maximum allowed size or copying through a stringstream.
// The caller owns the file; this also works with non-seekable files such as /proc entries.
inline bool read(std::FILE* file, std::string& text,
                 size_t maxBytes = std::numeric_limits<size_t>::max()) {
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

inline bool read(const std::string& path, std::string& text,
                 size_t maxBytes = std::numeric_limits<size_t>::max()) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) return false;
    const bool ok = read(file, text, maxBytes);
    const int error = errno;
    std::fclose(file);
    errno = error;
    return ok;
}

}  // namespace file_io
