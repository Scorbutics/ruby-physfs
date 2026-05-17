#include "PhysFSLock.h"

namespace physfs_gem {

	namespace {
		// Pinned VALUE holding the singleton Monitor instance. Pinned with
		// rb_gc_register_address in initMonitor() so the GC doesn't reclaim
		// it across calls.
		VALUE g_monitor = Qnil;
	}

	void initMonitor() {
		if (g_monitor != Qnil) return;
		// `monitor` is part of Ruby's standard library — `require` brings
		// the `Monitor` class into Object. Init_physfs runs early in the
		// VM boot, so we cannot assume the constant is already present.
		rb_require("monitor");
		const VALUE Monitor = rb_const_get(rb_cObject, rb_intern("Monitor"));
		g_monitor = rb_funcall(Monitor, rb_intern("new"), 0);
		rb_gc_register_address(&g_monitor);
	}

	VALUE getMonitor() {
		return g_monitor;
	}

	namespace detail {

		VALUE bodyTrampoline(VALUE arg) {
			auto* body = reinterpret_cast<LockBody*>(arg);
			// Capture C++ exceptions so they can be rethrown AFTER the
			// rb_ensure unwinds the Monitor. If we let a C++ exception
			// propagate out of this function, rb_ensure won't run our
			// ensure-callback and the Monitor would stay held forever.
			try {
				body->fn();
			} catch (...) {
				body->err = std::current_exception();
			}
			return Qnil;
		}

		VALUE ensureTrampoline(VALUE /*arg*/) {
			// `Monitor#exit` decrements the per-thread re-entry counter and
			// releases the underlying mutex when it hits zero. On a thread
			// that holds the monitor (which we always are, since
			// `with_physfs_lock` always pairs enter/exit), this never
			// raises. We deliberately do not wrap this in rb_protect — if
			// it ever DID raise, that's a serious bug we want surfaced
			// loudly rather than silently dropping the unlock.
			rb_funcall(getMonitor(), rb_intern("exit"), 0);
			return Qnil;
		}

	}
}
