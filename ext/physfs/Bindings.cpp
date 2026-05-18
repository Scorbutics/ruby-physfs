#include "Bindings.h"
#include "PhysFSGem.h"
#include "PhysFSLock.h"
#include "RubyIoBridge.h"
#include "Shim.h"
#include "physfs_wrapper.h"
#include "RubyValueHelper.h"

#include <stdexcept>
#include <string>

namespace {
	// Tracks how many archives are mounted via this Ruby surface so we can
	// auto-activate the shim on the first mount and auto-deactivate on the
	// last unmount. PhysFS itself has its own mount table; this counter
	// exists strictly for the Ruby shim lifecycle.
	int g_mount_count = 0;

	std::string toStdString(VALUE v) {
		Check_Type(v, T_STRING);
		return std::string{ RSTRING_PTR(v), static_cast<std::size_t>(RSTRING_LEN(v)) };
	}

	VALUE rb_PhysFS_Mount(int argc, VALUE* argv, VALUE) {
		VALUE source, mountPoint, prepend;
		rb_scan_args(argc, argv, "12", &source, &mountPoint, &prepend);
		const auto src = toStdString(source);
		const auto mp = NIL_P(mountPoint) ? std::string{ "/" } : toStdString(mountPoint);
		const bool prep = !NIL_P(prepend) && RTEST(prepend);
		try {
			physfs_gem::mount(src, mp, prep);
		} catch (const std::exception& e) {
			rb_raise(rb_ePhysFSError, "%s", e.what());
		}
		// Auto-activate the shim on the first mount. Idempotent for further
		// mounts. Embedders that called install_shim! explicitly are unaffected.
		if (g_mount_count++ == 0) PhysFSShim_Activate();
		PhysFSShim_InvalidatePathCache();
		return Qnil;
	}

	VALUE rb_PhysFS_MountIo(int argc, VALUE* argv, VALUE) {
		VALUE io_obj, fakeName, mountPoint, prepend;
		rb_scan_args(argc, argv, "22", &io_obj, &fakeName, &mountPoint, &prepend);
		Check_Type(fakeName, T_STRING);
		const auto fn   = toStdString(fakeName);
		const auto mp   = NIL_P(mountPoint) ? std::string{ "/" } : toStdString(mountPoint);
		const bool prep = !NIL_P(prepend) && RTEST(prepend);

		PHYSFS_Io* io = physfs_gem::CreatePhysFSIoFromRubyObject(io_obj);
		if (!io) {
			rb_raise(rb_ePhysFSError, "Failed to allocate PhysFS_Io bridge");
		}
		try {
			physfs_gem::mountIo(io, fn, mp, prep);
		} catch (const std::exception& e) {
			// mountIo already destroyed `io` on failure, so don't double-free.
			rb_raise(rb_ePhysFSError, "%s", e.what());
		}
		// First mount activates the File/Dir/IO/require shim, same as
		// PhysFS.mount above. Keeps the two surfaces interchangeable from
		// the shim-lifecycle point of view.
		if (g_mount_count++ == 0) PhysFSShim_Activate();
		PhysFSShim_InvalidatePathCache();
		return Qnil;
	}

	VALUE rb_PhysFS_Unmount(VALUE, VALUE source) {
		try {
			physfs_gem::unmount(toStdString(source));
		} catch (const std::exception& e) {
			rb_raise(rb_ePhysFSError, "%s", e.what());
		}
		// Auto-deactivate when the last archive goes away. The prepended
		// modules stay in the MRO (Ruby has no rb_unprepend), but every
		// override short-circuits to super while inactive.
		if (--g_mount_count <= 0) {
			g_mount_count = 0;
			PhysFSShim_Deactivate();
		}
		PhysFSShim_InvalidatePathCache();
		return Qnil;
	}

	VALUE rb_PhysFS_SetWriteDir(VALUE, VALUE dir) {
		try {
			physfs_gem::setWriteDir(toStdString(dir));
		} catch (const std::exception& e) {
			rb_raise(rb_ePhysFSError, "%s", e.what());
		}
		return dir;
	}

	VALUE rb_PhysFS_GetWriteDir(VALUE) {
		const auto& d = physfs_gem::getWriteDir();
		return d.empty() ? Qnil : rb_str_new(d.data(), static_cast<long>(d.size()));
	}

