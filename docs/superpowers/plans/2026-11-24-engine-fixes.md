# Engine Fixes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make file downloads retry by themselves, resume on more servers, use 1x disk space, survive crashes, and obey user-set limits on downloads at once and connections per server.

**Architecture:** The queue (`src/download_queue.cpp`) gains a retry wait and an active-jobs limit. The engine (`archive::download` in `src/http.cpp`) gains a second resume validator, a restart path, a single sparse staging file with timed checkpoints, and a shared per-host connection counter. Pure decisions move into a new `src/staging.{h,cpp}` so they are unit-tested; a loopback HTTP server tests the engine end to end.

**Tech Stack:** C++20, WinHTTP (Windows) and libcurl (Linux), nlohmann/json, wxWidgets, CMake/CTest, Python 3 standard library for the test server.

**Spec:** `docs/superpowers/specs/2026-11-24-engine-fixes-design.md`. Read it before any task.

## Global Constraints

- Every change builds and behaves the same on Windows and Linux; platform file code goes behind `src/platform/files.h` (both `files_windows.cpp` and `files_linux.cpp`).
- The two transports stay (`src/platform/http_winhttp.inc`, `src/platform/http_curl.inc`); no new third-party dependency.
- Partial data is reused only when proven to be the same file version (hash, strong ETag, or `Last-Modified` plus size) and on disk. An existing destination is never overwritten.
- New UI text uses `_(L"...")`; backend status text shown in the window is marked with `wxTRANSLATE()` in `src/wx/localization.cpp`. After adding text run `python tools/translations.py --update`, fill in all 12 non-English `locale/*.po` entries, then `python tools/translations.py`. Error messages that carry numbers stay English, like the existing technical errors.
- Format changed C++ with the repository's `.clang-format`. Write files with the editor tools, never shell heredocs.
- Tests follow the existing style: one `main`, a failed expectation throws `std::runtime_error` with a sentence naming it.
- Build: `cmake --build --preset vs2022-release`. Test: `ctest --test-dir build/vs2022 -C Release --output-on-failure`. All existing tests keep passing after every task.
- Implementers do not commit, push or switch branches. The reviewer commits each task on branch `engine-fixes` after review.

## Review Focus

1. A server answers the size probe with 206 but answers real range requests with 200, while other connections already hold data: the download must restart once on one connection and produce the correct file (Task 4 test `rangeprobe`).
2. The file changes on the server between pause and resume, on a server that ignores `If-Range`: the result must be one consistent version, never a mix (Task 4 test `ignore-ifrange`).
3. A crash leaves `state.json` older than `data.tmp`: resume must produce the correct file (Task 6 test "snapshot resume").
4. A job is paused or removed while it waits to retry, or while it waits for a free connection: it must leave that wait at once and not start again by itself (Task 2 tests d and g, Task 8 test "stop while waiting").
5. A limit is lowered while it is fully used: nothing deadlocks, running work finishes, and waiting work starts when the count drops under the new limit (Task 7 test c, Task 8 test "lowering").

Not testable here, check in review: the destination drive does not support sparse files (FAT32, exFAT). Marking the file sparse fails; the download must continue.

---

## File map

| File | Responsibility |
|---|---|
| `tests/engine_server.py` (new) | Loopback HTTP server with scripted behaviors and request statistics |
| `tests/run_engine_tests.py` (new) | Starts the server, runs `engine_tests`, returns its exit code |
| `tests/engine.cpp` (new) | End-to-end tests of `archive::download` |
| `src/staging.h`, `src/staging.cpp` (new) | Pure staging decisions: validator, free-space message, old-layout conversion |
| `tests/staging.cpp` (new) | Unit tests for the above and for `DataFile` and `HostSlot` |
| `src/platform/files.h`, `files_windows.cpp`, `files_linux.cpp` | `DataFile`: positional writes into one sparse file |
| `src/host_limit.h`, `src/host_limit.cpp` (new) | Per-host connection counter |
| `src/http.cpp` | Engine changes |
| `src/download_queue.h`, `.cpp` | Retry wait, downloads-at-once limit |
| `src/wx/settings_dialog.cpp` (new), `src/wx/ui.h`, `manager.cpp`, `app.cpp` | Settings window, retry text |

