#include "Shim.h"
#include "PhysFSGem.h"
#include "physfs_wrapper.h"
#include "RubyValueHelper.h"

#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>

// =============================================================================
// Design notes
// -----------------------------------------------------------------------------
// Every override below has the same shape:
//
//   1. Parse path arg.
//   2. resolveVirtualPath() applies any active virtual cwd.
//   3. Branch ONCE on physfs_gem (exists / isDirectory) — no rescue Exception.
//   4. Either VFS-handle the call or rb_call_super to the original method.
//
// The shim is registered as a Ruby module *prepended* to File / Dir / IO and
// Kernel — that's how rb_call_super reaches the original implementation
// without any Old_* method captures.
//
// Virtual cwd state is thread_local because Ruby threads share the GVL and
// can each have their own scoped Dir.chdir(block) state. physfs_gem itself
// stays a pure path-in/path-out API (no implicit cwd), so the C++ wrapper
// doesn't depend on Ruby semantics.
// =============================================================================

namespace {

	// ------------------------------------------------------------------
	// Activation state. The prepended modules are installed in the MRO
	// exactly once (Ruby has no rb_unprepend); each override checks
	// g_shim_active at the top and short-circuits to super when inactive.
	// ------------------------------------------------------------------

	bool g_modules_prepended = false;
	bool g_shim_active = false;

	// rb_call_super does NOT preserve keyword-arg semantics in Ruby 3+: callers
	// like Tempfile.create that invoke `File.open(path, flags, **opts)` lose
	// their kwargs through plain super, ending up with the kwargs Hash treated
	// as a positional Integer argument. shim_super_kw forwards the same way
	// the original caller invoked us (kwargs stay kwargs, positional stays
	// positional) by passing rb_keyword_given_p() as the kw_splat hint.
	inline VALUE shim_super_kw(int argc, VALUE* argv) {
		return rb_call_super_kw(argc, argv, rb_keyword_given_p());
	}

	#define SHIM_PASSTHROUGH_IF_INACTIVE() \
		do { if (!g_shim_active) return shim_super_kw(argc, argv); } while (0)

	// ------------------------------------------------------------------
	// Glob — segment-by-segment walking over the VFS, with each segment
	// matched via File.fnmatch? so per-segment semantics (dotfile exclusion,
	// FNM_* flags, backslash escapes, char classes) match Ruby exactly.
	//
	// We don't try to use File.fnmatch? on full paths: fnmatch's `**` does
	// NOT have Dir.glob's "zero-or-more directories" semantic, so the full-
	// path approach diverges on patterns like `**/`. Walking by segment
	// reproduces Dir.glob's recursive `**` faithfully, while delegating
	// character-level matching to Ruby for the simple segments.
	// ------------------------------------------------------------------

	bool rubyMatch(const std::string& pat, const std::string& name, int flags) {
		static const ID id_fnmatch = rb_intern("fnmatch?");
		const VALUE p = rb_str_new(pat.data(), static_cast<long>(pat.size()));
		const VALUE n = rb_str_new(name.data(), static_cast<long>(name.size()));
		return RTEST(rb_funcall(rb_cFile, id_fnmatch, 3, p, n, INT2FIX(flags)));
	}

	std::vector<std::string> splitPath(std::string_view p) {
		std::vector<std::string> out;
		std::string cur;
		for (const char c : p) {
			if (c == '/') {
				if (!cur.empty()) { out.push_back(std::move(cur)); cur.clear(); }
			} else {
				cur.push_back(c);
			}
		}
		if (!cur.empty()) out.push_back(std::move(cur));
		return out;
	}

