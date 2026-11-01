// Tests for "Saved data" (stored.cpp): which files are an eye log's, counting and deleting them in a made-up folder
// (never the one being recorded, nor one written to just now, nor anything else there, nor what a link points to),
// the worker thread, and following eyecam-rec's deletes in its status. Built with the panel as stored-test; exits
// non-zero on failure. Files go to a folder made under $TMPDIR (or /tmp), removed at the end.
#include "eyecam.h"
#include "stored.h"

#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>

namespace {

int gFailures = 0;

/**
 * Record a failed check.
 * @param ok the check
 * @param what what was checked
 * @param line where
 */
void check(bool ok, const char* what, int line) {
    if (ok) return;
    ++gFailures;
    std::fprintf(stderr, "FAILED line %d: %s\n", line, what);
}

#define CHECK(condition) check((condition), #condition, __LINE__)

/**
 * Write a file of a size, dated some seconds ago.
 * @param path where
 * @param bytes its size
 * @param ageSec how long ago it was written
 */
void file(const std::string& path, size_t bytes, double ageSec = 3600) {
    std::ofstream(path, std::ios::binary) << std::string(bytes, 'x');
    const std::time_t when = std::time(nullptr) - static_cast<std::time_t>(ageSec);
    utimbuf times {when, when};
    ::utime(path.c_str(), &times);
}

/**
 * @param path the file
 * @return true if it is there (a link counts, even a broken one)
 */
bool exists(const std::string& path) {
    struct stat info {};
    return ::lstat(path.c_str(), &info) == 0;
}

void testNames() {
    CHECK(stored::eyeLogStem("eyes_2026-10-31_10-41-01.csv") == "eyes_2026-10-31_10-41-01");
    CHECK(stored::eyeLogStem("eyes_2026-10-31_10-41-01.log") == "eyes_2026-10-31_10-41-01");
    CHECK(stored::eyeLogStem("eyes_2026-10-31_10-41-01.config.json") == "eyes_2026-10-31_10-41-01");
    CHECK(stored::eyeLogStem("eyes_2026-10-31_10-41-01_2.csv") == "eyes_2026-10-31_10-41-01_2");
    for (const char* other : {"eyes_2026-10-31_10-41-01.eyecam.csv", "eyes_2026-10-31_10-41-01.csv.bak",
                              "eyes_2026-10-31_10-41-01_.csv", "eyes_2026-10-31_10-41-01_x.csv", "eyes_2026-10-31.csv",
                              "notes.csv", "eyes_2026-1x-31_10-41-01.csv", ".csv", "eyes_2026-10-31_10-41-01"}) {
        if (!stored::eyeLogStem(other).empty()) std::fprintf(stderr, "  taken for an eye log: %s\n", other);
        CHECK(stored::eyeLogStem(other).empty());
    }
    CHECK(std::string(stored::eyecamName(stored::Kind::EyecamRecordings)) == "rec");
    CHECK(std::string(stored::eyecamName(stored::Kind::CalibImages)) == "calib_images");
    CHECK(std::string(stored::eyecamName(stored::Kind::EyeLogs)).empty());
}

void testCountAndDelete(const std::string& scratch) {
    const std::string dir = scratch + "/recordings";
    ::mkdir(dir.c_str(), 0755);
    // Two old recordings (one with all three files, one a second in the same second), the one being recorded, one
    // recorded by hand just now, and what isn't an eye log
    file(dir + "/eyes_2026-10-26_19-54-31.csv", 2000);
    file(dir + "/eyes_2026-10-26_19-54-31.log", 50);
    file(dir + "/eyes_2026-10-26_19-54-31.config.json", 30);
    file(dir + "/eyes_2026-10-26_19-54-31_2.csv", 1000);
    file(dir + "/eyes_2026-10-31_10-41-01.csv", 700);
    file(dir + "/eyes_2026-10-31_10-41-01.log", 40);
    file(dir + "/eyes_2026-10-31_10-41-01.config.json", 30);
    file(dir + "/eyes_2026-10-31_11-00-00.csv", 500, 2);
    file(dir + "/notes.txt", 10);
    file(dir + "/eyes_2026-10-12_16-06-27.eyecam.csv", 10);
    file(scratch + "/elsewhere.csv", 900);
    CHECK(::symlink((scratch + "/elsewhere.csv").c_str(), (dir + "/eyes_2026-10-01_00-00-00.csv").c_str()) == 0);
    ::mkdir((dir + "/eyes_2026-10-02_00-00-00.csv").c_str(), 0755);
    const std::string recording = dir + "/eyes_2026-10-31_10-41-01";
    const std::time_t now = std::time(nullptr);
    // Counted: the two old ones only (the one being recorded, the one just written, links and folders left out)
    const stored::Usage before = stored::eyeLogUsage(dir, recording, now);
    CHECK(before.count == 2 && before.bytes == 3080);
    // Nothing recorded now: that one counts too
    CHECK(stored::eyeLogUsage(dir, "", now).count == 3);
    CHECK(stored::eyeLogUsage(scratch + "/missing", "", now).count == 0);
    // Deleted: the same two, whole; everything else stays
    std::string error;
    const stored::Usage gone = stored::deleteEyeLogs(dir, recording, now, error);
    CHECK(gone.count == 2 && gone.bytes == 3080 && error.empty());
    for (const char* name : {"eyes_2026-10-26_19-54-31.csv", "eyes_2026-10-26_19-54-31.log",
                             "eyes_2026-10-26_19-54-31.config.json", "eyes_2026-10-26_19-54-31_2.csv"}) {
        CHECK(!exists(dir + "/" + name));
    }
    for (const char* name : {"eyes_2026-10-31_10-41-01.csv", "eyes_2026-10-31_10-41-01.log",
                             "eyes_2026-10-31_10-41-01.config.json", "eyes_2026-10-31_11-00-00.csv", "notes.txt",
                             "eyes_2026-10-12_16-06-27.eyecam.csv", "eyes_2026-10-01_00-00-00.csv",
                             "eyes_2026-10-02_00-00-00.csv"}) {
        if (!exists(dir + "/" + name)) std::fprintf(stderr, "  deleted: %s\n", name);
        CHECK(exists(dir + "/" + name));
    }
    CHECK(exists(scratch + "/elsewhere.csv"));
    CHECK(stored::eyeLogUsage(dir, recording, now).count == 0);
    // A file that can't go (its folder read-only): said, and the rest still tried
    const std::string locked = scratch + "/locked";
    ::mkdir(locked.c_str(), 0755);
    file(locked + "/eyes_2026-10-20_00-00-00.csv", 10);
    ::chmod(locked.c_str(), 0555);
    if (::access((locked + "/eyes_2026-10-20_00-00-00.csv").c_str(), F_OK) == 0 && ::geteuid() != 0) {
        const stored::Usage none = stored::deleteEyeLogs(locked, "", now, error);
        CHECK(none.count == 0 && error.find("eyes_2026-10-20_00-00-00.csv") != std::string::npos);
    }
    ::chmod(locked.c_str(), 0755);
}

void testWorker(const std::string& scratch) {
    const std::string dir = scratch + "/worker";
    ::mkdir(dir.c_str(), 0755);
    file(dir + "/eyes_2026-10-26_19-54-31.csv", 2000);
    file(dir + "/eyes_2026-10-27_19-54-31.csv", 1000);
    stored::EyeLogs logs;
    stored::View view;
    // (take starts a delete that waited for a count)
    const auto settle = [&]() {
        for (int i = 0; i < 1000 && logs.busy(); ++i) {
            logs.take(view);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        logs.take(view);
    };
    CHECK(logs.count(dir, ""));
    settle();
    CHECK(view.eyeLogsCounted && view.eyeLogs.count == 2 && view.eyeLogs.bytes == 3000);
    // A delete asked for while a count may still run (it waits for it), then goes
    CHECK(logs.count(dir, ""));
    CHECK(logs.remove(dir, ""));
    view.of(stored::Kind::EyeLogs).running = true;
    settle();
    const stored::Delete& d = view.of(stored::Kind::EyeLogs);
    CHECK(!logs.busy() && !d.running && d.done && d.ok && d.usage.count == 2 && d.usage.bytes == 3000);
    CHECK(view.eyeLogs.count == 0 && !exists(dir + "/eyes_2026-10-26_19-54-31.csv"));
}

void testFollowEyecam() {
    stored::View view;
    eyecam::Status s;
    s.stored.present = true;
    stored::Delete& rec = view.of(stored::Kind::EyecamRecordings);
    // Sent: under way while the status says so, done once it shows that delete ended after it was sent
    rec.running = true;
    rec.sentUnix = 1000.0;
    s.stored.deleting = "rec";
    CHECK(!stored::followEyecam(view, s, 1001.0) && rec.running);
    // (an earlier delete of it, from before this one, isn't this one's end)
    s.stored.deleting.clear();
    s.stored.hasDeleted = true;
    s.stored.deletedKind = "rec";
    s.stored.deletedUnix = 900.0;
    CHECK(!stored::followEyecam(view, s, 1002.0) && rec.running);
    s.stored.deletedUnix = 1002.5;
    s.stored.deletedCount = 13;
    s.stored.deletedBytes = 25'000'000'000LL;
    CHECK(stored::followEyecam(view, s, 1003.0));
    CHECK(!rec.running && rec.done && rec.ok && rec.usage.count == 13 && rec.usage.bytes == 25'000'000'000LL);
    // Not all of it: why, in both languages
    stored::Delete& calib = view.of(stored::Kind::CalibImages);
    calib.running = true;
    calib.sentUnix = 2000.0;
    s.stored.deletedKind = "calib_images";
    s.stored.deletedUnix = 2001.0;
    s.stored.deletedError = "/home/steamos/eyecam/calib_x/eye_L.raw: Permission denied (os error 13)";
    CHECK(stored::followEyecam(view, s, 2001.5));
    CHECK(calib.done && !calib.ok && calib.error == s.stored.deletedError);
    // Never shown under way or done: given up on after kEyecamDoneSec
    rec = stored::Delete();
    rec.running = true;
    rec.sentUnix = 3000.0;
    CHECK(!stored::followEyecam(view, s, 3000.0 + stored::kEyecamDoneSec - 1) && rec.running);
    CHECK(stored::followEyecam(view, s, 3000.0 + stored::kEyecamDoneSec) && rec.done && !rec.ok);
    // One asked for elsewhere: shown under way while the status says so
    rec = stored::Delete();
    s.stored.deleting = "rec";
    CHECK(stored::followEyecam(view, s, 4000.0) && rec.running);
}

void testParse() {
    // status.json's "stored" as eyecam-rec writes it (stored::Stored::to_json), and without it
    const std::string text =
        "{\"version\":1,\"state\":\"idle\",\"stored\":{\"scanned_unix\":1790000000.500,\"rec\":{\"count\":13,"
        "\"bytes\":25310000000},\"calib_images\":{\"count\":52,\"bytes\":21940000000},\"deleting\":\"rec\","
        "\"deleted\":{\"kind\":\"calib_images\",\"count\":2,\"bytes\":9,\"error\":\"e\","
        "\"unix\":1790000001.000}},\"pid\":7,\"updated_unix\":1790000002.000}";
    const eyecam::Status s = eyecam::parseStatus(text, 1790000002.0);
    const eyecam::Stored& d = s.stored;
    CHECK(d.present && d.scannedUnix == 1790000000.5 && d.recCount == 13 && d.recBytes == 25310000000LL);
    CHECK(d.calibCount == 52 && d.calibBytes == 21940000000LL && d.deleting == "rec");
    CHECK(d.hasDeleted && d.deletedKind == "calib_images" && d.deletedCount == 2 && d.deletedBytes == 9);
    CHECK(d.deletedError == "e" && d.deletedUnix == 1790000001.0);
    const eyecam::Status old = eyecam::parseStatus("{\"version\":1,\"state\":\"idle\"}", 1.0);
    CHECK(!old.stored.present && !old.stored.hasDeleted);
    const eyecam::Status none = eyecam::parseStatus(
        "{\"version\":1,\"state\":\"idle\",\"stored\":{\"scanned_unix\":0.000,\"rec\":{\"count\":0,\"bytes\":0},"
        "\"calib_images\":{\"count\":0,\"bytes\":0},\"deleting\":\"\",\"deleted\":null}}", 1.0);
    CHECK(none.stored.present && none.stored.scannedUnix == 0.0 && !none.stored.hasDeleted);
}

}  // namespace

int main() {
    const char* tmp = std::getenv("TMPDIR");
    std::string pattern = std::string(tmp != nullptr && tmp[0] == '/' ? tmp : "/tmp") + "/stored-test-XXXXXX";
    if (::mkdtemp(pattern.data()) == nullptr) {
        std::perror("mkdtemp");
        return 1;
    }
    const std::string scratch = pattern;
    testNames();
    testCountAndDelete(scratch);
    testWorker(scratch);
    testFollowEyecam();
    testParse();
    const std::string remove = "rm -rf '" + scratch + "'";
    if (std::system(remove.c_str()) != 0) std::fprintf(stderr, "could not remove %s\n", scratch.c_str());
    if (gFailures == 0) std::printf("stored-test: all passed\n");
    return gFailures == 0 ? 0 : 1;
}
