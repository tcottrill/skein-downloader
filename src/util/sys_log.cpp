/* =============================================================================
 * File: sys_log.cpp
 * Component: Asynchronous logging (threaded, non-blocking)
 *
 * Overview
 * --------
 * A lightweight, thread-safe logging system that formats messages on the caller
 * thread and performs all I/O on a dedicated background thread. Messages are
 * enqueued to a lock-protected queue and flushed to a logfile (and optionally
 * mirrored to stdout). The API is simple:
 *   - LogOpen("app.log") to start
 *   - LOG_DEBUG/LOG_INFO/LOG_ERROR(...) to write
 *   - LogClose() to flush and shut down
 *
 * Features
 * --------
 * - Background writer thread with condition-variable wakeups.
 * - Non-blocking call-site: minimal time spent under locks.
 * - Varargs formatting (printf-style) with file/function/line source tags.
 * - Compile-time timestamp toggle via LOG_WITH_TIMESTAMP.
 * - Optional console output (stdout; a console is allocated on Windows).
 * - Adjustable minimum level (Debug/Info/Warn/Error/Off).
 * - Messages written while no writer runs are kept up to a limit, then dropped.
 *
 * Threading Model
 * ---------------
 * - Public APIs are thread-safe.
 * - 'write(...)' formats on the caller thread, then enqueues a single string.
 * - A worker thread drains the queue and performs file/console output.
 * - LogClose() stops the worker, joins it, and flushes any remaining messages.
 *
 * Performance Notes
 * -----------------
 * - Formatting uses a fixed-size buffer per call (2 KB cap, safe-truncated).
 * - Each message is flushed immediately to keep logs crash-resilient.
 * - Console output is written by the same worker thread, in plain text.
 *
 * API Summary (see sys_log.h)
 * ---------------------------
 *   bool Log::open(const std::string& filename);
 *     Opens/creates the log file and starts the background thread.
 *
 *   void Log::close();
 *     Signals shutdown, joins the thread, flushes file/console, closes file.
 *
 *   void Log::setLevel(Log::Level level);
 *     Sets the minimum severity to emit (Debug, Info, Error, Off).
 *
 *   void Log::setConsoleOutputEnabled(bool enabled);
 *     Enables a Windows console and mirrors log output there.
 *
 *   void Log::write(Level lvl, const char* file, const char* func, int line,
 *                   const char* fmt, ...);
 *     Formats and enqueues a single message. Prefer the macros below so file,
 *     function, and line are captured automatically.
 *
 * Usage (typical)
 * ---------------
 *   LogOpen("game.log");
 *   Log::setLevel(Log::Level::Debug);
 *   Log::setConsoleOutputEnabled(true);   // optional
 *   LOG_INFO("Hello from %s", "GameEngine Alpha");
 *   LOG_ERROR("Could not open resource: %s", path.c_str());
 *   LogClose();
 *
 * Build / Platform
 * ----------------
 * - Uses the C++ standard library (thread, mutex, condition_variable, iostream).
 * - On Windows, enabling console output allocates a console (<windows.h>).
 *
 * Limitations
 * -----------
 * - Single destination file; no log rotation/retention policy built in.
 * - Messages longer than the internal buffer are safely truncated.
 *
 * ---------------------------------------------------------------------------
 * License (GPLv3):
 *   This file is part of GameEngine Alpha.
 *
 *   <Project Name> is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   <Project Name> is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with GameEngine Alpha.  If not, see <https://www.gnu.org/licenses/>.
 *
 *   Copyright (C) 2012-2025  Tim Cottrill
 *   SPDX-License-Identifier: GPL-3.0-or-later
 * ============================================================================= */

#include "sys_log.h"
#include <cstdarg>  // <-- Required for va_start, va_list, va_end
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <sstream>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <thread>
#include <atomic>
#include <iostream>
#include <iomanip>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#endif

static const char* baseName(const char* path) {
	// __FILE__ uses '\' under MSVC and '/' under GCC/Clang; take whichever
	// separator comes last so Linux logs show the file name, not the full path.
	const char* back = strrchr(path, '\\');
	const char* forward = strrchr(path, '/');
	const char* slash = forward > back ? forward : back;
	return slash ? slash + 1 : path;
}

namespace {
	FILE* stream = nullptr;
	std::mutex configMutex;
	std::atomic<Log::Level> currentLogLevel = Log::Level::Debug;
	// Mirrors each message to stdout (plain text). On by default: Linux runs show the log
	// in the terminal; a Windows GUI process has no console until one is allocated.
	std::atomic<bool> consoleOutput = true;

	std::mutex queueMutex;
	std::condition_variable queueCV;
	std::queue<std::string> messageQueue;
	std::thread logThread;
	// Changed only with queueMutex held, so a message is either queued for a running
	// worker or counted against the limit below, never left in a queue nobody drains.
	bool running = false;
	// Messages kept while no writer runs (before open, after close, or when the file
	// could not be opened). Beyond this they are dropped instead of growing without end.
	constexpr size_t kMaxPendingWithoutWriter = 1000;

	std::once_flag consoleInitFlag;

	const char* levelToString(Log::Level level) {
		switch (level) {
		case Log::Level::Debug: return "DEBUG";
		case Log::Level::Info:  return "INFO ";
		case Log::Level::Warn:  return "WARN ";
		case Log::Level::Error: return "ERROR";
		default:                return "UNKNOWN";
		}
	}

