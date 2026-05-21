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

	// PhysFS rejects any path containing a "." or ".." segment via
	// PHYSFS_ERR_BAD_FILENAME, so idiomatic Ruby paths like "./Game.rb"
	// silently miss archive lookups. Collapse those segments before
	// consulting physfs_gem. A bare "." is preserved verbatim so callers
	// like Dir.entries(".") fall through to the real filesystem instead
	// of being rerouted to the archive root.
	//
	// Application code (notably the pokemonsdk loader) also builds
	// absolute paths via File.expand_path against the project root and
	// then requires them — a real-FS-shaped string for content that
	// actually lives in the archive. If the result lives under the
	// configured PhysFS.write_dir, strip that prefix so the lookup hits
	// the archive instead of the non-existent real-FS location. Mirrors
	// what the legacy ruby_physfs_patch.rb did via path_in_assets.
	std::string normalizePhysFSPath(const std::string& path) {
		if (path.empty() || path == ".") return path;
		std::string out = std::filesystem::path(path).lexically_normal().generic_string();
		if (out == ".") out = path;
		if (!out.empty() && out[0] == '/') {
			const auto& root = physfs_gem::getWriteDir();
			if (!root.empty() && out.size() > root.size() &&
				out.compare(0, root.size(), root) == 0 &&
				(root.back() == '/' || out[root.size()] == '/')) {
				out.erase(0, root.size());
				while (!out.empty() && out[0] == '/') out.erase(0, 1);
			}
		}
		return out;
	}

	// Apply the virtual chdir state if the verbatim path doesn't exist on
	// the real filesystem. Mirrors what the legacy patch did with its
	// path_in_assets() helper, but in a single uniform call site.
	std::string resolveVirtualPath(VALUE rb_path) {
		std::string path = toStdString(rb_path);
		if (!path.empty() && path[0] != '/' && !g_virtual_pwd.empty()) {
			std::error_code ec;
			if (!std::filesystem::exists(path, ec)) {
				path = g_virtual_pwd + "/" + path;
			}
		}
		return normalizePhysFSPath(path);
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
	// File / Dir / IO overrides — uniform shape.
	//
	// Every override below follows the same checklist before doing any
	// VFS work, and the order matters:
	//
	//   1. SHIM_PASSTHROUGH_IF_INACTIVE — short-circuit when the shim is
	//      not active (e.g. unmounted), preserving zero-overhead semantics.
	//   2. rb_keyword_given_p() → super-forward. Always check this FIRST,
	//      before reading argv in any way.
	//   3. argc bounds check → super-forward when the call shape is
	//      something we don't VFS-handle (extra args, etc.).
	//   4. RB_TYPE_P(argv[i], T_STRING) → super-forward on non-string
	//      args. We do NOT call to_str / rb_check_string_type — that
	//      would dispatch user code in a frame that's still mid-override,
	//      and it would also let exotic argv shapes reach our VFS path.
	//   5. Only after all the above: read argv directly (NEVER via
	//      rb_scan_args) and branch on physfs_gem::exists / isDirectory.
	//
	// Why no rb_scan_args: when invoked from a prepended-module override,
	// rb_scan_args mutates Ruby's per-frame call-info / cd state, and a
	// subsequent rb_call_super_kw can then forward an inconsistent frame
	// to the parent method. The corruption surfaces randomly, often many
	// calls later, as the "Object is missing entry in generic_fields_tbl"
	// internal BUG. Reading argv directly avoids the trigger entirely.
	// ------------------------------------------------------------------

	VALUE rb_File_exist_q(int argc, VALUE* argv, VALUE /*self*/) {
		SHIM_PASSTHROUGH_IF_INACTIVE();
		if (rb_keyword_given_p()) return shim_super_kw(argc, argv);
		if (argc != 1 || !RB_TYPE_P(argv[0], T_STRING)) return shim_super_kw(argc, argv);
		if (physfs_gem::exists(resolveVirtualPath(argv[0]))) return Qtrue;
		return shim_super_kw(argc, argv);
	}

	VALUE rb_File_directory_q(int argc, VALUE* argv, VALUE /*self*/) {
		SHIM_PASSTHROUGH_IF_INACTIVE();
		if (rb_keyword_given_p()) return shim_super_kw(argc, argv);
		if (argc != 1 || !RB_TYPE_P(argv[0], T_STRING)) return shim_super_kw(argc, argv);
		if (physfs_gem::isDirectory(resolveVirtualPath(argv[0]))) return Qtrue;
		return shim_super_kw(argc, argv);
	}

	VALUE rb_File_file_q(int argc, VALUE* argv, VALUE /*self*/) {
		SHIM_PASSTHROUGH_IF_INACTIVE();
		if (rb_keyword_given_p()) return shim_super_kw(argc, argv);
		if (argc != 1 || !RB_TYPE_P(argv[0], T_STRING)) return shim_super_kw(argc, argv);
		const auto p = resolveVirtualPath(argv[0]);
		if (physfs_gem::exists(p) && !physfs_gem::isDirectory(p)) return Qtrue;
		return shim_super_kw(argc, argv);
	}

	VALUE rb_File_mtime(int argc, VALUE* argv, VALUE /*self*/) {
		SHIM_PASSTHROUGH_IF_INACTIVE();
		if (rb_keyword_given_p()) return shim_super_kw(argc, argv);
		if (argc != 1 || !RB_TYPE_P(argv[0], T_STRING)) return shim_super_kw(argc, argv);
		const auto p = resolveVirtualPath(argv[0]);
		if (physfs_gem::exists(p)) return rb_time_new(physfs_gem::mtime(p), 0);
		return shim_super_kw(argc, argv);
	}

	// File.size(path) — byte count via PHYSFS_stat (no file body read).
	// Directories and missing entries fall through to super so the real-FS
	// behavior (Errno::ENOENT for missing, EISDIR for directories) is
	// preserved when the path isn't a regular VFS file.
	VALUE rb_File_size(int argc, VALUE* argv, VALUE /*self*/) {
		SHIM_PASSTHROUGH_IF_INACTIVE();
		if (rb_keyword_given_p()) return shim_super_kw(argc, argv);
		if (argc != 1 || !RB_TYPE_P(argv[0], T_STRING)) return shim_super_kw(argc, argv);
		const auto p = resolveVirtualPath(argv[0]);
		const auto sz = physfs_gem::fileSize(p);
		if (sz < 0) return shim_super_kw(argc, argv);
		return LL2NUM(static_cast<long long>(sz));
	}

	// File.read / binread / readlines: only the simple "read whole file" form
	// is VFS-backed. length/offset variants fall through to super.
	VALUE rb_File_read(int argc, VALUE* argv, VALUE /*self*/) {
		SHIM_PASSTHROUGH_IF_INACTIVE();
		if (rb_keyword_given_p()) return shim_super_kw(argc, argv);
		if (argc != 1 || !RB_TYPE_P(argv[0], T_STRING)) return shim_super_kw(argc, argv);
		const auto p = resolveVirtualPath(argv[0]);
		if (!physfs_gem::exists(p)) return shim_super_kw(argc, argv);
		return forceUtf8(readVfsAsRubyString(p));
	}

	VALUE rb_File_binread(int argc, VALUE* argv, VALUE /*self*/) {
		SHIM_PASSTHROUGH_IF_INACTIVE();
		if (rb_keyword_given_p()) return shim_super_kw(argc, argv);
		if (argc != 1 || !RB_TYPE_P(argv[0], T_STRING)) return shim_super_kw(argc, argv);
		const auto p = resolveVirtualPath(argv[0]);
		if (!physfs_gem::exists(p)) return shim_super_kw(argc, argv);
		return readVfsAsRubyString(p);
	}

	VALUE rb_File_readlines(int argc, VALUE* argv, VALUE /*self*/) {
		SHIM_PASSTHROUGH_IF_INACTIVE();
		if (rb_keyword_given_p()) return shim_super_kw(argc, argv);
		if (argc != 1 || !RB_TYPE_P(argv[0], T_STRING)) return shim_super_kw(argc, argv);
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
	// Keyword args are extracted from argv (last slot, when
	// rb_keyword_given_p() is true) and the read path is still taken so
	// archive-bundled files keep working when callers pass `encoding:` or
	// other IO kwargs. Motivated by stdlib `CSV.open`, which does
	//   File.open(filename, mode, **file_opts)
	// internally — before this, the shim's blanket "bail on kwargs"
	// short-circuit meant every CSV read fell through to native fopen,
	// which then ENOENT'd on archive-only assets like PSDK's
	// Data/Text/Dialogs/<id>.csv.
	//
	// We honour `encoding:` by force_encoding'ing the StringIO's backing
	// string; other kwargs (binmode:, autoclose:, newline:, perm bits)
	// don't apply to StringIO and are silently dropped. Anything that
	// genuinely needs the real File contract — numeric mode flags,
	// non-String first arg, write/append/update modes — still bails to
	// super so Tempfile et al. keep working unchanged.
	VALUE rb_File_open(int argc, VALUE* argv, VALUE /*self*/) {
		SHIM_PASSTHROUGH_IF_INACTIVE();

		// Peel off the kwargs hash if it was passed via `**`; everything
		// before it is the positional argv we evaluate below.
		VALUE kwargs = Qnil;
		int positional_argc = argc;
		if (rb_keyword_given_p() && argc >= 1) {
			const VALUE last = argv[argc - 1];
			if (RB_TYPE_P(last, T_HASH)) {
				kwargs = last;
				positional_argc = argc - 1;
			}
		}

		if (positional_argc < 1 || positional_argc > 2) return shim_super_kw(argc, argv);
		if (!RB_TYPE_P(argv[0], T_STRING)) return shim_super_kw(argc, argv);

		const VALUE mode_v = (positional_argc >= 2) ? argv[1] : Qnil;
		if (!NIL_P(mode_v) && !RB_TYPE_P(mode_v, T_STRING)) {
			return shim_super_kw(argc, argv);  // numeric flags etc.
		}
		if (parseMode(mode_v) == OpenIntent::Write) return shim_super_kw(argc, argv);

		const auto p = resolveVirtualPath(argv[0]);
		if (!physfs_gem::exists(p)) return shim_super_kw(argc, argv);

		VALUE bytes = readVfsAsRubyString(p);
		if (!NIL_P(kwargs)) {
			VALUE encoding = rb_hash_aref(kwargs, ID2SYM(rb_intern("encoding")));
			if (!NIL_P(encoding)) {
				// File.open accepts pseudo-encoding directives that
				// String#force_encoding does not, notably the `bom|<name>`
				// prefix (which tells the real IO layer to consume a BOM and
				// then treat the remainder as <name>). Strip BOM bytes if
				// present and pass the suffix to force_encoding. Stdlib CSV
				// hits this path with `bom|utf-8` on every read.
				if (RB_TYPE_P(encoding, T_STRING)) {
					const char* cstr = StringValueCStr(encoding);
					if (strncmp(cstr, "bom|", 4) == 0) {
						const long len = RSTRING_LEN(bytes);
						const char* bs = RSTRING_PTR(bytes);
						if (len >= 3 &&
						    static_cast<unsigned char>(bs[0]) == 0xEF &&
						    static_cast<unsigned char>(bs[1]) == 0xBB &&
						    static_cast<unsigned char>(bs[2]) == 0xBF) {
							bytes = rb_str_new(bs + 3, len - 3);
						}
						encoding = rb_str_new_cstr(cstr + 4);
					}
				}
				rb_funcall(bytes, rb_intern("force_encoding"), 1, encoding);
			}
		}
		VALUE rb_StringIO = rb_const_get(rb_cObject, rb_intern("StringIO"));
		VALUE io = rb_funcall(rb_StringIO, rb_intern("new"), 1, bytes);
		if (!rb_block_given_p()) return io;
		OpenCtx ctx{ io };
		return rb_ensure(openYieldBody, reinterpret_cast<VALUE>(&ctx),
		                 openCloseEnsure, reinterpret_cast<VALUE>(&ctx));
	}

	// File.new(path, mode) — same VFS-vs-super decision as File.open, but
	// File.new never takes a block. Returning a StringIO duck-types as the
	// IO interface that downstream code (Marshal.load(io), io.read, io.pos=)
	// actually exercises. Write/update modes ('w', 'a', 'rb+', '+', numeric
	// flags, kwargs, etc.) all super-forward to the real File constructor.
	//
	// Motivated by Pokémon SDK's Yuki::VD reader, which does
	//   @file = File.new(filename, 'rb')
	//   Marshal.load(@file)
	// against archive-bundled .dat files.
	VALUE rb_File_new(int argc, VALUE* argv, VALUE /*self*/) {
		SHIM_PASSTHROUGH_IF_INACTIVE();
		if (rb_keyword_given_p()) return shim_super_kw(argc, argv);
		if (argc < 1 || argc > 2) return shim_super_kw(argc, argv);
		if (!RB_TYPE_P(argv[0], T_STRING)) return shim_super_kw(argc, argv);

		const VALUE mode_v = (argc >= 2) ? argv[1] : Qnil;
		if (!NIL_P(mode_v) && !RB_TYPE_P(mode_v, T_STRING)) {
			return shim_super_kw(argc, argv);
		}
		if (parseMode(mode_v) == OpenIntent::Write) return shim_super_kw(argc, argv);

		const auto p = resolveVirtualPath(argv[0]);
		if (!physfs_gem::exists(p)) return shim_super_kw(argc, argv);

		VALUE bytes = readVfsAsRubyString(p);
		VALUE rb_StringIO = rb_const_get(rb_cObject, rb_intern("StringIO"));
		return rb_funcall(rb_StringIO, rb_intern("new"), 1, bytes);
	}

	// File.copy_stream(src, dst): if src is in VFS, read it and write dst
	// natively. Else super (which handles real-FS to real-FS copies).
	VALUE rb_File_copy_stream(int argc, VALUE* argv, VALUE /*self*/) {
		SHIM_PASSTHROUGH_IF_INACTIVE();
		if (rb_keyword_given_p()) return shim_super_kw(argc, argv);
		if (argc < 2) return shim_super_kw(argc, argv);
		if (!RB_TYPE_P(argv[0], T_STRING) || !RB_TYPE_P(argv[1], T_STRING)) {
			return shim_super_kw(argc, argv);
		}
		const auto sp = resolveVirtualPath(argv[0]);
		if (!physfs_gem::exists(sp)) return shim_super_kw(argc, argv);
		const auto buf = physfs_gem::loadFully(sp);
		const auto dst_path = toStdString(argv[1]);
		std::error_code ec;
		const auto parent = std::filesystem::path(dst_path).parent_path();
		if (!parent.empty()) std::filesystem::create_directories(parent, ec);
		std::FILE* f = std::fopen(dst_path.c_str(), "wb");
		if (!f) rb_raise(rb_eIOError, "Cannot open '%s' for writing", dst_path.c_str());
		const auto written = std::fwrite(buf.data(), 1, buf.size(), f);
		std::fclose(f);
		return LL2NUM(static_cast<long long>(written));
	}

	// File.binwrite(path, string [, offset] [, **opts]):
	// Native filesystem write that ENOENTs when the parent path doesn't
	// exist. Our own Dir.exist? / File.directory? shims report `true` for
	// any path resolvable through ANY mount — including the read-only
	// archive — which makes the common
	//
	//     mkdir(*dirname.split('/')) unless Dir.exist?(dirname)
	//     File.binwrite(filename, contents)
	//
	// pattern skip the actual native mkdir whenever the archive happens to
	// shadow that directory. The subsequent native binwrite then ENOENTs
	// against the empty writeDir-side parent.
	//
	// Fix it once for every caller: walk every parent component via
	// std::filesystem::create_directories (which is idempotent — already-
	// existing components hit EEXIST under the hood and are ignored)
	// before super-forwarding to native binwrite. Stdlib uses for binwrite
	// (Tempfile.create rotation, Marshal.dump-to-disk patterns) keep
	// working unchanged because create_directories is a no-op on already-
	// existing trees.
	VALUE rb_File_binwrite(int argc, VALUE* argv, VALUE /*self*/) {
		SHIM_PASSTHROUGH_IF_INACTIVE();
		if (argc >= 1 && RB_TYPE_P(argv[0], T_STRING)) {
			const std::string path = toStdString(argv[0]);
			const auto parent = std::filesystem::path(path).parent_path();
			if (!parent.empty() && parent != ".") {
				std::error_code ec;
				std::filesystem::create_directories(parent, ec);
				// Ignore errors here on purpose — if the directory really
				// can't be created the super call below will surface a
				// proper Errno::* with the exact reason.
			}
		}
		return shim_super_kw(argc, argv);
	}

	// File.delete(*paths):
	// Native delete raises Errno::ENOENT when the path doesn't exist on
	// the real filesystem. With archives mounted, callers can reasonably
	// hold a path that resolves through the shim (via a read-only mount)
	// but has no writeDir-side native presence — and PSDK's
	// ScriptLoad.rb#start does exactly that for `pokemonsdk/scripts/{
	// mega_script.deflate, scripts.dat}` during cache-invalidation. From
	// the writable side's perspective those files don't exist anyway, so
	// dropping the delete is the consistent answer.
	//
	// Strategy: filter out arguments that aren't on the native FS but ARE
	// in PhysFS (i.e. read-only mount paths). Whatever's left we forward
	// to super, preserving the standard ENOENT-for-truly-missing-paths
	// behaviour. Returning the number of paths actually delegated keeps
	// the contract of File.delete close to what callers expect (Ruby's
	// docs say it returns the number of files deleted; super does that).
	VALUE rb_File_delete(int argc, VALUE* argv, VALUE /*self*/) {
		SHIM_PASSTHROUGH_IF_INACTIVE();

		std::vector<VALUE> forward;
		forward.reserve(argc);
		int dropped = 0;
		for (int i = 0; i < argc; ++i) {
			const VALUE arg = argv[i];
			if (!RB_TYPE_P(arg, T_STRING)) {
				forward.push_back(arg);
				continue;
			}
			std::error_code ec;
			const std::string native_path = toStdString(arg);
			if (!std::filesystem::exists(native_path, ec)) {
				const auto vpath = resolveVirtualPath(arg);
				if (physfs_gem::exists(vpath)) {
					// Read-only mount path. Treat the delete as a no-op:
					// the caller can't observe the file as present on the
					// writable side anyway, so "already gone" is the
					// consistent answer.
					++dropped;
					continue;
				}
			}
			forward.push_back(arg);
		}

		if (forward.empty()) return INT2FIX(dropped);

		// Ruby's File.delete contract returns "the number of names passed
		// as arguments". We super-forward only the surviving names, so
		// super's return value is (forward.size()) — we add back `dropped`
		// so the caller sees the same count they would have without the
		// shim filter.
		const VALUE result = shim_super_kw(static_cast<int>(forward.size()), forward.data());
		if (dropped > 0 && RB_INTEGER_TYPE_P(result)) {
			return LONG2NUM(NUM2LONG(result) + dropped);
		}
		return result;
	}

	// ------------------------------------------------------------------
	// Dir overrides
	// ------------------------------------------------------------------

	VALUE rb_Dir_chdir(int argc, VALUE* argv, VALUE /*self*/) {
		SHIM_PASSTHROUGH_IF_INACTIVE();
		if (rb_keyword_given_p()) return shim_super_kw(argc, argv);
		// 0-arg form (chdir to home) and >1 arg (illegal) → super-forward.
		if (argc != 1) return shim_super_kw(argc, argv);
		if (!RB_TYPE_P(argv[0], T_STRING)) return shim_super_kw(argc, argv);
		const std::string p = toStdString(argv[0]);
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

	// Merge VFS-side and native-FS glob results into a single array, deduping
	// by CANONICAL ABSOLUTE PATH so the same logical file reachable through
	// both layers surfaces exactly once.
	//
	// Why this is needed: VFS results from PhysFSShim_Glob are mount-relative
	// strings (e.g. "scripts/foo.rb"); native Dir.glob returns paths shaped
	// like the input pattern (absolute when the caller passed an absolute
	// pattern, e.g. "/data/.../scripts/foo.rb"). When a file lives in both
	// the mount and on the real FS — common when an archive's contents have
	// been extracted to disk, or when the write_dir overlaps the mount root
	// — the two arrays contain the same logical file as DIFFERENT strings.
	// A plain Array#uniq doesn't merge them, so callers that iterate the
	// result and process each path end up processing the same file twice.
	// PSDK's ScriptCollector hit this on Android: every project script came
	// back twice, the second eval re-aliased PFM::Options#initialize, and
	// the alias chain closed into a circular dispatch that infinite-looped
	// at Options.new on play-game.
	//
	// Dedup key: File.expand_path(path, write_dir). Absolute paths ignore
	// the second arg; VFS-relative paths get rebased onto the active
	// write_dir so they collide with their native counterpart. When no
	// write_dir is set, fall back to File.expand_path's default (Dir.pwd).
	//
	// Output format: native paths are emitted first and win the format
	// duel — they're what other Ruby APIs return for the same file, so
	// callers that iterate the result and pass each path back into
	// File.open / require / etc. don't have to translate. VFS-only entries
	// are appended in their mount-relative form (no useful alternative).
	VALUE merge_glob_results_dedup(VALUE vfs, VALUE native) {
		if (!RB_TYPE_P(vfs, T_ARRAY))    vfs    = rb_ary_new();
		if (!RB_TYPE_P(native, T_ARRAY)) native = rb_ary_new();

		static const ID id_expand_path = rb_intern("expand_path");
		const auto& wd = physfs_gem::getWriteDir();
		const VALUE write_dir_str =
			wd.empty() ? Qnil
			           : rb_str_new(wd.data(), static_cast<long>(wd.size()));

		auto canonical = [&](VALUE entry) -> VALUE {
			if (NIL_P(write_dir_str)) {
				return rb_funcall(rb_cFile, id_expand_path, 1, entry);
			}
			return rb_funcall(rb_cFile, id_expand_path, 2, entry, write_dir_str);
		};

		const VALUE seen = rb_hash_new();
		const VALUE out  = rb_ary_new();

		const long nlen = RARRAY_LEN(native);
		for (long i = 0; i < nlen; ++i) {
			VALUE entry = RARRAY_AREF(native, i);
			if (!RB_TYPE_P(entry, T_STRING)) { rb_ary_push(out, entry); continue; }
			VALUE key = canonical(entry);
			if (NIL_P(rb_hash_aref(seen, key))) {
				rb_hash_aset(seen, key, Qtrue);
				rb_ary_push(out, entry);
			}
		}
		const long vlen = RARRAY_LEN(vfs);
		for (long i = 0; i < vlen; ++i) {
			VALUE entry = RARRAY_AREF(vfs, i);
			if (!RB_TYPE_P(entry, T_STRING)) { rb_ary_push(out, entry); continue; }
			VALUE key = canonical(entry);
			if (NIL_P(rb_hash_aref(seen, key))) {
				rb_hash_aset(seen, key, Qtrue);
				rb_ary_push(out, entry);
			}
		}
		return out;
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
	//
	// Dedup of the merged result set happens by canonical absolute path
	// (see merge_glob_results_dedup), not by string equality. Without that,
	// files reachable via both the mount and the real FS surface twice
	// because their VFS form is mount-relative and their native form is
	// pattern-shaped — Array#uniq keeps both.
	VALUE rb_Dir_glob(int argc, VALUE* argv, VALUE /*self*/) {
		SHIM_PASSTHROUGH_IF_INACTIVE();
		if (rb_keyword_given_p()) return shim_super_kw(argc, argv);
		if (argc < 1) return shim_super_kw(argc, argv);
		if (!RB_TYPE_P(argv[0], T_STRING)) return shim_super_kw(argc, argv);

		const auto resolved = resolveVirtualPath(argv[0]);
		const VALUE adjusted = rb_str_new(resolved.data(), static_cast<long>(resolved.size()));
		const int user_flags = (argc >= 2 && FIXNUM_P(argv[1])) ? NUM2INT(argv[1]) : 0;

		const VALUE vfs    = PhysFSShim_Glob(adjusted, user_flags);
		const VALUE native = shim_super_kw(argc, argv);
		return merge_glob_results_dedup(vfs, native);
	}

	VALUE rb_Dir_entries(int argc, VALUE* argv, VALUE /*self*/) {
		SHIM_PASSTHROUGH_IF_INACTIVE();
		if (rb_keyword_given_p()) return shim_super_kw(argc, argv);
		if (argc != 1 || !RB_TYPE_P(argv[0], T_STRING)) return shim_super_kw(argc, argv);
		const auto p = resolveVirtualPath(argv[0]);
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
		if (rb_keyword_given_p()) return shim_super_kw(argc, argv);
		if (argc != 1 || !RB_TYPE_P(argv[0], T_STRING)) return shim_super_kw(argc, argv);
		if (physfs_gem::isDirectory(resolveVirtualPath(argv[0]))) return Qtrue;
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

		// Try VFS as the fallback. Append .rb if the caller didn't, then
		// normalize so leading "./" (and any embedded "." / ".." segments)
		// are collapsed before PhysFS sees them — its sanitizer rejects
		// paths with such segments outright.
		std::string n = toStdString(name);
		if (n.size() < 3 || n.substr(n.size() - 3) != ".rb") n += ".rb";
		n = normalizePhysFSPath(n);
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
		rb_define_method(m_FileShim, "size",        _rbf rb_File_size,         -1);
		rb_define_method(m_FileShim, "read",        _rbf rb_File_read,         -1);
		rb_define_method(m_FileShim, "binread",     _rbf rb_File_binread,      -1);
		rb_define_method(m_FileShim, "readlines",   _rbf rb_File_readlines,    -1);
		rb_define_method(m_FileShim, "open",        _rbf rb_File_open,         -1);
		rb_define_method(m_FileShim, "new",         _rbf rb_File_new,          -1);
		rb_define_method(m_FileShim, "copy_stream", _rbf rb_File_copy_stream,  -1);
		rb_define_method(m_FileShim, "binwrite",    _rbf rb_File_binwrite,     -1);
		rb_define_method(m_FileShim, "delete",      _rbf rb_File_delete,       -1);
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
