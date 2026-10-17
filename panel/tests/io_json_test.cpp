#include "file_io.h"
#include "json.h"

#include <cerrno>
#include <cstdio>
#include <limits>
#include <string>

namespace {

int failures = 0;
#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "FAILED line %d: %s\n", __LINE__, #condition); ++failures; \
} } while (false)

void testReads() {
    std::FILE* file = std::tmpfile();
    CHECK(file != nullptr);
    if (file == nullptr) return;
    std::string actual = "old contents";
    CHECK(file_io::read(file, actual, 0) && actual.empty());
    // More than two chunks, with embedded NULs: no text-mode truncation or trailing padding.
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
    CHECK(!file_io::read(file, actual, 0) && errno == EFBIG);
    std::fclose(file);
}

void testJson() {
    JsonValue root;
    const std::string key = "a_long_setting_name_with_suffix";
    // A view need not be NUL-terminated. Replacing keeps the member's order and owns the key.
    root.set(std::string_view(key.data(), 19), JsonValue::makeNumber(1, true));
    root.set("second", JsonValue::makeString("a\n\"b"));
    root.set(std::string_view(key.data(), 19), JsonValue::makeBool(true));
    CHECK(root.members.size() == 2 && root.members[0].first == key.substr(0, 19));
    CHECK(root.get(std::string_view(key.data(), 19))->boolean);
    CHECK(root.get(key) == nullptr);
    CHECK(writeJson(root) == "{\n  \"a_long_setting_name\": true,\n  \"second\": \"a\\n\\\"b\"\n}\n");
    JsonValue values;
    values.type = JsonValue::Type::Array;
    values.items = {JsonValue::makeNull(), JsonValue::makeNumber(2), JsonValue::makeNumber(1.25),
                    JsonValue::makeNumber(std::numeric_limits<double>::infinity()), JsonValue()};
    values.items.back().type = JsonValue::Type::Object;
    CHECK(writeJson(values) == "[\n  null,\n  2.0,\n  1.25,\n  null,\n  {}\n]\n");
    // Indentation remains exact beyond a typical string's small buffer.
    JsonValue nested = JsonValue::makeString("leaf");
    for (int depth = 0; depth < 20; ++depth) {
        JsonValue parent;
        parent.type = JsonValue::Type::Array;
        parent.items.push_back(std::move(nested));
        nested = std::move(parent);
    }
    const std::string text = writeJson(nested);
    CHECK(text.find(std::string(40, ' ') + "\"leaf\"\n") != std::string::npos);
    JsonValue back;
    std::string error;
    CHECK(parseJson(text, back, error) && writeJson(back) == text);
}

}  // namespace

int main() {
    testReads();
    testJson();
    if (failures == 0) std::puts("io-json-test: all passed");
    return failures != 0;
}