Add each new source file to the right target in `CMakeLists.txt` (`staging.cpp` and `host_limit.cpp` to `archive_core`, `settings_dialog.cpp` to `skein-downloader`) and each new test to `tests/tests.cmake`.

---

### Task 1: Engine test harness

Proves today's engine behavior so later tasks cannot break it unnoticed. No product code changes.

**Files:**
- Create: `tests/engine_server.py`, `tests/run_engine_tests.py`, `tests/engine.cpp`
- Modify: `tests/tests.cmake`

**Interfaces:**
- Produces: URLs `http://127.0.0.1:<port>/<mode>/<size>/file.bin`, `GET /stats`, `GET /reset`; the helpers below in `tests/engine.cpp`. Later tasks add modes and cases to these files.

- [ ] **Step 1: Write `tests/engine_server.py`**

`ThreadingHTTPServer` on `127.0.0.1`, port 0. Content version `v` of a file is byte `i -> (i * 31 + 7 + v) & 0xFF`. Version 1 unless a mode says otherwise. Fixed headers: `ETag: "skein-test-1"`, `Last-Modified: Mon, 02 Nov 2026 10:00:00 GMT` (version 2: `"skein-test-2"` and `10:00:05`). Modes:

| Mode | Behavior |
|---|---|
| `etag` | Strong ETag, no Last-Modified. Honors `Range`; `If-Range` not equal to the ETag gives 200 with the whole file |
| `norange` | Ignores `Range`: always 200 with `Content-Length` |
| `nolength` | Ignores `Range`: 200, no `Content-Length`, closes the connection at the end |

`/stats` returns JSON: `requests` (count), `ranged` (requests with a `Range` other than `bytes=0-0`), `bytes` (body bytes sent), `maxConcurrent` (most ranged responses in flight at once). `/reset` zeroes them. Responses are written in 64 KiB pieces with a 2 ms sleep between pieces, so pauses and concurrency are observable.

- [ ] **Step 2: Write `tests/run_engine_tests.py <engine_tests path>`**

Starts the server on a thread, runs the executable with the base URL as its only argument, exits with its code.

- [ ] **Step 3: Write `tests/engine.cpp` with these helpers and cases**

```cpp
std::string expectedBytes(uint64_t size, int version = 1);              // the server's content
std::string readFile(const archive::Path&);
nlohmann::json stats(const std::string& base);                          // GET /stats via archive::fetch
void reset(const std::string& base);                                    // GET /reset
// Runs archive::download; when pauseAt is not 0, sets stop once done >= pauseAt and expects "Paused".
void run(const std::string& url, const archive::Path& target, unsigned connections, uint64_t pauseAt = 0);
```

Cases, each in a fresh temporary folder (`MiB = 1024 * 1024`):
1. `etag/10 MiB`, 4 connections: file equals `expectedBytes`; `stats.ranged >= 10`.
2. `norange/3 MiB` and `nolength/1 MiB`, 4 connections: file correct.
3. `etag/0`: an empty file is created.
4. `etag/10 MiB`, 4 connections, `pauseAt = 3 MiB`, then run again: file correct; the staging folder `<target>.archive-parts` is gone; `stats.bytes < 15 MiB` (the resume reused data).
5. A destination that already exists: `run` throws and the file is unchanged.

- [ ] **Step 4: Register in `tests/tests.cmake`**

`engine_tests` links `archive_core`; inside the existing `if(Python3_FOUND)` add test `engine` running `run_engine_tests.py $<TARGET_FILE:engine_tests>`. Add `engine_tests` to the `skein_warnings(...)` call.

- [ ] **Step 5: Build and run**

Run the Global Constraints build and test commands. Expected: `engine` and all existing tests pass.

---

### Task 2: Automatic retry in the queue

**Files:**
- Modify: `src/download_queue.h`, `src/download_queue.cpp`, `src/wx/manager.cpp`, `locale/*`
- Test: `tests/download_queue.cpp`

