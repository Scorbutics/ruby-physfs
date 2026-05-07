#include "RubyIoBridge.h"

#include <cstring>
#include <new>

#include <physfs.h>

namespace {

	// Per-PHYSFS_Io state: the Ruby object plus a tracking flag for the GC
	// registration so destroy() is idempotent.
	struct RubyIoOpaque {
		VALUE rubyValue;
		bool  gcRegistered;
	};

	// Args bundle for rb_protect, since rb_protect's callback signature is
	// `VALUE(VALUE)` and we need to smuggle through receiver + method id +
	// argv.
	struct CallArgs {
		VALUE recv;
		ID    mid;
		int   argc;
		VALUE argv[2];
	};

	VALUE protectedFuncall(VALUE rawArgs) {
		auto* a = reinterpret_cast<CallArgs*>(rawArgs);
		return rb_funcallv(a->recv, a->mid, a->argc, a->argv);
	}

	// Calls `recv.send(mid, *argv)` under rb_protect. If a Ruby exception
	// is raised, returns Qnil and *raised is set true; otherwise *raised is
	// false. The Ruby exception is cleared from the thread-local errinfo
	// either way — we deliberately discard it because there's no PhysFS
	// channel to report it through.
	VALUE safeFuncall(VALUE recv, ID mid, int argc, const VALUE* argv, bool& raised) {
		CallArgs a;
		a.recv = recv;
		a.mid  = mid;
		a.argc = argc;
		for (int i = 0; i < argc && i < 2; ++i) a.argv[i] = argv[i];

		int state = 0;
		VALUE result = rb_protect(protectedFuncall, reinterpret_cast<VALUE>(&a), &state);
		if (state) {
			rb_set_errinfo(Qnil);
			raised = true;
			return Qnil;
		}
		raised = false;
		return result;
	}

	// PHYSFS_Io callbacks ---------------------------------------------------

	PHYSFS_sint64 io_read(PHYSFS_Io* io, void* buf, PHYSFS_uint64 len) {
		auto* op = static_cast<RubyIoOpaque*>(io->opaque);
		const VALUE arg = ULL2NUM(len);
		bool raised = false;
		VALUE result = safeFuncall(op->rubyValue, rb_intern("read"), 1, &arg, raised);
		if (raised) return -1;
		if (NIL_P(result)) return 0;
		if (!RB_TYPE_P(result, T_STRING)) return -1;

		auto got = static_cast<PHYSFS_uint64>(RSTRING_LEN(result));
		if (got > len) got = len;
		std::memcpy(buf, RSTRING_PTR(result), static_cast<size_t>(got));
		return static_cast<PHYSFS_sint64>(got);
	}

	PHYSFS_sint64 io_write(PHYSFS_Io*, const void*, PHYSFS_uint64) {
		// Read-only.
		return -1;
	}

	int io_seek(PHYSFS_Io* io, PHYSFS_uint64 offset) {
		auto* op = static_cast<RubyIoOpaque*>(io->opaque);
		const VALUE arg = ULL2NUM(offset);
		bool raised = false;
		safeFuncall(op->rubyValue, rb_intern("seek"), 1, &arg, raised);
		return raised ? 0 : 1;
	}

	PHYSFS_sint64 io_tell(PHYSFS_Io* io) {
		auto* op = static_cast<RubyIoOpaque*>(io->opaque);
		bool raised = false;
		VALUE result = safeFuncall(op->rubyValue, rb_intern("tell"), 0, nullptr, raised);
		if (raised || NIL_P(result)) return -1;
		return static_cast<PHYSFS_sint64>(NUM2LL(result));
	}

	PHYSFS_sint64 io_length(PHYSFS_Io* io) {
		auto* op = static_cast<RubyIoOpaque*>(io->opaque);
		bool raised = false;
		VALUE result = safeFuncall(op->rubyValue, rb_intern("length"), 0, nullptr, raised);
		if (raised || NIL_P(result)) return -1;
		return static_cast<PHYSFS_sint64>(NUM2LL(result));
	}

	PHYSFS_Io* io_duplicate(PHYSFS_Io* io) {
		auto* op = static_cast<RubyIoOpaque*>(io->opaque);
		bool raised = false;
		VALUE result = safeFuncall(op->rubyValue, rb_intern("duplicate"), 0, nullptr, raised);
		if (raised || NIL_P(result)) return nullptr;
		return physfs_gem::CreatePhysFSIoFromRubyObject(result);
	}

	int io_flush(PHYSFS_Io* io) {
		auto* op = static_cast<RubyIoOpaque*>(io->opaque);
		// flush is optional on the Ruby side. respond_to? lets us no-op
		// silently when the IO doesn't implement it (common for read-only
		// streams like EpsaStream).
		const VALUE flushSym = ID2SYM(rb_intern("flush"));
		const VALUE responds = rb_funcall(op->rubyValue, rb_intern("respond_to?"), 1, flushSym);
		if (!RTEST(responds)) return 1;
		bool raised = false;
		safeFuncall(op->rubyValue, rb_intern("flush"), 0, nullptr, raised);
		return raised ? 0 : 1;
	}

	void io_destroy(PHYSFS_Io* io) {
		if (!io) return;
		auto* op = static_cast<RubyIoOpaque*>(io->opaque);
		if (op) {
			if (op->gcRegistered) {
				bool raised = false;
				safeFuncall(op->rubyValue, rb_intern("close"), 0, nullptr, raised);
				rb_gc_unregister_address(&op->rubyValue);
				op->gcRegistered = false;
			}
			delete op;
			io->opaque = nullptr;
		}
		delete io;
	}
}

namespace physfs_gem {

	PHYSFS_Io* CreatePhysFSIoFromRubyObject(VALUE rubyIo) {
		auto* op = new (std::nothrow) RubyIoOpaque{ rubyIo, false };
		if (!op) return nullptr;

		// rb_gc_register_address pins the VALUE so the GC won't reclaim the
		// Ruby object while PhysFS holds the PHYSFS_Io. Paired release in
		// io_destroy.
		rb_gc_register_address(&op->rubyValue);
		op->gcRegistered = true;

		auto* io = new (std::nothrow) PHYSFS_Io;
		if (!io) {
			rb_gc_unregister_address(&op->rubyValue);
			delete op;
			return nullptr;
		}

		io->version   = 0;
		io->opaque    = op;
		io->read      = io_read;
		io->write     = io_write;
		io->seek      = io_seek;
		io->tell      = io_tell;
		io->length    = io_length;
		io->duplicate = io_duplicate;
		io->flush     = io_flush;
		io->destroy   = io_destroy;
		return io;
	}
}