	// Path-level brace expansion: `{a,b}/x` -> ["a/x", "b/x"]. Handles nested.
	// Done in C++ so per-segment fnmatch doesn't need FNM_EXTGLOB (which would
	// also enable other extensions we don't want at the segment level).
	std::vector<std::string> braceExpand(const std::string& pattern) {
		const auto open = pattern.find('{');
		if (open == std::string::npos) return { pattern };
		int depth = 1;
		std::size_t close = open + 1;
		for (; close < pattern.size() && depth > 0; ++close) {
			if (pattern[close] == '{') ++depth;
			else if (pattern[close] == '}') --depth;
		}
		if (depth != 0) return { pattern };
		--close;
		const std::string prefix = pattern.substr(0, open);
		const std::string body = pattern.substr(open + 1, close - open - 1);
		const std::string suffix = pattern.substr(close + 1);
		std::vector<std::string> alts;
		std::string current;
		int innerDepth = 0;
		for (const char c : body) {
			if (c == '{') ++innerDepth;
			else if (c == '}') --innerDepth;
			if (c == ',' && innerDepth == 0) {
				alts.push_back(current); current.clear();
			} else {
				current.push_back(c);
			}
		}
		alts.push_back(current);
		std::vector<std::string> out;
		for (const auto& a : alts) {
			for (auto& e : braceExpand(prefix + a + suffix)) {
				out.push_back(std::move(e));
			}
		}
		return out;
	}

	void emitMatch(const std::string& path, bool dir_only, VALUE out) {
		if (dir_only) {
			if (!physfs_gem::isDirectory(path)) return;
			VALUE s = rb_str_new(path.data(), static_cast<long>(path.size()));
			rb_ary_push(out, rb_str_plus(s, rb_str_new_cstr("/")));
		} else {
			rb_ary_push(out, rb_str_new(path.data(), static_cast<long>(path.size())));
		}
	}

	void globRecurse(const std::string& base,
	                 const std::vector<std::string>& parts,
	                 std::size_t idx,
	                 int flags,
	                 bool dir_only,
	                 VALUE out) {
		if (idx >= parts.size()) {
			if (!base.empty()) emitMatch(base, dir_only, out);
			return;
		}
		const std::string& seg = parts[idx];
		const std::string baseList = base.empty() ? std::string{ "/" } : base;
		const auto entries = physfs_gem::enumerate(baseList);

		if (seg == "**") {
			// `**` matches zero or more directories. Try the rest of the
			// pattern right here (zero dirs case)…
			globRecurse(base, parts, idx + 1, flags, dir_only, out);
			// …and recurse into every subdirectory keeping `**` in place.
			for (const auto& e : entries) {
				const auto child = base.empty() ? e : base + "/" + e;
				if (physfs_gem::isDirectory(child)) {
					globRecurse(child, parts, idx, flags, dir_only, out);
				}
			}
			return;
		}

		for (const auto& e : entries) {
			if (!rubyMatch(seg, e, flags)) continue;
			const auto child = base.empty() ? e : base + "/" + e;
			if (idx + 1 == parts.size()) {
				emitMatch(child, dir_only, out);
			} else if (physfs_gem::isDirectory(child)) {
				globRecurse(child, parts, idx + 1, flags, dir_only, out);
			}
		}
	}

	// ------------------------------------------------------------------
	// Helpers — small, single-purpose, used by every override.
	// ------------------------------------------------------------------

	thread_local std::string g_virtual_pwd;  // empty = inactive

	std::string toStdString(VALUE v) {
		Check_Type(v, T_STRING);
		return std::string{ RSTRING_PTR(v), static_cast<std::size_t>(RSTRING_LEN(v)) };
	}

	// Apply the virtual chdir state if the verbatim path doesn't exist on
	// the real filesystem. Mirrors what the legacy patch did with its
	// path_in_assets() helper, but in a single uniform call site.
	std::string resolveVirtualPath(VALUE rb_path) {
		std::string path = toStdString(rb_path);
		if (g_virtual_pwd.empty() || path.empty() || path[0] == '/') {
			return path;
		}
		std::error_code ec;
		if (std::filesystem::exists(path, ec)) {
			return path;
		}
		return g_virtual_pwd + "/" + path;
	}

	enum class OpenIntent { Read, Write };

	OpenIntent parseMode(VALUE rb_mode) {
		if (NIL_P(rb_mode)) return OpenIntent::Read;
		if (!RB_TYPE_P(rb_mode, T_STRING)) return OpenIntent::Read;
		const std::string m = toStdString(rb_mode);
		for (char c : m) {
			if (c == 'w' || c == 'a' || c == '+') return OpenIntent::Write;
		}
		return OpenIntent::Read;
	}