	std::string currentTimeString() {
		std::time_t now = std::time(nullptr);
		struct tm timeInfo;
		// Both are the thread-safe variant, but the argument order is
		// REVERSED between them: MSVC's localtime_s(tm*, time_t*) vs POSIX's
		// localtime_r(time_t*, tm*). Swapping them compiles cleanly and
		// produces garbage, so this is worth spelling out.
#ifdef _WIN32
		localtime_s(&timeInfo, &now);
#else
		localtime_r(&now, &timeInfo);
#endif
		char buffer[32];
		std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &timeInfo);
		return buffer;
	}

	void logWorker() {
		// messageQueue is only ever examined with queueMutex held; the file and
		// console writes below run with it released.
		std::unique_lock<std::mutex> lock(queueMutex);
		for (;;) {
			queueCV.wait(lock, [] {
				return !messageQueue.empty() || !running;
				});

			while (!messageQueue.empty()) {
				std::string msg = std::move(messageQueue.front());
				messageQueue.pop();
				lock.unlock();

				// Write to file
				if (stream) {
					std::fputs(msg.c_str(), stream);
					std::fflush(stream);
				}

				// Optional console output
				if (consoleOutput) {
					std::cout << msg;
				}

				lock.lock();
			}
			// Queue drained with the lock held, so nothing queued before
			// close() set running to false can be left behind.
			if (!running)
				return;
		}
	}

	void enqueueMessage(std::string&& msg) {
		{
			std::lock_guard<std::mutex> lock(queueMutex);
			if (!running && messageQueue.size() >= kMaxPendingWithoutWriter)
				return;
			messageQueue.push(std::move(msg));
		}
		queueCV.notify_one();
	}

	// Stops and joins the writer, then writes what is still queued. configMutex is held.
	void stopWriter() {
		{
			std::lock_guard<std::mutex> lock(queueMutex);
			running = false;
		}
		queueCV.notify_all();
		if (logThread.joinable())
			logThread.join();

		std::lock_guard<std::mutex> lock(queueMutex);
		while (!messageQueue.empty()) {
			const std::string& msg = messageQueue.front();
			if (stream)
				std::fputs(msg.c_str(), stream);
			if (consoleOutput)
				std::cout << msg;
			messageQueue.pop();
		}
		if (stream)
			std::fflush(stream);
	}
}

namespace Log {
	bool open(const std::string& filename) {
		std::lock_guard<std::mutex> lock(configMutex);

		// Opening again replaces the file: the old writer must finish first
		// (assigning over a joinable std::thread would terminate the program).
		stopWriter();
		if (stream) {
			std::fclose(stream);
			stream = nullptr;
		}

		// fopen_s is kept on Windows rather than switching both platforms to
		// plain fopen: MSVC emits C4996 ("consider using fopen_s") for fopen,
		// and this phase must not change the build's warning count.
#ifdef _WIN32
		errno_t err = fopen_s(&stream, filename.c_str(), "w");
		if (err != 0 || !stream)
			return false;
#else
		stream = std::fopen(filename.c_str(), "w");
		if (!stream)
			return false;
#endif

		{
			std::lock_guard<std::mutex> queueLock(queueMutex);
			running = true;
		}
		logThread = std::thread(logWorker);
		return true;
	}

	void close() {
		std::lock_guard<std::mutex> lock(configMutex);
		if (!logThread.joinable()) return;
		stopWriter();
		if (stream) {
			std::fclose(stream);
			stream = nullptr;
		}
	}

	void setLevel(Level level) {
		currentLogLevel = level;
	}

	void setConsoleOutputEnabled(bool enabled) {
		std::lock_guard<std::mutex> lock(configMutex);
		consoleOutput = enabled;

		if (enabled) {
			std::call_once(consoleInitFlag, []() {
#ifdef _WIN32
				// Windows-only: a GUI-subsystem process has no console until
				// it allocates one. Linux processes already have stdout, so
				// there is nothing to do there.
				AllocConsole();
				freopen_s((FILE**)stdout, "CONOUT$", "w", stdout);
				freopen_s((FILE**)stderr, "CONOUT$", "w", stderr);

				// Optional: set text mode
				_setmode(_fileno(stdout), _O_TEXT);
				_setmode(_fileno(stderr), _O_TEXT);

#endif
				std::cout.clear();
				std::cerr.clear();
				});
		}
	}

	void write(Level level, const char* file, const char* function, int line, const char* format, ...) {
		if (level < currentLogLevel)
			return;

		constexpr size_t kMaxMessageSize = 2048;
		char formatted[kMaxMessageSize];

		va_list args;
		va_start(args, format);
		// Plain vsnprintf, not MSVC's vsnprintf_s: it is standard C++11,
		// truncates and null-terminates exactly like _TRUNCATE asked for, and
		// MSVC does not deprecate it (unlike fopen above).
		vsnprintf(formatted, kMaxMessageSize, format, args);
		va_end(args);

		std::ostringstream oss;
#ifdef LOG_WITH_TIMESTAMP
		oss << "[" << currentTimeString() << "] ";
#endif
		oss << levelToString(level) << " ("
			<< baseName(file) << ":" << line << " " << function << ") - "
			<< formatted << "\n";

		// The writer thread copies it to the console too when that is enabled.
		enqueueMessage(std::move(oss).str());
	}
}