**Interfaces:**
- Produces:
  - `uint64_t downloads::Job::retryIn = 0;` seconds until an automatic retry, 0 when none. Filled by `Queue::snapshot()`, never saved.
  - `void Queue::setRetryWaits(std::vector<std::chrono::milliseconds> waits, unsigned maxRetries);` default `{5s, 15s, 60s, 300s}` and `12`; the last wait repeats.

- [ ] **Step 1: Write failing tests** in `tests/download_queue.cpp`, a new block with its own history file and queue per case, `setRetryWaits({10ms}, 12)` unless stated:

a. Runner throws `archive::NetworkError("x")` twice, then returns: job is Complete, runner ran 3 times.
b. Runner throws `archive::NetworkError("x", false)`: job is Failed with detail `x`, runner ran once.
c. `setRetryWaits({10ms}, 3)`, runner always throws retryable: job is Failed, runner ran 4 times.
d. `setRetryWaits({10s}, 12)`, runner throws retryable: snapshot shows `State::Queued`, `retryIn` from 1 to 10, detail `x`. `pause(id)`: Paused, `retryIn == 0`, runner not called again within 200 ms. `resume(id)`: runner called again within 1 s.
e. Runner throws `archive::NetworkError("x", true, 2)` (a 2 s `Retry-After`): `retryIn >= 1` although the schedule says 10 ms.
f. `setRetryWaits({10ms}, 2)`; runner reports `done` one higher on each run and throws retryable on runs 1 to 5, returns on run 6: job is Complete (progress resets the count).
g. `setRetryWaits({10s}, 12)`, runner throws retryable, then `remove(id)`: the job is gone from the snapshot and the runner is not called again.
h. A runner that throws `std::runtime_error("y")`: Failed at once (only `NetworkError` retries).

- [ ] **Step 2: Run** the test command. Expected: `download_queue` fails to compile (`setRetryWaits`, `retryIn` unknown).

- [ ] **Step 3: Implement**

`Entry` gets the retry count, the not-before time and the `done` value at the start of the run. In `Queue::work` catch `const archive::NetworkError&` before `std::exception`. A retryable failure of a job that was not stopped and is not being removed: state `Queued`, detail keeps the error text, wait = `max(schedule value, e.wait seconds)`. Workers skip entries whose time has not come, and the condition wait becomes a timed wait until the earliest such time. `pause`, `pauseAll`, `resume`, `resumeAll` and `shutdown` clear the wait; `resume` and `resumeAll` also clear the count. Everything else that tests for `State::Queued` is unchanged.

- [ ] **Step 4: Show it** in `src/wx/manager.cpp`: one helper `wxString jobDetail(const downloads::Job&)` that returns `wxString::Format(_(L"Retrying in %d s"), int(j.retryIn)) + " · " + translated(j.detail)` when `retryIn` is not 0 and `translated(j.detail)` otherwise; use it at the three places that show `translated(j.detail)` (the two tooltips and `selectedDetail`).

- [ ] **Step 5: Translations** per Global Constraints for `Retrying in %d s`.

- [ ] **Step 6: Build and run** the test command. Expected: all tests pass, including `translation_catalogs`.

---

### Task 3: Free-space check

**Files:**
- Create: `src/staging.h`, `src/staging.cpp`, `tests/staging.cpp`
- Modify: `src/http.cpp`, `CMakeLists.txt`, `tests/tests.cmake`

**Interfaces:**
- Produces: `std::string archive::freeSpaceError(uint64_t needed, uint64_t available);` empty when it fits, otherwise `"Not enough free space: " + formatBytes(needed) + " needed, " + formatBytes(available) + " free."`