	VALUE readVfsAsRubyString(const std::string& path) {
		const auto buf = physfs_gem::loadFully(path);
		return rb_str_new(buf.data(), static_cast<long>(buf.size()));
	}

	VALUE forceUtf8(VALUE str) {
		static ID id_force_encoding = rb_intern("force_encoding");
		rb_funcall(str, id_force_encoding, 1, rb_str_new_cstr("UTF-8"));
		return str;
	}

	// ------------------------------------------------------------------
	// File overrides — uniform shape.
	// ------------------------------------------------------------------

	VALUE rb_File_exist_q(int argc, VALUE* argv, VALUE /*self*/) {
		SHIM_PASSTHROUGH_IF_INACTIVE();
		VALUE path; rb_scan_args(argc, argv, "1", &path);
		if (physfs_gem::exists(resolveVirtualPath(path))) return Qtrue;
		return shim_super_kw(argc, argv);
	}

	VALUE rb_File_directory_q(int argc, VALUE* argv, VALUE /*self*/) {
		SHIM_PASSTHROUGH_IF_INACTIVE();
		VALUE path; rb_scan_args(argc, argv, "1", &path);
		if (physfs_gem::isDirectory(resolveVirtualPath(path))) return Qtrue;
		return shim_super_kw(argc, argv);
	}

	VALUE rb_File_file_q(int argc, VALUE* argv, VALUE /*self*/) {
		SHIM_PASSTHROUGH_IF_INACTIVE();
		VALUE path; rb_scan_args(argc, argv, "1", &path);
		const auto p = resolveVirtualPath(path);
		if (physfs_gem::exists(p) && !physfs_gem::isDirectory(p)) return Qtrue;
		return shim_super_kw(argc, argv);
	}

	VALUE rb_File_mtime(int argc, VALUE* argv, VALUE /*self*/) {
		SHIM_PASSTHROUGH_IF_INACTIVE();
		VALUE path; rb_scan_args(argc, argv, "1", &path);
		const auto p = resolveVirtualPath(path);
		if (physfs_gem::exists(p)) return rb_time_new(physfs_gem::mtime(p), 0);
		return shim_super_kw(argc, argv);
	}

	// File.read / binread / readlines: only the simple "read whole file" form
	// is VFS-backed. length/offset variants fall through to super.
	VALUE rb_File_read(int argc, VALUE* argv, VALUE /*self*/) {
		SHIM_PASSTHROUGH_IF_INACTIVE();
		if (argc != 1) return shim_super_kw(argc, argv);
		const auto p = resolveVirtualPath(argv[0]);
		if (!physfs_gem::exists(p)) return shim_super_kw(argc, argv);
		return forceUtf8(readVfsAsRubyString(p));
	}

	VALUE rb_File_binread(int argc, VALUE* argv, VALUE /*self*/) {
		SHIM_PASSTHROUGH_IF_INACTIVE();
		if (argc != 1) return shim_super_kw(argc, argv);
		const auto p = resolveVirtualPath(argv[0]);
		if (!physfs_gem::exists(p)) return shim_super_kw(argc, argv);
		return readVfsAsRubyString(p);
	}

	VALUE rb_File_readlines(int argc, VALUE* argv, VALUE /*self*/) {
		SHIM_PASSTHROUGH_IF_INACTIVE();
		if (argc != 1) return shim_super_kw(argc, argv);
		const auto p = resolveVirtualPath(argv[0]);
		if (!physfs_gem::exists(p)) return shim_super_kw(argc, argv);
		VALUE bytes = forceUtf8(readVfsAsRubyString(p));
		return rb_funcall(bytes, rb_intern("split"), 1, rb_str_new_cstr("\n"));
	}

	struct OpenCtx { VALUE io; };
	VALUE openYieldBody(VALUE arg) {
		return rb_yield(reinterpret_cast<OpenCtx*>(arg)->io);
	}
	VALUE openCloseEnsure(VALUE arg) {
		rb_funcall(reinterpret_cast<OpenCtx*>(arg)->io, rb_intern("close"), 0);
		return Qnil;
	}

