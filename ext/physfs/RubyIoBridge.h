#ifndef PHYSFS_GEM_RUBY_IO_BRIDGE_H
#define PHYSFS_GEM_RUBY_IO_BRIDGE_H

#include "RubyValueHelper.h"

struct PHYSFS_Io;

namespace physfs_gem {

	// Build a PHYSFS_Io that delegates every callback to a Ruby IO-like
	// object via rb_funcall. The returned PHYSFS_Io takes a GC-rooted
	// reference to `rubyIo`; that reference is released when destroy() is
	// called (which PhysFS itself does on unmount).
	//
	// Required Ruby methods on the object:
	//   read(n)    -> ASCII-8BIT String of up to n bytes (empty/nil at EOF)
	//   seek(off)  -> any (return value ignored)
	//   tell       -> Integer current offset
	//   length     -> Integer total length
	//   duplicate  -> a fresh IO-like object satisfying this protocol
	//   close      -> any (return value ignored); called from destroy()
	//
	// Optional:
	//   flush      -> any; treated as success even if not defined
	//
	// Ruby exceptions raised inside any callback are swallowed and surfaced
	// to PhysFS as the normal error code (-1 / 0 / nullptr). PhysFS then
	// reports a generic I/O error to its caller. Since we can only report
	// a single error code per callback, we deliberately drop the Ruby
	// exception — PhysFS has no facility for richer error info anyway.
	PHYSFS_Io* CreatePhysFSIoFromRubyObject(VALUE rubyIo);
}

#endif