- [ ] **Step 1: Failing test** in `tests/staging.cpp` (links `archive_core`, test name `staging`): `freeSpaceError(10, 10)` is empty; `freeSpaceError(11, 10)` equals the sentence above built with `formatBytes`.
- [ ] **Step 2: Run.** Expected: build fails, `staging.h` missing.
- [ ] **Step 3: Implement** the function. In `archive::download`, after the existing loop that counts bytes already in part files and only when the size is known: `needed = (size - done) + size` (assembly copies the file; Task 6 changes this), `available` from `std::filesystem::space(destination.parent_path(), errorCode).available`; skip the check when `space` reports an error; otherwise throw `std::runtime_error` with a non-empty message. Log it with `LOG_ERROR`.
- [ ] **Step 4: Build and run.** Expected: all tests pass.

---

### Task 4: Last-Modified resume and the restart path

**Files:**
- Modify: `src/http.cpp`, `src/platform/http_curl.inc`, `src/staging.h`, `src/staging.cpp`, `tests/engine_server.py`, `tests/engine.cpp`, `tests/staging.cpp`

**Interfaces:**
- Produces: `std::wstring archive::resumeValidator(const std::wstring& etag, const std::wstring& lastModified);` the ETag when it is not empty and does not start with `W/`, otherwise `lastModified`.

- [ ] **Step 1: Failing unit test** in `tests/staging.cpp`: `resumeValidator(L"\"a\"", L"date") == L"\"a\""`; `resumeValidator(L"W/\"a\"", L"date") == L"date"`; `resumeValidator(L"", L"") == L""`.

- [ ] **Step 2: Add server modes**

| Mode | Behavior |
|---|---|
| `modified` | `Last-Modified` only. Honors `Range`; `If-Range` not equal to the date gives 200 with the whole file |
| `weak` | `ETag: W/"skein-weak"` only, honors `Range` |
| `rangeprobe` | `Last-Modified` only. `bytes=0-0` gets 206; every other request gets 200 with the whole file |
| `changed` | Like `modified`, serving version 1 until 3 ranged requests have been answered since `/reset`, then version 2 with its date |
| `ignore-ifrange` | Like `changed`, but never looks at `If-Range`: always 206 for a `Range` |

- [ ] **Step 3: Failing engine tests** (10 MiB, 4 connections unless stated):
1. `modified`: file correct, `stats.maxConcurrent >= 2`.
2. `modified` with `pauseAt = 3 MiB`, then again: file correct, `stats.bytes < 15 MiB`.
3. `weak`: file correct, `stats.ranged == 0` after the probe (one connection, as today).
4. `rangeprobe`: file correct.
5. `changed`: file equals `expectedBytes(size, 2)`.
6. `ignore-ifrange`: file equals `expectedBytes(size, 2)`.

- [ ] **Step 4: Run.** Expected: cases 1, 2 (too many bytes or one connection), 4, 5 and 6 fail.

- [ ] **Step 5: Implement**

- `http_curl.inc`: `constexpr const char* WINHTTP_QUERY_LAST_MODIFIED = "last-modified";` (WinHTTP has the constant).
- In `download`: the validator is `resumeValidator(etag, Last-Modified of the probe)`. `resumable = ranged && (hash || !validator.empty())`. The validator is what goes into `If-Range` for the probed source; mirror sources keep their own ETag as today.
- `state.json`: key `modified` holds the `Last-Modified` value when it is the validator; write the key only when not empty, so manifests from the current version still match. With a hash, copy the new value over the previous one before comparing, as the code does for `etag`.
- New private exception `RestartWhole`. A ranged request throws it when the answer is 200, or when the download has no hash, the source's validator is not empty, and the answer's validator (`resumeValidator` of its ETag and Last-Modified) differs. A 206 with a wrong `Content-Range` stays the existing error.
- `RestartWhole` stops all of this download's connections. The engine then, once per call of `download`: logs the reason, deletes the staged data, switches to one connection without resume (`ranged = false`, ranges `split(size, 1)`, state rewritten), repeats the free-space check for the full size, and transfers again. `RestartWhole` is not a `NetworkError`, so the per-part retry loop does not catch it.

- [ ] **Step 6: Build and run.** Expected: all tests pass.

---

### Task 5: `DataFile`

**Files:**
- Modify: `src/platform/files.h`, `src/platform/files_windows.cpp`, `src/platform/files_linux.cpp`
- Test: `tests/staging.cpp`