	// Read intent → StringIO over VFS bytes (or super if not in VFS).
	// Write intent → straight to super (real fopen, supports rename/fsync).
	//
	// Only the simple File.open(path, "r"|"rb") form is VFS-handled. Any
	// other shape — numeric mode, kwargs (encoding:, etc.), perm bits,
	// extra positional args — bails to super so callers like Tempfile that
	// rely on the full Ruby File.open contract keep working unchanged.
	VALUE rb_File_open(int argc, VALUE* argv, VALUE /*self*/) {
		SHIM_PASSTHROUGH_IF_INACTIVE();
		if (argc < 1 || argc > 2) return shim_super_kw(argc, argv);
		if (rb_keyword_given_p()) return shim_super_kw(argc, argv);

		const VALUE mode_v = (argc >= 2) ? argv[1] : Qnil;
		if (!NIL_P(mode_v) && !RB_TYPE_P(mode_v, T_STRING)) {
			return shim_super_kw(argc, argv);  // numeric flags etc.
		}
		if (parseMode(mode_v) == OpenIntent::Write) return shim_super_kw(argc, argv);

		const auto p = resolveVirtualPath(argv[0]);
		if (!physfs_gem::exists(p)) return shim_super_kw(argc, argv);

		VALUE bytes = readVfsAsRubyString(p);
		VALUE rb_StringIO = rb_const_get(rb_cObject, rb_intern("StringIO"));
		VALUE io = rb_funcall(rb_StringIO, rb_intern("new"), 1, bytes);
		if (!rb_block_given_p()) return io;
		OpenCtx ctx{ io };
		return rb_ensure(openYieldBody, reinterpret_cast<VALUE>(&ctx),
		                 openCloseEnsure, reinterpret_cast<VALUE>(&ctx));
	}

	// File.copy_stream(src, dst): if src is in VFS, read it and write dst
	// natively. Else super (which handles real-FS to real-FS copies).
	VALUE rb_File_copy_stream(int argc, VALUE* argv, VALUE /*self*/) {
		SHIM_PASSTHROUGH_IF_INACTIVE();
		if (rb_keyword_given_p()) return shim_super_kw(argc, argv);
		VALUE src, dst;
		rb_scan_args(argc, argv, "2*", &src, &dst);
		const auto sp = resolveVirtualPath(src);
		if (!physfs_gem::exists(sp)) return shim_super_kw(argc, argv);
		const auto buf = physfs_gem::loadFully(sp);
		const auto dst_path = toStdString(dst);
		std::error_code ec;
		const auto parent = std::filesystem::path(dst_path).parent_path();
		if (!parent.empty()) std::filesystem::create_directories(parent, ec);
		std::FILE* f = std::fopen(dst_path.c_str(), "wb");
		if (!f) rb_raise(rb_eIOError, "Cannot open '%s' for writing", dst_path.c_str());
		const auto written = std::fwrite(buf.data(), 1, buf.size(), f);
		std::fclose(f);
		return LL2NUM(static_cast<long long>(written));
	}

	// ------------------------------------------------------------------
	// Dir overrides
	// ------------------------------------------------------------------

	VALUE rb_Dir_chdir(int argc, VALUE* argv, VALUE /*self*/) {
		SHIM_PASSTHROUGH_IF_INACTIVE();
		if (rb_keyword_given_p()) return shim_super_kw(argc, argv);
		VALUE path;
		rb_scan_args(argc, argv, "01", &path);
		if (NIL_P(path)) return shim_super_kw(argc, argv);
		const std::string p = toStdString(path);
		std::error_code ec;
		if (std::filesystem::is_directory(p, ec)) return shim_super_kw(argc, argv);

		// Virtual chdir — scoped if a block was given, otherwise persistent.
		const std::string old = g_virtual_pwd;
		g_virtual_pwd = p;
		if (!rb_block_given_p()) return Qnil;

		struct ChdirCtx { std::string previous; };
		ChdirCtx ctx{ old };
		return rb_ensure(
			[](VALUE) -> VALUE { return rb_yield(Qnil); }, Qnil,
			[](VALUE arg) -> VALUE {
				g_virtual_pwd = reinterpret_cast<ChdirCtx*>(arg)->previous;
				return Qnil;
			},
			reinterpret_cast<VALUE>(&ctx)
		);
	}

