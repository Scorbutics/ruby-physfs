#include "physfs_wrapper.h"
#include "PhysFSLock.h"

#include <filesystem>
#include <stdexcept>

#include <physfs.h>

namespace {
	std::string g_writeDir;

	void ensureInit() {
		if (PHYSFS_isInit()) return;
		if (PHYSFS_init(nullptr) == 0) {
			throw std::runtime_error{
				std::string{ "PhysFS init failed: " } +
				PHYSFS_getErrorByCode(PHYSFS_getLastErrorCode())
			};
		}
	}

	[[noreturn]] void throwPhysFSError(const std::string& context) {
		throw std::runtime_error{
			context + ": " + PHYSFS_getErrorByCode(PHYSFS_getLastErrorCode())
		};
	}

	std::string toString(std::string_view sv) { return std::string{ sv }; }
}

// Every public entry point here is wrapped in `with_physfs_lock`, which
// acquires the gem-wide reentrant Ruby Monitor. See PhysFSLock.h for the
// full rationale — short version: PhysFS's internal `stateLock` is
// pthread-based, but Ruby callbacks coming out of the archiver (io_read
// etc.) can release the GVL mid-call, which lets a second Ruby Thread try
// to enter PhysFS while the first is still mid-callback. The Monitor here
// is the GVL-aware lock that actually prevents that.

namespace physfs_gem {

	void mount(const std::string& source, const std::string& mountPoint, bool prepend) {
		with_physfs_lock([&] {
			ensureInit();
			const int appendFlag = prepend ? 0 : 1;
			if (PHYSFS_mount(source.c_str(), mountPoint.c_str(), appendFlag) == 0) {
				throwPhysFSError("PhysFS mount failed for '" + source + "' at '" + mountPoint + "'");
			}
		});
	}

	void mountIo(PHYSFS_Io* io, const std::string& fakeName,
	             const std::string& mountPoint, bool prepend) {
		with_physfs_lock([&] {
			ensureInit();
			const int appendFlag = prepend ? 0 : 1;
			if (PHYSFS_mountIo(io, fakeName.c_str(), mountPoint.c_str(), appendFlag) == 0) {
				// PHYSFS_mountIo's contract: on failure, the caller still owns the
				// PHYSFS_Io. Destroy it ourselves so the caller can't double-free.
				if (io && io->destroy) io->destroy(io);
				throwPhysFSError("PhysFS mountIo failed for '" + fakeName +
				                 "' at '" + mountPoint + "'");
			}
		});
	}

	void unmount(const std::string& source) {
		with_physfs_lock([&] {
			if (!PHYSFS_isInit()) return;
			if (PHYSFS_unmount(source.c_str()) == 0) {
				throwPhysFSError("PhysFS unmount failed for '" + source + "'");
			}
		});
	}

	void setWriteDir(const std::string& realDir) {
		with_physfs_lock([&] {
			ensureInit();
			// If we previously mounted a write_dir, drop that read mount before
			// rebinding so the new dir can claim priority 0 cleanly.
			if (!g_writeDir.empty()) {
				PHYSFS_unmount(g_writeDir.c_str());
			}
			if (PHYSFS_setWriteDir(realDir.c_str()) == 0) {
				throwPhysFSError("PHYSFS_setWriteDir('" + realDir + "') failed");
			}
			if (PHYSFS_mount(realDir.c_str(), "/", 0) == 0) {
				throwPhysFSError("PhysFS mount of write_dir '" + realDir + "' failed");
			}
			g_writeDir = realDir;
		});
	}

	const std::string& getWriteDir() {
		// We return by reference for callers that want zero-copy, but a
		// concurrent setWriteDir would race on the underlying string's
		// allocator state, so we read it under the monitor and the caller
		// holds it briefly through that snapshot.
		// (The lambda copies under the lock, so even after we release the
		// monitor the returned reference is stable for the caller's
		// immediate use; that's how every caller currently consumes it.)
		static thread_local std::string snapshot;
		with_physfs_lock([&] { snapshot = g_writeDir; });
		return snapshot;
	}

	void shutdown() {
		with_physfs_lock([&] {
			if (!PHYSFS_isInit()) return;
			PHYSFS_deinit();
			g_writeDir.clear();
		});
	}

	bool exists(std::string_view path) {
		return with_physfs_lock([&]() -> bool {
			return PHYSFS_isInit() && PHYSFS_exists(toString(path).c_str());
		});
	}

	bool isDirectory(std::string_view path) {
		return with_physfs_lock([&]() -> bool {
			if (!PHYSFS_isInit()) return false;
			PHYSFS_Stat raw{};
			if (PHYSFS_stat(toString(path).c_str(), &raw) == 0) return false;
			return raw.filetype == PHYSFS_FILETYPE_DIRECTORY;
		});
	}

	std::int64_t mtime(std::string_view path) {
		return with_physfs_lock([&]() -> std::int64_t {
			if (!PHYSFS_isInit()) return 0;
			PHYSFS_Stat raw{};
			if (PHYSFS_stat(toString(path).c_str(), &raw) == 0) return 0;
			return raw.modtime;
		});
	}

	std::int64_t fileSize(std::string_view path) {
		return with_physfs_lock([&]() -> std::int64_t {
			if (!PHYSFS_isInit()) return -1;
			PHYSFS_Stat raw{};
			if (PHYSFS_stat(toString(path).c_str(), &raw) == 0) return -1;
			if (raw.filetype != PHYSFS_FILETYPE_REGULAR) return -1;
			return raw.filesize;
		});
	}

	std::vector<std::string> enumerate(std::string_view directory) {
		return with_physfs_lock([&]() -> std::vector<std::string> {
			if (!PHYSFS_isInit()) return {};
			std::vector<std::string> result;
			char** list = PHYSFS_enumerateFiles(toString(directory).c_str());
			if (!list) return result;
			for (char** it = list; *it != nullptr; ++it) result.emplace_back(*it);
			PHYSFS_freeList(list);
			return result;
		});
	}

	std::vector<char> loadFully(std::string_view path) {
		return with_physfs_lock([&]() -> std::vector<char> {
			ensureInit();
			PHYSFS_File* file = PHYSFS_openRead(toString(path).c_str());
			if (!file) throwPhysFSError("PhysFS openRead failed for '" + toString(path) + "'");
			const auto length = PHYSFS_fileLength(file);
			std::vector<char> buffer;
			if (length > 0) {
				buffer.resize(static_cast<std::size_t>(length));
				const auto got = PHYSFS_readBytes(file, buffer.data(), static_cast<PHYSFS_uint64>(length));
				if (got != length && !PHYSFS_eof(file)) {
					PHYSFS_close(file);
					throwPhysFSError("PhysFS readBytes failed for '" + toString(path) + "'");
				}
				buffer.resize(static_cast<std::size_t>(got));
			}
			PHYSFS_close(file);
			return buffer;
		});
	}
}