**Interfaces:**
- Produces, in namespace `platform`:

```cpp
// One staging file written at any offset from any thread.
class DataFile {
public:
    // Opens path, creating it when missing; existing bytes are kept. With sizeKnown the
    // file's length is set to size (on Windows it is marked sparse first; failure to mark is ignored).
    DataFile(const archive::Path& path, bool sizeKnown, uint64_t size);
    ~DataFile();
    DataFile(const DataFile&) = delete;
    void write(uint64_t offset, const char* data, size_t size); // throws "Disk write failed. Check available space."
    void sync();                                                // data to disk (FlushFileBuffers / fsync)
};
```

- [ ] **Step 1: Failing tests** in `tests/staging.cpp`:
a. Create with size 8 MiB; from two threads write 1 MiB of `0xAB` at offset 6 MiB and 1 MiB of `0xCD` at offset 0; destroy; the file is 8 MiB long and both regions read back.
b. Reopen the same path with the same size, write 1 byte at offset 3 MiB, destroy: the regions from (a) are still there.
c. `sizeKnown = false`: writing 10 bytes at 0 then 10 at 10 gives a 20-byte file.
d. A path that is a symbolic link (Linux) or junction (Windows, skip when it cannot be created) throws.
- [ ] **Step 2: Run.** Expected: compile failure.
- [ ] **Step 3: Implement.** Windows: `CreateFileW` with `OPEN_ALWAYS` and `FILE_SHARE_READ`, `FSCTL_SET_SPARSE` before `SetEndOfFile`, `WriteFile` with an `OVERLAPPED` carrying the offset (a synchronous handle; loop until all bytes are written). Linux: `open` with `O_CREAT | O_CLOEXEC | O_NOFOLLOW`, `ftruncate`, `pwrite` in a loop. Call `regular(path)` first on both.
- [ ] **Step 4: Build and run.** Expected: all tests pass.

---

### Task 6: One staging file with checkpoints

**Files:**
- Modify: `src/http.cpp`, `src/staging.h`, `src/staging.cpp`, `tests/engine.cpp`, `tests/staging.cpp`

**Interfaces:**
- Consumes: `platform::DataFile` (Task 5), `archive::freeSpaceError` (Task 3).
- Produces:
  - `std::vector<uint64_t> archive::convertedDone(const std::vector<Range>& oldRanges, const std::vector<uint64_t>& oldLengths, const std::vector<Range>& newRanges);` for each new range, the number of contiguous valid bytes starting at its first byte, where old part `i` makes bytes `oldRanges[i].first ... first + min(oldLengths[i], its range length) - 1` valid.
  - `void archive::setCheckpointInterval(std::chrono::milliseconds);` default 5 s; for tests.

- [ ] **Step 1: Failing unit tests** for `convertedDone` in `tests/staging.cpp`:
a. Same partition old and new: result equals `oldLengths`.
b. Old `{0-99}` with length 50, new `{0-24, 25-49, 50-74, 75-99}`: `{25, 25, 0, 0}`.
c. Old `{0-49, 50-99}` with lengths `{50, 10}`, new `{0-39, 40-79, 80-99}`: `{40, 20, 0}`.
d. An old length larger than its range counts as the range length.
e. Empty old vectors: all zeros.

- [ ] **Step 2: Failing engine tests** (`etag` and `modified`, 10 MiB, 4 connections):
1. In the progress callback of a normal run, the staging folder never contains a name ending in `.part` or `assembled.tmp`.
2. Snapshot resume: `setCheckpointInterval(100ms)`; when `done >= 4 MiB` the callback copies the staging folder (all files except `lock`) to a side folder and then sets stop. After the run ends, replace the staging folder's files with the copies and run again: file correct, `stats.bytes < 17 MiB`.
3. Old layout: build a staging folder by hand in the `queued-v1` format the current code writes (read `download` before changing it: `owner.txt`, `state.json` with `url`, `size`, `known`, `sha1`, `etag`, `parts`, `ranged`, `layout`, `source` and `"clean": true`), with parts 0 and 1 complete and part 2 half written, all holding the server's bytes, for the `etag` mode. Run: file correct, `stats.bytes < 8.5 MiB`, no `.part` file left.
4. The same folder without `"clean": true`: file correct, `stats.bytes >= 10 MiB` (unsafe old data is discarded, as today).