	// Dir.[] / Dir.glob — merge VFS results with real-FS results so callers
	// that mix archive and on-disk paths see both. Pattern matching delegates
	// to File.fnmatch? (Ruby) so semantics — dotfile exclusion, backslash
	// escapes, FNM_* flags — are guaranteed-equivalent to native Dir.glob.
	//
	// Kwargs short-circuit: if the caller passes any keyword args (notably
	// `base:`, which scopes the search to a specific real-FS directory),
	// we MUST NOT inject VFS results — the caller is asking for that one
	// location only. Just super-forward.
	VALUE rb_Dir_glob(int argc, VALUE* argv, VALUE /*self*/) {
		SHIM_PASSTHROUGH_IF_INACTIVE();
		if (argc < 1) return shim_super_kw(argc, argv);
		if (rb_keyword_given_p()) return shim_super_kw(argc, argv);

		const auto resolved = resolveVirtualPath(argv[0]);
		const VALUE adjusted = rb_str_new(resolved.data(), static_cast<long>(resolved.size()));
		const int user_flags = (argc >= 2 && FIXNUM_P(argv[1])) ? NUM2INT(argv[1]) : 0;

		VALUE out = PhysFSShim_Glob(adjusted, user_flags);
		const VALUE native = shim_super_kw(argc, argv);
		if (RB_TYPE_P(native, T_ARRAY)) rb_ary_concat(out, native);
		return rb_funcall(out, rb_intern("uniq"), 0);
	}

	VALUE rb_Dir_entries(int argc, VALUE* argv, VALUE /*self*/) {
		SHIM_PASSTHROUGH_IF_INACTIVE();
		// Kwargs (encoding:) → super-forward without touching argv. Even
		// reading the kwargs Hash via rb_scan_args "1*" can leave Ruby's
		// internal call-info state mismatched with our forwarded super,
		// which manifests as a "generic_fields_tbl" BUG much later.
		if (rb_keyword_given_p()) return shim_super_kw(argc, argv);
		VALUE path;
		rb_scan_args(argc, argv, "1*", &path);
		const auto p = resolveVirtualPath(path);
		if (!physfs_gem::isDirectory(p)) return shim_super_kw(argc, argv);
		const auto entries = physfs_gem::enumerate(p);
		VALUE out = rb_ary_new_capa(static_cast<long>(entries.size()) + 2);
		rb_ary_push(out, rb_str_new_cstr("."));
		rb_ary_push(out, rb_str_new_cstr(".."));
		for (const auto& e : entries) {
			rb_ary_push(out, rb_str_new(e.data(), static_cast<long>(e.size())));
		}
		return out;
	}

	VALUE rb_Dir_exist_q(int argc, VALUE* argv, VALUE /*self*/) {
		SHIM_PASSTHROUGH_IF_INACTIVE();
		VALUE path; rb_scan_args(argc, argv, "1", &path);
		if (physfs_gem::isDirectory(resolveVirtualPath(path))) return Qtrue;
		return shim_super_kw(argc, argv);
	}

	// ------------------------------------------------------------------
	// IO override
	// ------------------------------------------------------------------

	VALUE rb_IO_copy_stream(int argc, VALUE* argv, VALUE self) {
		return rb_File_copy_stream(argc, argv, self);
	}

	// ------------------------------------------------------------------
	// Kernel#require / require_relative
	// ------------------------------------------------------------------

	VALUE callRequireSuper(VALUE name) {
		return rb_call_super_kw(1, &name, rb_keyword_given_p());
	}

