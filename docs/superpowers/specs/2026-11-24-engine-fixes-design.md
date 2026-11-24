# Engine fixes: design

Date: 2026-11-24. Status: waiting for approval.

## Intent

Skein's download engine is correct but gives up or wastes resources in cases a
professional download manager handles: a server without a strong ETag, a network drop, a
nearly full disk, and several jobs hitting one server. These fixes make ordinary HTTP,
HTTPS and FTP downloads finish more often, on less disk, with less attention from the
user. They come before sign-in, more protocols and browser integration because those
build on the same engine.

## Goals

1. A job that fails for a temporary network reason retries by itself.
2. A download that cannot fit on the destination drive says so before it starts.
3. Servers that send `Last-Modified` but no strong ETag get multi-connection downloads
   and resume.
4. A download needs disk space equal to its size, not twice its size, and each byte is
   written once.
5. After a crash or power loss, a download resumes from its last checkpoint instead of
   starting over.
6. The user chooses how many downloads run at once: 1, 2, 3 or 4 (requested by the user;
   today it is fixed at 4).
7. Connections to one server are capped across all jobs.

## Non-goals

Sign-in, custom headers or cookies, speed limits, schedules, proxies, libcurl on Windows,
HTTP/2, new protocols, and any change to video (yt-dlp) jobs beyond the downloads-at-once
limit applying to them as it does today.

## Anti-goals

- Partial data is never reused unless it is proven to belong to the same file version
  (hash, strong ETag, or `Last-Modified` plus size) and to have reached the disk.
- An existing destination file is never overwritten.
- A download paused under the current version is never thrown away silently by the
  upgrade.
- No job retries forever without the user being able to see it and stop it.

## Constraints

- C++20, the two transports as they are (`src/platform/http_winhttp.inc`,
  `src/platform/http_curl.inc`); every change works on Windows and Linux.
- Platform-specific file code goes behind `src/platform/files.h`.
- New user-visible text goes through the translation pipeline for all 13 languages
  (`locale/README.md`, `tools/translations.py`).
- Sources are formatted with the repository's `.clang-format`.
- The existing tests (`tests/tests.cmake`) keep passing.

## Approaches considered (writing to disk)

- **A. One sparse staging file written in place (chosen).** Each connection writes at its
  own offset; the finished file is renamed with no copy. 1x disk, one write per byte, and
  checkpoints survive a crash. Costs a staging format change and a conversion for
  downloads paused under the old format.
- **B. Keep part files, delete each as it is assembled.** Small change, no format change,
  but every byte is still written twice and an interrupted assembly needs its own
  recovery.
- **C. Keep everything and only check for 2x free space.** Fixes nothing.

A was recommended; the user did not object and moved on to how the work is carried out.
Treat it as the working choice until this document is approved.

## Design

Three pieces, built in this order as separate commits.

### Piece 1: retry, free space, Last-Modified

**Automatic retry (queue).** Today a failed job stays Failed
(`src/download_queue.cpp`, `Queue::work`).

- When the runner throws an `archive::NetworkError` with `retry == true` and the job was
  not stopped, the job goes back to Queued with a wait before it may start again.
- Waits: 5 s, 15 s, 60 s, then 300 s for each further attempt. After 12 automatic
  attempts in a row the job becomes Failed with the last error.
- An attempt during which the job's `done` grew resets the count.
- While waiting, the row shows "Retrying in N s" (translated, counted down), and the last
  error stays in the log.
- Pause cancels the wait (job becomes Paused). Resume starts at once and resets the count.
  Pause all and shutdown behave as for any Queued job.
- Other failures (HTTP 401, 403, 404, disk errors, checksum mismatch, video jobs) fail at
  once, as today.
- The wait and the count are not saved; after a restart unfinished jobs are Paused, as
  today.
- The engine's own four attempts per part stay as they are.
- A server's `Retry-After` (`NetworkError::wait`) is honored as a minimum: the wait is the
  larger of it and the schedule's value.
- A waiting job is an ordinary Queued job with a not-before time. Pause, remove, move to
  top, pause all, maintenance and shutdown treat it as Queued; only the workers skip it
  until its time comes, so the queue needs a timed wake-up. History saves it as Queued,
  which loads as Paused, as today.
- The wait schedule can be replaced in tests so they do not sleep.

**Free-space check (engine).** In `archive::download`, once the size is known and the
partial data on disk has been counted: if the bytes still needed exceed the free space
on the destination's drive (`std::filesystem::space(...).available`), fail with
"Not enough free space: N needed, M free." This error is not retried. Unknown size: no
check. Until piece 2 lands the need is (size - done) + size, because assembly copies the
file; piece 2 changes it to size - done.

**Last-Modified resume (engine).** Today `resumable` requires a hash or a strong ETag
(`src/http.cpp`, `download`).

- The validator is the strong ETag when there is one, otherwise the probe's
  `Last-Modified` header, sent back unchanged in `If-Range`. Weak ETags are still ignored.
- `resumable = ranged && (hash || strong ETag || Last-Modified)`.
- `state.json` records the validator under a new `modified` key next to `etag`. It
  follows the ETag's rule: for a download without a hash a different value discards the
  partial data; for a hash-verified download the difference is ignored (as
  `previous["etag"]` is overwritten today), because the hash decides.
- Every 206 answer is checked, not only trusted: when the download's identity is a
  validator (no hash), the answer's ETag or `Last-Modified` must equal the stored one. A
  server that ignores `If-Range` and sends a changed file's bytes is caught this way.
