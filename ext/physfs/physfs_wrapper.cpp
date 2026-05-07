#include "physfs_wrapper.h"

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

namespace physfs_gem {

	void mount(const std::string& source, const std::string& mountPoint, bool prepend) {
		ensureInit();
		const int appendFlag = prepend ? 0 : 1;
		if (PHYSFS_mount(source.c_str(), mountPoint.c_str(), appendFlag) == 0) {
			throwPhysFSError("PhysFS mount failed for '" + source + "' at '" + mountPoint + "'");
		}
	}

	void unmount(const std::string& source) {
		if (!PHYSFS_isInit()) return;
		if (PHYSFS_unmount(source.c_str()) == 0) {
			throwPhysFSError("PhysFS unmount failed for '" + source + "'");
		}
	}

	void setWriteDir(const std::string& realDir) {
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
	}

	const std::string& getWriteDir() { return g_writeDir; }

	void shutdown() {
		if (!PHYSFS_isInit()) return;
		PHYSFS_deinit();
		g_writeDir.clear();
	}

	bool exists(std::string_view path) {
		return PHYSFS_isInit() && PHYSFS_exists(toString(path).c_str());
	}

	bool isDirectory(std::string_view path) {
		if (!PHYSFS_isInit()) return false;
		PHYSFS_Stat raw{};
		if (PHYSFS_stat(toString(path).c_str(), &raw) == 0) return false;
		return raw.filetype == PHYSFS_FILETYPE_DIRECTORY;
	}

	std::int64_t mtime(std::string_view path) {
		if (!PHYSFS_isInit()) return 0;
		PHYSFS_Stat raw{};
		if (PHYSFS_stat(toString(path).c_str(), &raw) == 0) return 0;
		return raw.modtime;
	}

	std::int64_t fileSize(std::string_view path) {
		if (!PHYSFS_isInit()) return -1;
		PHYSFS_Stat raw{};
		if (PHYSFS_stat(toString(path).c_str(), &raw) == 0) return -1;
		if (raw.filetype != PHYSFS_FILETYPE_REGULAR) return -1;
		return raw.filesize;
	}

	std::vector<std::string> enumerate(std::string_view directory) {
		if (!PHYSFS_isInit()) return {};
		std::vector<std::string> result;
		char** list = PHYSFS_enumerateFiles(toString(directory).c_str());
		if (!list) return result;
		for (char** it = list; *it != nullptr; ++it) result.emplace_back(*it);
		PHYSFS_freeList(list);
		return result;
	}

	std::vector<char> loadFully(std::string_view path) {
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
	}
}