- [ ] **Step 3: Run.** Expected: the unit tests fail to compile; engine tests 1 to 3 fail.

- [ ] **Step 4: Implement** in `download`, following the spec's "Piece 2":

- Staging folder, `owner.txt`, `lock` and `state.json` stay. Data goes to `<staging>/data.tmp` through one `platform::DataFile` shared by all connections. `state.json` gets `"layout": "single-v1"` and `"done"`, an array with one byte count per range. The `clean` key and its rule go away.
- Each connection keeps its range's count in an atomic and writes with `DataFile::write(range.first + count, ...)`.
- Checkpoint: copy the counts, `DataFile::sync()`, then write `state.json` through a temporary file and `platform::replace`. Run it from a timer thread every checkpoint interval during the transfer, and once when the transfer stops for any reason. One mutex serializes checkpoints.
- Resume: a state that matches (same comparison as today, plus the layout) restores the counts from `done`; a count larger than its range, or a `data.tmp` shorter than the size, discards everything.
- Old layouts (`queued-v1`, and the one without a `layout` key whose ranges are `split(size, parts)`): when today's rules would reuse the parts, copy each part into `data.tmp` at its range's first byte and delete it, one at a time, then set the counts with `convertedDone` and checkpoint. Otherwise delete the parts.
- Single-connection and unknown-size transfers write `data.tmp` from 0 (`sizeKnown` false when unknown) and never resume.
- Finish: checkpoint, check the size, verify the hash by reading `data.tmp`, `platform::publish(data.tmp, destination)`, remove the staging folder as today. A hash mismatch deletes `data.tmp`.
- The free-space need becomes `size - done`.
- `RestartWhole` (Task 4) now truncates and reuses `data.tmp`.

- [ ] **Step 5: Build and run.** Expected: every test passes, including all of Task 1's and Task 4's engine cases.

---

### Task 7: Downloads-at-once limit

**Files:**
- Modify: `src/download_queue.h`, `src/download_queue.cpp`
- Test: `tests/download_queue.cpp`

**Interfaces:**
- Produces: `void Queue::setLimit(unsigned jobs);` clamped to 1 to 4, default 4, callable at any time from any thread.

- [ ] **Step 1: Failing tests** (runner blocks until the test releases a job by id):
a. `setLimit(2)` before `start()`, 4 jobs: exactly 2 have started after 300 ms.
b. Then `setLimit(3)`: a third starts within 1 s.
c. Then `setLimit(1)`: releasing one job starts no new one (2 still running); releasing a second still starts none (1 running); releasing the third starts the fourth.
d. `setLimit(0)` and `setLimit(9)` behave as 1 and 4.
- [ ] **Step 2: Run.** Expected: compile failure.
- [ ] **Step 3: Implement.** Keep the four worker threads. A worker takes a job only while the number of running entries is under the limit; `setLimit` and every job end notify the workers.
- [ ] **Step 4: Build and run.** Expected: all tests pass.

---

### Task 8: Connections per server

**Files:**
- Create: `src/host_limit.h`, `src/host_limit.cpp`
- Modify: `src/http.cpp`, `src/wx/localization.cpp`, `locale/*`, `CMakeLists.txt`, `tests/staging.cpp`, `tests/engine.cpp`

**Interfaces:**
- Produces, in namespace `archive`:

```cpp
void setHostLimit(unsigned connections); // clamped to 1..16, default 16; takes effect at once
// Holds one of a host's connection slots for its lifetime.
class HostSlot {
public:
    // Waits for a free slot of host (compared in lower case). Calls waiting() once if it has to wait.
    // Throws std::runtime_error("Paused") when stop becomes true while waiting.
    HostSlot(const std::string& host, std::atomic_bool& stop, const std::function<void()>& waiting = {});
    ~HostSlot();
    HostSlot(const HostSlot&) = delete;
};
```

