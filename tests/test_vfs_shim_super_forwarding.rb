# Direct tests for the super-forwarding edge cases that were unmasked while
# implementing the Ruby-equivalent glob:
#
#   1. rb_call_super does NOT preserve keyword-arg semantics in Ruby 3+ —
#      callers like Tempfile.create that pass `**opts` lose their kwargs
#      and the kwargs Hash gets treated as a positional Integer.
#
#   2. File.open with non-string mode (Integer flag bitmask) trips the same
#      issue because the bail-to-super path also needs kwargs forwarding.
#
#   3. Other overrides (File.read with `encoding:`, Dir.glob with `base:`)
#      also accept kwargs and need the same forwarding.
#
# These tests exercise the paths DIRECTLY (no Tempfile indirection) so that
# the coverage holds regardless of how Ruby's stdlib changes its internals.
#
# Run with: ruby tests/test_vfs_shim_super_forwarding.rb

$LOAD_PATH.unshift(File.expand_path("../lib", __dir__))
$LOAD_PATH.unshift(File.expand_path("../lib", __dir__))

require "minitest/autorun"
require "tmpdir"
require "fileutils"
require "tempfile"
require "physfs"

class TestVFSShimSuperForwarding < Minitest::Test
  def setup
    @archive = Dir.mktmpdir("vfs_super_arch_")
    @real    = Dir.mktmpdir("vfs_super_real_")
    File.binwrite(File.join(@archive, "in_archive.txt"), "ARCHIVE")
    File.binwrite(File.join(@real,    "on_disk.txt"),    "DISK")
    PhysFS.mount(@archive)
  end

  def teardown
    PhysFS.unmount(@archive) rescue nil
    FileUtils.remove_entry(@archive)
    FileUtils.remove_entry(@real)
  end

  # ---------- File.open: kwargs propagated to super ----------

  def test_file_open_with_kwargs_on_real_fs
    # File.open(real_path, "r", encoding: "UTF-8") — kwargs must reach the
    # original File.open (not get smashed into a positional Integer).
    target = File.join(@real, "on_disk.txt")
    content = File.open(target, "r", encoding: "UTF-8") { |f| f.read }
    assert_equal "DISK", content
    assert_equal Encoding::UTF_8, content.encoding
  end

  def test_file_open_with_integer_mode_flags
    # File::RDWR | File::CREAT | File::EXCL is what Tempfile.create uses.
    # Non-string mode must super-forward without trying to VFS-handle.
    target = File.join(@real, "writeme.txt")
    flags = File::WRONLY | File::CREAT | File::TRUNC
    File.open(target, flags) { |f| f.write("ok") }
    assert_equal "ok", File.binread(target)
  end

  def test_file_open_with_integer_mode_and_kwargs
    # The exact shape Tempfile.create uses: positional integer flags PLUS
    # kwargs. Both must reach super intact.
    target = File.join(@real, "writeme2.txt")
    flags = File::WRONLY | File::CREAT | File::TRUNC
    File.open(target, flags, perm: 0o644) { |f| f.write("kw_ok") }
    assert_equal "kw_ok", File.binread(target)
  end

  def test_tempfile_create_works_through_the_shim
    # Direct regression test for the original Tempfile crash: Tempfile.create
    # internally calls File.open(path, RDWR|CREAT|EXCL, perm: 0o600, **opts).
    # Before the kwargs-forwarding fix, this crashed with
    # "no implicit conversion of Hash into Integer".
    Tempfile.create("vfs_super_") do |tmp|
      tmp.write("survived")
      tmp.flush
      assert_equal "survived", File.binread(tmp.path)
    end
  end

  # ---------- File.read / binread: super-forward keeps kwargs ----------

  def test_file_read_with_encoding_kwarg_on_real_fs
    # File.read accepts (path, length=nil, offset=0, **opts). For real-FS
    # paths the override bails to super; kwargs must propagate.
    target = File.join(@real, "on_disk.txt")
    content = File.read(target, encoding: "UTF-8")
    assert_equal "DISK", content
    assert_equal Encoding::UTF_8, content.encoding
  end

  def test_file_read_with_length_offset_super_forwards
    # File.read with length+offset is the multi-arg form the shim deliberately
    # bails on for VFS files too; native must still work.
    target = File.join(@real, "on_disk.txt")
    assert_equal "DI", File.read(target, 2)
    assert_equal "SK", File.read(target, 2, 2)
  end

  # ---------- Dir.glob: kwargs propagated to super ----------

  def test_dir_glob_with_base_kwarg_on_real_fs
    # Dir.glob(pattern, base: dir) is Ruby 3+ kwargs. The shim must forward
    # the kwarg through super when delegating to native Dir.glob.
    matches = Dir.glob("*.txt", base: @real)
    assert_includes matches, "on_disk.txt"
    refute_includes matches, "in_archive.txt", "VFS files must not leak when base: scopes to real FS"
  end

  def test_dir_glob_with_flags_positional
    # Dir.glob(pattern, flags) — flags as positional Integer. Shim's
    # rb_Dir_glob currently forwards both positional flags AND merges its own
    # VFS results. Verify the flag-bearing native call still works.
    fixture_real = File.join(@real, ".hidden.txt")
    File.binwrite(fixture_real, "h")
    Dir.chdir(@real) do
      matches = Dir.glob("*", File::FNM_DOTMATCH)
      assert_includes matches, ".hidden.txt"
      assert_includes matches, "on_disk.txt"
    end
  end

  # ---------- Dir.entries: real-FS path with encoding kwarg ----------

  def test_dir_entries_with_encoding_kwarg_on_real_fs
    entries = Dir.entries(@real, encoding: "UTF-8")
    assert_includes entries, "on_disk.txt"
  end

  # ---------- Sanity: VFS still wins for archive paths ----------

  def test_vfs_paths_still_handled_after_super_forwarding_changes
    # Make sure adding kwargs forwarding didn't accidentally route archive
    # paths through super.
    assert_equal "ARCHIVE", File.read("in_archive.txt")
    assert File.exist?("in_archive.txt")
  end
end