	VALUE rb_Kernel_require(VALUE /*self*/, VALUE name) {
		if (!g_shim_active) return callRequireSuper(name);

		// Try the original loader first — it covers gems, .so, stdlib.
		int state = 0;
		const VALUE result = rb_protect(callRequireSuper, name, &state);
		if (!state) return result;
		const VALUE err = rb_errinfo();
		rb_set_errinfo(Qnil);
		if (!rb_obj_is_kind_of(err, rb_eLoadError)) rb_exc_raise(err);

		// Try VFS as the fallback. Append .rb if the caller didn't.
		std::string n = toStdString(name);
		if (n.size() < 3 || n.substr(n.size() - 3) != ".rb") n += ".rb";
		if (!physfs_gem::exists(n)) rb_exc_raise(err);

		// Honor $LOADED_FEATURES so re-require is idempotent.
		const VALUE loaded = rb_gv_get("$LOADED_FEATURES");
		const VALUE n_val = rb_str_new(n.data(), static_cast<long>(n.size()));
		if (RTEST(rb_funcall(loaded, rb_intern("include?"), 1, n_val))) return Qfalse;

		const auto buf = physfs_gem::loadFully(n);
		const VALUE code = rb_str_new(buf.data(), static_cast<long>(buf.size()));
		rb_ary_push(loaded, n_val);  // mark loaded BEFORE eval to prevent recursive reload
		const VALUE binding = rb_const_get(rb_cObject, rb_intern("TOPLEVEL_BINDING"));
		rb_funcall(binding, rb_intern("eval"), 2, code, n_val);
		return Qtrue;
	}

	VALUE rb_Kernel_require_relative(VALUE self, VALUE name) {
		if (!g_shim_active) return rb_call_super_kw(1, &name, rb_keyword_given_p());

		// Resolve relative to the caller's __FILE__ — same as Ruby's stdlib.
		const VALUE caller_loc = rb_funcall(rb_mKernel, rb_intern("caller_locations"), 2,
		                                    INT2FIX(1), INT2FIX(1));
		std::string base;
		if (RB_TYPE_P(caller_loc, T_ARRAY) && RARRAY_LEN(caller_loc) > 0) {
			const VALUE loc = RARRAY_AREF(caller_loc, 0);
			const VALUE path = rb_funcall(loc, rb_intern("absolute_path"), 0);
			if (!NIL_P(path) && RB_TYPE_P(path, T_STRING)) {
				base = std::filesystem::path(toStdString(path)).parent_path().string();
			}
		}
		const std::string rel = toStdString(name);
		const std::string full = base.empty() ? rel : (base + "/" + rel);
		const VALUE expanded = rb_str_new(full.data(), static_cast<long>(full.size()));
		return rb_Kernel_require(self, expanded);
	}

}  // namespace

// =============================================================================
// Install / activate — two-stage:
//
//   1. ensureModulesPrepended() runs ONCE on first activation. It prepends
//      our shim modules into File / Dir / IO / Kernel's MRO. This step
//      cannot be undone — Ruby has no rb_unprepend_module.
//
//   2. g_shim_active is a runtime flag flipped by Activate / Deactivate.
//      Every override checks it at the top and short-circuits to super
//      when inactive. This is the "lifecycle" knob driven by mount/unmount.
//
// Cost when inactive: one bool load + one branch + a rb_call_super that
// resolves to the stock Ruby method — i.e. exactly what would happen
// without the shim at all.
// =============================================================================