- FTP is unchanged (its modification time already stands in as an ETag).
- Mirrors and the speed test are unchanged: they need a SHA-1 and keep using ETags only.
- New fallback, "the file changed or the server will not do ranges": a range request
  answered with HTTP 200, or a 206 whose validator differs. All of the download's
  connections stop, the staged data is discarded, the reason is logged, and the download
  restarts on one connection without resume, including a new free-space check for the
  full size. This happens at most once per call of `archive::download`; in the restarted
  run a 200 is the expected answer. It applies with or without a hash (a hash is still
  verified at the end). A 206 with the wrong `Content-Range` stays the error it is today.

### Piece 2: one staging file

**Layout.** The staging folder `<destination>.archive-parts` stays, with `owner.txt`,
`lock` and `state.json` as today. The numbered `.part` files and `assembled.tmp` are
replaced by one file, `data.tmp`. `state.json` gets `"layout": "single-v1"` and a `done`
array with the bytes completed in each range. The ranges themselves still come from
`queuedRanges(size)`.

**Writing.** A new class behind `src/platform/files.h` opens or creates the data file,
sets its length when the size is known, and writes a buffer at a given offset from any
thread (`WriteFile` with an offset on Windows, `pwrite` on Linux), plus a flush to disk.
On Windows the file is marked sparse before its length is set, so NTFS does not zero-fill
up to each write offset; if marking fails the download continues. On Linux setting the
length leaves it sparse already.

**Checkpoints.** Every 5 seconds while transferring, and whenever the transfer stops
(pause, error, completion):

1. copy the per-range byte counts,
2. flush the data file to disk,
3. write `state.json` with the copied counts, atomically (temporary file, flush, rename).

The saved counts are therefore never ahead of the data on disk, so the state is always
safe to resume from. This replaces the `clean` flag and its rule that unhashed data is
reused only after a normal stop. A crash loses at most the last 5 seconds.

**Single-connection and unknown-size downloads** write the same `data.tmp` from offset 0,
without setting a length when the size is unknown, and without resume, as today.

**Finishing.** Flush, check the size, verify the hash by reading `data.tmp`, rename it to
the destination with `platform::publish`, remove the staging folder. A hash mismatch
deletes `data.tmp` and fails, as today.

**Old staging folders.** A folder whose `state.json` has the old layouts (`queued-v1`, or
the older one without a layout key) and whose data today's rules would reuse is
converted on first resume: each part's bytes are copied into `data.tmp` at their offset
and the part deleted, one part at a time. The old partition (`split(size, count)` for
the oldest layout) differs from `queuedRanges`, so first build the set of valid byte
intervals from all old parts (each part is valid from its first byte for its file
length); for each new range, `done` is the number of contiguous valid bytes starting at
the range's first byte. Old data today's rules would
discard is discarded.

**Free space.** The check from piece 1 becomes size - done. Because the file is sparse,
the disk can still fill during the download; that stays the "Disk write failed" error.

### Piece 3: concurrency settings

**Downloads at once.** A setting with the choices 1, 2, 3 and 4, default 4, saved in
`settings.ini`. The queue keeps its four worker threads and adds a limit on how many
jobs may be active; workers check it before taking a job. A change applies at once:
raising it starts waiting jobs, lowering it lets running jobs finish and starts no new
one until the count is under the limit.

**How the settings reach the code.** The queue and the engine do not read
`settings.ini`. Each gets a setter the window calls at startup and whenever the setting
changes; both take effect live. Lowering a limit lets running jobs or connections finish
and starts no new one until the count is under the limit.

**Connections per server.** A setting from 1 to 16, default 16, saved in `settings.ini`.
It bounds the total across all jobs, so it also bounds one job: a job set to 16
connections runs on 8 when the cap is 8. With the default of 16 a single job is never
reduced, and four 16-connection jobs to one server share 16 instead of opening 64. The
file picker's per-job choices stay as they are. All archive.org jobs that start at
`archive.org` share that one counter even though they are redirected to different
storage servers; a job forced to a named server counts against that server.
The engine keeps one counter per server host name (lower case, the host of the address
being requested, before redirects) shared by all jobs. A connection that transfers file
data takes a slot before its request and gives it back when its response ends. Waiting
for a slot stops when the job is paused, and the row shows "Waiting for a free
connection" (translated). Probes, speed tests and the site loaders' page and API requests
do not take slots. Video jobs are not affected.

**Where the settings live.** A "Settings" button in the main window opens a small native
dialog with the two choices. (Proposed; the theme choice stays in About.)

## Left to the builder

- Names and exact shapes of new functions, classes and `settings.ini` keys.
- How the "Retrying in N s" countdown reaches the window (for example, the queue's
  snapshot carrying the seconds left).
- Unit tests for the parts that need no network: the retry schedule and its reset, the
  validator choice, checkpoint save and load, the old-layout conversion, the
  downloads-at-once limit and the per-server counter. Whether to add a small loopback
  HTTP server for engine tests.
- Placement of the Settings button among the existing toolbar buttons.

## Calls made by the author (not decided by the user)

- The retry waits (5, 15, 60, 300 s) and the limit of 12 attempts.
- The 5-second checkpoint interval.
- Keeping the staging folder and its name.
- Converting old staging folders rather than finishing them with the old code.
- Connections per server: range 1 to 16, default 16, counted before redirects, and
  binding a single job as well as the total.
- Checking the validator on every 206 answer, and restarting once on one connection when
  it differs or the server answers 200.
- Honoring `Retry-After` as a minimum wait.
- A separate Settings dialog rather than adding to About.