- [ ] **Step 1: Failing unit tests** in `tests/staging.cpp`:
a. `setHostLimit(2)`; 5 threads each hold a `HostSlot("a.test", ...)` for 50 ms: at most 2 at once, all 5 finish.
b. Hosts are separate and case does not matter: with limit 1, a slot for `A.test` blocks `a.test` but not `b.test`.
c. Stop while waiting: with the limit used up, a waiting constructor throws `Paused` within 300 ms of `stop = true`, and `waiting` was called exactly once.
d. Lowering: limit 2 with 2 slots held, `setHostLimit(1)`; a third constructor waits until both are released... then gets a slot.
e. Raising: limit 1 with 1 held and 1 waiting, `setHostLimit(2)`: the waiter gets its slot within 300 ms.
- [ ] **Step 2: Failing engine test:** `setHostLimit(2)`; two downloads of `etag/6 MiB` to different targets, 4 connections each, on two threads: both files correct and `stats.maxConcurrent <= 2`. Then `setHostLimit(16)` so later cases are unaffected.
- [ ] **Step 3: Run.** Expected: compile failure.
- [ ] **Step 4: Implement.** One mutex, one condition variable, a map from host to slots in use; the waiting loop wakes at least every 100 ms to look at `stop`. In `download`, a connection that transfers file data takes a `HostSlot` for the host of the address it requests (before redirects) just before its request, and holds it until its response has been read or failed. The `waiting` callback reports the status `Waiting for a free connection`. The size probe, `measureServers`, `fetch` and `post` take no slot.
- [ ] **Step 5: Translations:** mark `Waiting for a free connection` with `wxTRANSLATE()` in `src/wx/localization.cpp` next to the other engine statuses, then the Global Constraints translation steps.
- [ ] **Step 6: Build and run.** Expected: all tests pass.

---

### Task 9: Settings window

**Files:**
- Create: `src/wx/settings_dialog.cpp`
- Modify: `src/wx/ui.h`, `src/wx/manager.cpp`, `src/wx/app.cpp`, `CMakeLists.txt`, `locale/*`, `README.md`

**Interfaces:**
- Consumes: `Queue::setLimit` (Task 7), `archive::setHostLimit` (Task 8), `lite::pref` / `lite::save`.
- Produces, declared in `src/wx/ui.h`:
  - `void applySettings(downloads::Queue&);` reads `maxDownloads` (default `4`) and `hostConnections` (default `16`) from `settings.ini`, clamps them, and calls the two setters.
  - `void showSettings(wxWindow*, downloads::Queue&);`

- [ ] **Step 1: Implement `applySettings`** and call it in `src/wx/app.cpp` right after the queue is created, before `queue.start()`.
- [ ] **Step 2: Implement `showSettings`:** a modal native dialog titled `_(L"Settings")` with `_(L"Downloads at once")` as a `wxChoice` of 1, 2, 3, 4 and `_(L"Connections per server")` as a `wxSpinCtrl` from 1 to 16, standard OK and Cancel buttons, `appIcons` applied. OK saves both keys with `save(...)` and calls `applySettings`, so the change applies at once. Follow the spacing and construction style of the About dialog in `src/wx/app.cpp`.
- [ ] **Step 3: Add the button** `_(L"Settings")` to the main window's toolbar in `src/wx/manager.cpp`, in the group with About, before it.
- [ ] **Step 4: Translations** per Global Constraints for the three new strings (reuse existing catalog entries where the text already exists).
- [ ] **Step 5: README:** in Features, replace "with up to four downloads running at once" with wording that says the number is a setting (1 to 4) and mention the per-server connection limit, automatic retry, and that a download needs free space equal to its size.
- [ ] **Step 6: Build and run** the test command. Expected: all tests pass. Then start `dist/skein-downloader.exe --data-dir <a temporary absolute folder>`, open Settings, set Downloads at once to 2, press OK, reopen: it shows 2, and `settings.ini` in that folder holds `maxDownloads=2`.