namespace {
	void ensureModulesPrepended() {
		if (g_modules_prepended) return;

		// Ensure StringIO is available for the File.open block-form path.
		rb_require("stringio");

		// File / Dir / IO are class-method overrides → prepend to the singleton class.
		VALUE m_FileShim = rb_define_module_under(rb_mPhysFS, "FileShim");
		rb_define_method(m_FileShim, "exist?",      _rbf rb_File_exist_q,      -1);
		rb_define_method(m_FileShim, "directory?",  _rbf rb_File_directory_q,  -1);
		rb_define_method(m_FileShim, "file?",       _rbf rb_File_file_q,       -1);
		rb_define_method(m_FileShim, "mtime",       _rbf rb_File_mtime,        -1);
		rb_define_method(m_FileShim, "read",        _rbf rb_File_read,         -1);
		rb_define_method(m_FileShim, "binread",     _rbf rb_File_binread,      -1);
		rb_define_method(m_FileShim, "readlines",   _rbf rb_File_readlines,    -1);
		rb_define_method(m_FileShim, "open",        _rbf rb_File_open,         -1);
		rb_define_method(m_FileShim, "copy_stream", _rbf rb_File_copy_stream,  -1);
		rb_prepend_module(rb_singleton_class(rb_cFile), m_FileShim);

		VALUE m_DirShim = rb_define_module_under(rb_mPhysFS, "DirShim");
		rb_define_method(m_DirShim, "chdir",   _rbf rb_Dir_chdir,    -1);
		rb_define_method(m_DirShim, "[]",      _rbf rb_Dir_glob,     -1);
		rb_define_method(m_DirShim, "glob",    _rbf rb_Dir_glob,     -1);
		rb_define_method(m_DirShim, "entries", _rbf rb_Dir_entries,  -1);
		rb_define_method(m_DirShim, "exist?",  _rbf rb_Dir_exist_q,  -1);
		rb_prepend_module(rb_singleton_class(rb_cDir), m_DirShim);

		VALUE m_IOShim = rb_define_module_under(rb_mPhysFS, "IOShim");
		rb_define_method(m_IOShim, "copy_stream", _rbf rb_IO_copy_stream, -1);
		rb_prepend_module(rb_singleton_class(rb_cIO), m_IOShim);

		// Kernel#require / require_relative are INSTANCE methods on Kernel.
		VALUE m_KernelShim = rb_define_module_under(rb_mPhysFS, "KernelShim");
		rb_define_method(m_KernelShim, "require",          _rbf rb_Kernel_require,          1);
		rb_define_method(m_KernelShim, "require_relative", _rbf rb_Kernel_require_relative, 1);
		rb_prepend_module(rb_mKernel, m_KernelShim);

		g_modules_prepended = true;
	}

	VALUE rb_PhysFS_InstallShim(VALUE /*self*/) {
		return PhysFSShim_Activate() ? Qtrue : Qfalse;
	}

	VALUE rb_PhysFS_UninstallShim(VALUE /*self*/) {
		return PhysFSShim_Deactivate() ? Qtrue : Qfalse;
	}

	VALUE rb_PhysFS_ShimInstalledQ(VALUE /*self*/) {
		return g_shim_active ? Qtrue : Qfalse;
	}
}

bool PhysFSShim_Activate() {
	ensureModulesPrepended();
	if (g_shim_active) return false;
	g_shim_active = true;
	return true;
}

bool PhysFSShim_Deactivate() {
	if (!g_shim_active) return false;
	g_shim_active = false;
	return true;
}

void PhysFSShim_InvalidatePathCache() {
	// Reserved hook: no caching at the moment, but kept in the C++ ABI so
	// Bindings.cpp's mount/unmount call it unconditionally. If a future
	// optimization caches the path enumeration, this is where to clear it.
}

VALUE PhysFSShim_Glob(VALUE pattern, int extra_flags) {
	Check_Type(pattern, T_STRING);
	std::string pat{ RSTRING_PTR(pattern), static_cast<std::size_t>(RSTRING_LEN(pattern)) };

	// Detect and strip trailing slash; remember dir-only emission.
	bool dir_only = !pat.empty() && pat.back() == '/';
	if (dir_only) pat.pop_back();

	VALUE out = rb_ary_new();
	for (const auto& expanded : braceExpand(pat)) {
		const auto parts = splitPath(expanded);
		if (parts.empty()) continue;
		globRecurse("", parts, 0, extra_flags, dir_only, out);
	}
	return rb_funcall(out, rb_intern("uniq"), 0);
}

void PhysFSShim_DefineRubyMethods() {
	rb_define_module_function(rb_mPhysFS, "install_shim!",   _rbf rb_PhysFS_InstallShim,    0);
	rb_define_module_function(rb_mPhysFS, "uninstall_shim!", _rbf rb_PhysFS_UninstallShim,  0);
	rb_define_module_function(rb_mPhysFS, "shim_installed?", _rbf rb_PhysFS_ShimInstalledQ, 0);
}
