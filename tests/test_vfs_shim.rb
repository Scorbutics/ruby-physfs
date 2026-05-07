# Integration tests for the C-extension shim (RubyVFSShim) — verifies that
# native Ruby File / Dir / IO / Kernel#require calls are transparently
# re-routed through cgss::vfs when the path resolves inside a mount, and
# fall through to the real filesystem otherwise.
#
# These tests REPLACE the legacy ruby_physfs_patch.rb monkey-patch: every
# observable behaviour the patch provided must be covered here.
#
# Run with: ruby tests/test_vfs_shim.rb

$LOAD_PATH.unshift(File.expand_path("../lib", __dir__))
$LOAD_PATH.unshift(File.expand_path("../lib", __dir__))

require "minitest/autorun"
require "fileutils"
require "tmpdir"
require "stringio"
require "physfs"

# Pin the shim active for the whole suite — every test mounts and unmounts,
# and we want the overrides observable across that whole lifecycle (rather
# than testing the auto-activate-on-mount lifecycle here, which lives in
# tests/test_vfs_shim_lifecycle.rb).
PhysFS.install_shim!

class TestVFSShim < Minitest::Test
  def setup
    @archive = Dir.mktmpdir("vfs_shim_archive_")
    @write   = Dir.mktmpdir("vfs_shim_write_")
    @real    = Dir.mktmpdir("vfs_shim_realfs_")
    @mounted = false
  end

  def teardown
    if @mounted
      begin PhysFS.unmount(@archive) rescue nil end
    end
    [@archive, @write, @real].each do |dir|
      FileUtils.remove_entry(dir) if dir && File.directory?(dir)
    end
  end

  def fixture(root, relative, content)
    full = File.join(root, relative)
    FileUtils.mkdir_p(File.dirname(full))
    File.binwrite(full, content)
    full
  end

  def mount_archive
    PhysFS.mount(@archive)
    @mounted = true
  end

  # ---------- File.exist? / file? / directory? ----------

  def test_file_exist_q_falls_through_to_real_fs_when_unmounted
    real = fixture(@real, "real_only.txt", "hi")
    refute PhysFS.exist?("anything")
    assert File.exist?(real), "shim must not break real-FS lookups"
  end

  def test_file_exist_q_sees_mounted_archive
    fixture(@archive, "archived.txt", "hello")
    mount_archive
    assert File.exist?("archived.txt")
    refute File.exist?("does/not/exist")
  end

  def test_file_directory_q_distinguishes_files_and_dirs
    fixture(@archive, "tree/leaf.txt", "x")
    mount_archive
    assert File.directory?("tree")
    refute File.directory?("tree/leaf.txt")
    refute File.directory?("missing")
  end

  def test_file_file_q_only_true_for_regular_files
    fixture(@archive, "tree/leaf.txt", "x")
    mount_archive
    assert File.file?("tree/leaf.txt")
    refute File.file?("tree")
    refute File.file?("missing")
  end

  # ---------- File.mtime ----------

  def test_file_mtime_returns_real_value_for_archive_files
    # Regression-guards the old patch's hardcoded 0.
    fixture(@archive, "stamp.txt", "x")
    mount_archive
    mtime = File.mtime("stamp.txt")
    assert_kind_of Time, mtime
    assert_operator mtime.to_i, :>, 0
  end

  def test_file_mtime_falls_through_for_real_fs_paths
    real = fixture(@real, "real.txt", "x")
    assert_kind_of Time, File.mtime(real)
  end

  # ---------- File.read / binread / readlines ----------

  def test_file_read_returns_utf8_string_from_archive
    fixture(@archive, "hello.txt", "world")
    mount_archive
    content = File.read("hello.txt")
    assert_equal "world", content
    assert_equal Encoding::UTF_8, content.encoding
  end

  def test_file_binread_returns_binary_string_from_archive
    bytes = (0..255).map(&:chr).join
    fixture(@archive, "blob.bin", bytes)
    mount_archive
    out = File.binread("blob.bin")
    assert_equal bytes.bytesize, out.bytesize
    assert_equal bytes, out
  end

  def test_file_readlines_splits_on_newlines
    fixture(@archive, "lines.txt", "a\nb\nc")
    mount_archive
    lines = File.readlines("lines.txt")
    assert_equal ["a", "b", "c"], lines
  end

  def test_file_read_falls_through_when_not_in_archive
    real = fixture(@real, "real.txt", "real content")
    mount_archive
    assert_equal "real content", File.read(real)
  end

  # ---------- File.open ----------

  def test_file_open_read_returns_io_when_no_block
    fixture(@archive, "in.txt", "abc")
    mount_archive
    io = File.open("in.txt", "r")
    assert_respond_to io, :read
    assert_equal "abc", io.read
    io.close
  end

  def test_file_open_read_with_block_yields_and_closes
    fixture(@archive, "in.txt", "abc")
    mount_archive
    seen = nil
    File.open("in.txt", "r") { |f| seen = f.read }
    assert_equal "abc", seen
  end

  def test_file_open_write_falls_through_to_real_fs
    # Even with an archive mounted, write-mode opens MUST hit the real FS
    # (PhysFS write API can't do rename/fsync — game saves rely on those).
    target = File.join(@real, "saved.txt")
    mount_archive
    File.open(target, "w") { |f| f.write("game-progress") }
    assert File.exist?(target)
    assert_equal "game-progress", File.binread(target)
  end

  def test_file_open_append_mode_falls_through_to_real_fs
    target = File.join(@real, "log.txt")
    File.write(target, "first\n")
    mount_archive
    File.open(target, "a") { |f| f.write("second\n") }
    assert_equal "first\nsecond\n", File.binread(target)
  end

  # ---------- File.copy_stream / IO.copy_stream ----------

  def test_file_copy_stream_from_archive_to_real_fs
    fixture(@archive, "src.bin", "payload")
    dst = File.join(@real, "out.bin")
    mount_archive
    written = File.copy_stream("src.bin", dst)
    assert_equal 7, written
    assert_equal "payload", File.binread(dst)
  end

  def test_io_copy_stream_delegates_to_file_copy_stream
    fixture(@archive, "src.bin", "abcdef")
    dst = File.join(@real, "via_io.bin")
    mount_archive
    IO.copy_stream("src.bin", dst)
    assert_equal "abcdef", File.binread(dst)
  end

  # ---------- Dir.[] / Dir.glob / Dir.entries / Dir.exist? ----------

  def test_dir_glob_brace_expansion_in_archive
    # The old patch raised "UNSUPPORTED" on this shape.
    fixture(@archive, "alpha/x.png", "")
    fixture(@archive, "beta/x.png", "")
    fixture(@archive, "gamma/x.png", "")
    mount_archive
    matches = Dir["{alpha,beta}/*.png"]
    assert_equal 2, matches.size
    assert_includes matches, "alpha/x.png"
    assert_includes matches, "beta/x.png"
  end

  def test_dir_glob_recursive_double_star_in_archive
    fixture(@archive, "a.rb", "")
    fixture(@archive, "sub/b.rb", "")
    fixture(@archive, "sub/deeper/c.rb", "")
    fixture(@archive, "ignored.md", "")
    mount_archive
    matches = Dir["**/*.rb"]
    assert_includes matches, "a.rb"
    assert_includes matches, "sub/b.rb"
    assert_includes matches, "sub/deeper/c.rb"
    refute(matches.any? { |m| m.end_with?(".md") })
  end

  def test_dir_glob_merges_archive_and_real_fs
    fixture(@archive, "in_arch.rb", "")
    real_match = fixture(@real, "in_arch_real.rb", "")
    mount_archive
    matches = Dir["#{@real}/*.rb"]
    assert_includes matches, real_match
  end

  def test_dir_entries_returns_dot_dotdot_plus_children
    fixture(@archive, "things/a.txt", "")
    fixture(@archive, "things/b.txt", "")
    mount_archive
    entries = Dir.entries("things")
    assert_includes entries, "."
    assert_includes entries, ".."
    assert_includes entries, "a.txt"
    assert_includes entries, "b.txt"
  end

  def test_dir_exist_q
    fixture(@archive, "things/x", "")
    mount_archive
    assert Dir.exist?("things")
    refute Dir.exist?("things/x")  # file, not dir
    refute Dir.exist?("missing")
  end

  # ---------- Dir.chdir (virtual) ----------

  def test_dir_chdir_real_path_falls_through
    Dir.chdir(@real) do
      assert_equal File.realpath(@real), File.realpath(Dir.pwd)
    end
  end

  def test_dir_chdir_virtual_path_scopes_archive_lookups
    # When chdir'd into a virtual subdir, relative reads should resolve
    # against that subdir without the caller having to prefix the path.
    fixture(@archive, "scripts/foo.rb", "VIRT_OK")
    mount_archive
    Dir.chdir("scripts") do
      assert File.exist?("foo.rb")
      assert_equal "VIRT_OK", File.read("foo.rb")
    end
    # After block, virtual cwd is restored — bare "foo.rb" no longer resolves.
    refute File.exist?("foo.rb")
  end

  # ---------- Kernel#require / require_relative ----------

  def test_require_loads_ruby_script_from_archive
    fixture(@archive, "vfs_required_module.rb", "$vfs_required_module_loaded = true")
    mount_archive
    refute defined?($vfs_required_module_loaded) && $vfs_required_module_loaded
    assert require("vfs_required_module")
    assert $vfs_required_module_loaded
    assert_equal false, require("vfs_required_module"),
                 "second require must be a no-op (returns false)"
  end

  def test_require_raises_loaderror_when_missing_everywhere
    mount_archive
    assert_raises(LoadError) { require("nope_does_not_exist_anywhere") }
  end

  def test_require_strips_leading_dot_slash
    # Regression: PhysFS's path sanitizer rejects any "." or ".." segment
    # with PHYSFS_ERR_BAD_FILENAME. Naively forwarding "./Game.rb" through
    # to PHYSFS_exists() returns false, so the require falls through to a
    # LoadError even though the file is in the archive. The shim must
    # normalize before consulting PhysFS.
    fixture(@archive, "vfs_dotslash_module.rb", "$vfs_dotslash_loaded = :ok")
    mount_archive
    assert require("./vfs_dotslash_module.rb")
    assert_equal :ok, $vfs_dotslash_loaded
  end

  def test_file_apis_strip_leading_dot_slash
    fixture(@archive, "leading_dot.txt", "ok")
    mount_archive
    assert File.exist?("./leading_dot.txt")
    assert File.file?("./leading_dot.txt")
    assert_equal "ok", File.read("./leading_dot.txt")
  end

  def test_require_strips_write_dir_prefix_from_absolute_path
    # Regression: pokemonsdk's loader does
    #   psdk_path = File.expand_path('pokemonsdk')
    #   require "#{psdk_path}/scripts/ScriptLoad.rb"
    # The expand_path step (not shimmed) yields a real-FS absolute path
    # under the project root, but the file actually lives in the archive.
    # When the resolved path lives under PhysFS.write_dir, the shim must
    # strip that prefix so the require resolves through the archive.
    fixture(@archive, "pokemonsdk/scripts/Loader.rb",
            "$vfs_writedir_loader = :ok")
    PhysFS.write_dir = @write
    mount_archive
    abs = File.join(@write, "pokemonsdk/scripts/Loader.rb")
    assert require(abs)
    assert_equal :ok, $vfs_writedir_loader
  end

  def test_file_apis_strip_write_dir_prefix_from_absolute_path
    fixture(@archive, "deep/asset.txt", "ARCHIVED")
    PhysFS.write_dir = @write
    mount_archive
    abs = File.join(@write, "deep/asset.txt")
    assert File.exist?(abs)
    assert_equal "ARCHIVED", File.read(abs)
  end

  def test_absolute_path_outside_write_dir_falls_through_to_real_fs
    # Sanity-check: paths whose prefix isn't write_dir must NOT be
    # stripped — they're real-FS lookups and the original absolute path
    # has to reach super unchanged.
    real = fixture(@real, "outside.txt", "on-disk")
    PhysFS.write_dir = @write
    mount_archive
    assert File.exist?(real)
    assert_equal "on-disk", File.read(real)
  end

  def test_dir_entries_dot_preserved_for_real_fs_fallthrough
    # The "." preservation guard inside normalizePhysFSPath: a bare "."
    # must not be rewritten to "" (the archive root). Otherwise
    # Dir.entries(".") would silently start listing the archive instead
    # of the real cwd. We verify by mounting an archive that contains a
    # uniquely-named file and asserting it does NOT show up in the real
    # cwd's entries.
    fixture(@archive, "vfs_root_marker_xyz.txt", "x")
    mount_archive
    entries = Dir.entries(".")
    refute_includes entries, "vfs_root_marker_xyz.txt"
  end

  def test_require_relative_resolves_against_caller_file
    # caller_locations gives us the path of THIS test file, so the relative
    # require should look in tests/. Use a transient file there.
    transient = File.join(__dir__, "_vfs_shim_transient.rb")
    File.write(transient, "$vfs_shim_transient = :ok")
    begin
      require_relative("_vfs_shim_transient")
      assert_equal :ok, $vfs_shim_transient
    ensure
      File.delete(transient) if File.exist?(transient)
    end
  end

  # ---------- write_dir-shadows-archive end-to-end ----------

  def test_save_shadowing_through_native_file_apis
    # The end-to-end save scenario, exercised through plain File.* calls
    # (no PhysFS calls) — proves the shim transparently surfaces
    # write_dir's contents on subsequent reads.
    fixture(@archive, "save01.dat", "stock")
    PhysFS.write_dir = @write
    mount_archive

    assert_equal "stock", File.read("save01.dat")

    # Player saves locally — write goes through the shim's "real fopen" branch.
    File.binwrite(File.join(@write, "save01.dat"), "player-saved")
    assert_equal "player-saved", File.read("save01.dat")

    File.delete(File.join(@write, "save01.dat"))
    assert_equal "stock", File.read("save01.dat")
  end
end
