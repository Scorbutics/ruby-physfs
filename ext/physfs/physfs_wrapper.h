#ifndef PHYSFS_GEM_WRAPPER_H
#define PHYSFS_GEM_WRAPPER_H

// Minimal C++ facade over PhysFS — just the operations the Ruby surface
// and shim actually use. Keeps this gem free of any other engine dep.
//
// PhysFS itself is a process-global singleton: if the host process also
// uses another PhysFS wrapper (e.g., LiteCGSS's internal one for asset
// loading), they coexist on the same underlying mounts because PhysFS
// holds the mount table internally. Init is idempotent (PHYSFS_isInit gate).

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Forward-declared so this header doesn't have to drag in <physfs.h> for
// callers that only touch the path-based API.
struct PHYSFS_Io;

// Internal C++ namespace for this gem. Distinct from the C library's
// PHYSFS_ prefix so they coexist cleanly inside the same translation unit.
namespace physfs_gem {
	// Mount a zip / directory at the given mount point. prepend=true raises
	// priority above earlier mounts at the same point.
	void mount(const std::string& source, const std::string& mountPoint = "/", bool prepend = false);

	// Mount via a caller-provided PHYSFS_Io (e.g. a streaming decrypter).
	// PhysFS takes ownership: it will call io->destroy when the mount is
	// released. If PHYSFS_mountIo fails, this function calls io->destroy
	// itself before throwing — callers must not double-free.
	void mountIo(PHYSFS_Io* io, const std::string& fakeName,
	             const std::string& mountPoint = "/", bool prepend = false);

	void unmount(const std::string& source);

	// Set the writable native dir. Mounts it at priority 0 so written files
	// shadow archive entries on subsequent reads.
	void setWriteDir(const std::string& realDir);
	const std::string& getWriteDir();

	// Refcounted shutdown. Safe to call multiple times.
	void shutdown();

	bool exists(std::string_view path);
	bool isDirectory(std::string_view path);
	std::int64_t mtime(std::string_view path);  // 0 if missing
	std::int64_t fileSize(std::string_view path);  // -1 if missing / not a regular file
	std::vector<std::string> enumerate(std::string_view directory);

	// Read entire file into a buffer. Used by File.read / require shim paths
	// where the bytes need to outlive the read call.
	std::vector<char> loadFully(std::string_view path);
}

#endif