	VALUE rb_PhysFS_Exist(VALUE, VALUE path) {
		return physfs_gem::exists(toStdString(path)) ? Qtrue : Qfalse;
	}

	VALUE rb_PhysFS_IsDirectory(VALUE, VALUE path) {
		return physfs_gem::isDirectory(toStdString(path)) ? Qtrue : Qfalse;
	}

	VALUE rb_PhysFS_Mtime(VALUE, VALUE path) {
		return LL2NUM(physfs_gem::mtime(toStdString(path)));
	}

	VALUE rb_PhysFS_Read(VALUE, VALUE path) {
		try {
			const auto buf = physfs_gem::loadFully(toStdString(path));
			return rb_str_new(buf.data(), static_cast<long>(buf.size()));
		} catch (const std::exception& e) {
			rb_raise(rb_ePhysFSError, "%s", e.what());
		}
		return Qnil;
	}

	VALUE rb_PhysFS_Enumerate(VALUE, VALUE directory) {
		const auto list = physfs_gem::enumerate(toStdString(directory));
		VALUE out = rb_ary_new_capa(static_cast<long>(list.size()));
		for (const auto& f : list) {
			rb_ary_push(out, rb_str_new(f.data(), static_cast<long>(f.size())));
		}
		return out;
	}

	VALUE rb_PhysFS_Glob(int argc, VALUE* argv, VALUE) {
		VALUE pattern, flags_v;
		rb_scan_args(argc, argv, "11", &pattern, &flags_v);
		Check_Type(pattern, T_STRING);
		const int flags = NIL_P(flags_v) ? 0 : NUM2INT(flags_v);
		// Use the same matcher as the Dir.glob shim so users get one consistent
		// semantic — exactly equivalent to Ruby's Dir.glob via File.fnmatch?.
		return PhysFSShim_Glob(pattern, flags);
	}

	// Returns the gem-wide reentrant Monitor that serializes every PhysFS
	// entry point. Exposed so other gems (LiteRGSS / LiteCGSS) that also
	// call into PhysFS directly can share the same lock — without it, the
	// GVL-vs-stateLock inversion described in PhysFSLock.h re-emerges.
	VALUE rb_PhysFS_Monitor(VALUE) {
		return physfs_gem::getMonitor();
	}
}

void PhysFSGem_DefineModuleMethods() {
	rb_define_module_function(rb_mPhysFS, "mount",           _rbf rb_PhysFS_Mount,         -1);
	rb_define_module_function(rb_mPhysFS, "mount_io",        _rbf rb_PhysFS_MountIo,       -1);
	rb_define_module_function(rb_mPhysFS, "unmount",         _rbf rb_PhysFS_Unmount,        1);
	rb_define_module_function(rb_mPhysFS, "write_dir=",      _rbf rb_PhysFS_SetWriteDir,    1);
	rb_define_module_function(rb_mPhysFS, "write_dir",       _rbf rb_PhysFS_GetWriteDir,    0);
	rb_define_module_function(rb_mPhysFS, "exist?",          _rbf rb_PhysFS_Exist,          1);
	rb_define_module_function(rb_mPhysFS, "directory?",      _rbf rb_PhysFS_IsDirectory,    1);
	rb_define_module_function(rb_mPhysFS, "mtime",           _rbf rb_PhysFS_Mtime,          1);
	rb_define_module_function(rb_mPhysFS, "read",            _rbf rb_PhysFS_Read,           1);
	rb_define_module_function(rb_mPhysFS, "enumerate",       _rbf rb_PhysFS_Enumerate,      1);
	rb_define_module_function(rb_mPhysFS, "glob",            _rbf rb_PhysFS_Glob,          -1);
	rb_define_module_function(rb_mPhysFS, "monitor",         _rbf rb_PhysFS_Monitor,        0);

	// Define PhysFS.install_shim! / .uninstall_shim! / .shim_installed?
	// — the transparent File / Dir / IO / Kernel#require overrides are
	// auto-activated by mount and auto-deactivated by the last unmount,
	// but embedders can also force-toggle them explicitly.
	PhysFSShim_DefineRubyMethods();
}
