// file_io::read (from PR #33's io_json_test.cpp by cinnacappu): whole files, the size limit, embedded NULs, chunk
// boundaries and a missing file: panel/build/file-io-test
#include "file_io.h"

#include <cerrno>
#include <cstdio>
#include <string>

namespace {

int failures = 0;
#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "FAILED line %d: %s\n", __LINE__, #condition); ++failures; \
} } while (false)

/**
 * An open file: empty, then more than two chunks with NULs at the chunk edges, read whole and against the limit.
 */
void testOpenFile() {
    std::FILE* file = std::tmpfile();
    CHECK(file != nullptr);
    if (file == nullptr) return;
    std::string actual = "old contents";
    CHECK(file_io::read(file, actual, 0) && actual.empty());
    // No text-mode truncation or padding
    std::string bytes(8193, 'x');
    bytes[4095] = bytes[4096] = bytes[8192] = '\0';
    CHECK(std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size());
    std::rewind(file);
    CHECK(file_io::read(file, actual) && actual == bytes);
    std::rewind(file);
    CHECK(file_io::read(file, actual, bytes.size()) && actual == bytes);
    std::rewind(file);
    errno = 0;
    CHECK(!file_io::read(file, actual, bytes.size() - 1) && errno == EFBIG);
    std::rewind(file);
    errno = 0;
    CHECK(!file_io::read(file, actual, 0) && errno == EFBIG);
    std::fclose(file);
}

/**
 * By path: a file in the current folder (exactly one chunk long), the limit, and a missing file.
 */
void testPath() {
    const std::string path = "file-io-test.tmp";
    const std::string bytes(4096, 'y');
    std::FILE* file = std::fopen(path.c_str(), "wb");
    CHECK(file != nullptr);
    if (file == nullptr) return;
    CHECK(std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size());
    std::fclose(file);
    std::string actual;
    CHECK(file_io::read(path, actual) && actual == bytes);
    CHECK(file_io::read(path, actual, 4096) && actual == bytes);
    errno = 0;
    CHECK(!file_io::read(path, actual, 4095) && errno == EFBIG);
    std::remove(path.c_str());
    CHECK(!file_io::read(path, actual));
}

}  // namespace

int main() {
    testOpenFile();
    testPath();
    if (failures == 0) std::puts("file-io-test: all passed");
    return failures != 0;
}
