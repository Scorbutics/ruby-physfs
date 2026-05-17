#ifndef PHYSFS_GEM_PHYSFS_LOCK_H
#define PHYSFS_GEM_PHYSFS_LOCK_H

// One reentrant Ruby Monitor that serializes EVERY call into the PhysFS C
// library. The serialization is needed because:
//
//   - PhysFS itself has an internal recursive mutex (`stateLock`) protecting
//     its globals (mount table, write_dir, etc.), but that mutex uses
//     `pthread_self()` for ownership. Under MRI Ruby with multiple Ruby
//     Threads, each Ruby Thread gets its own pthread_t — so that's fine in
//     isolation.
//
//   - However, an archive's `io->read` callback re-enters Ruby
//     (RubyIoBridge::io_read -> rb_funcall("read")). If that Ruby method
//     internally releases the GVL (e.g. its inner `IO#pread` does a
//     blocking syscall and releases the GVL while waiting), another Ruby
//     Thread can wake up and call into PhysFS. That second thread will
//     then block on `stateLock` while the first thread is waiting on the
//     GVL to be returned. Classic lock-inversion deadlock.
//
//   - The fix that actually works under MRI: serialize at the *Ruby* level
//     so the second thread never reaches `stateLock` while the first is
//     mid-callback. The Monitor's `#enter` is GVL-aware (it parks the Ruby
//     Thread instead of busy-waiting), so cross-thread coordination works
//     the way Ruby expects.
//
// Reentrancy:
//   We use `Monitor` (not `Mutex`) because the callback chain can recurse
//   back into PhysFS on the same Ruby Thread:
//     PhysFS.read -> physfs_gem::loadFully -> PHYSFS_openRead ->
//     ZIP archiver -> io->duplicate -> Ruby duplicate() -> ...
//   If any link calls a shimmed `File.*`, that re-enters `physfs_gem::*`.
//   Monitor allows the same thread to re-acquire — `Mutex` would raise
//   `ThreadError`.

#include <ruby.h>

#include <exception>
#include <functional>

namespace physfs_gem {

	// Initialise the module-level Monitor. Must be called once from
	// Init_physfs, after `rb_require("monitor")`.
	void initMonitor();

	// For tests / introspection only.
	VALUE getMonitor();

	namespace detail {
		// Type-erased body wrapper. We use std::function so the templated
		// `with_physfs_lock` below can carry an arbitrary callable through
		// `rb_ensure`'s VALUE argument without templating the C-trampoline
		// callbacks.
		struct LockBody {
			std::function<void()> fn;
			std::exception_ptr err;
		};

		VALUE bodyTrampoline(VALUE arg);
		VALUE ensureTrampoline(VALUE arg);
	}

	// Acquire the gem-wide PhysFS monitor, run `fn`, then release the
	// monitor. Safe across:
	//   - Ruby exceptions raised inside `fn` (cleanup via rb_ensure;
	//     the Ruby exception is re-raised after release).
	//   - C++ exceptions thrown by `fn` (captured inside the trampoline
	//     and rethrown after release).
	template <class Fn>
	auto with_physfs_lock(Fn&& fn) -> decltype(fn()) {
		using ReturnType = decltype(fn());
		rb_funcall(getMonitor(), rb_intern("enter"), 0);

		if constexpr (std::is_void_v<ReturnType>) {
			detail::LockBody body{ [&] { fn(); }, nullptr };
			rb_ensure(detail::bodyTrampoline, reinterpret_cast<VALUE>(&body),
			          detail::ensureTrampoline, Qnil);
			if (body.err) std::rethrow_exception(body.err);
		} else {
			ReturnType result{};
			detail::LockBody body{ [&] { result = fn(); }, nullptr };
			rb_ensure(detail::bodyTrampoline, reinterpret_cast<VALUE>(&body),
			          detail::ensureTrampoline, Qnil);
			if (body.err) std::rethrow_exception(body.err);
			return result;
		}
	}

}

#endif
