# End-to-end tests for the PhysFS Ruby surface introduced in Layer 1.
#
# Covers the same invariants as the LiteCGSS gtest suite, but exercised through
# the Ruby C-extension boundary so the bindings, refcounting, and exception
# translation are all live.
#
# Run with: ruby tests/test_vfs.rb

$LOAD_PATH.unshift(File.expand_path("../lib", __dir__))
$LOAD_PATH.unshift(File.expand_path("../lib", __dir__))

require "minitest/autorun"
require "fileutils"
require "tmpdir"
require "physfs"

class TestPhysFS < Minitest::Test
  def setup
    @archive = Dir.mktmpdir("litergss_vfs_archive_")
    @write   = Dir.mktmpdir("litergss_vfs_write_")
  end

  def teardown
    # No public unmount-all on the Ruby side; tear down individual mounts we
    # know we created. Only the archive is mounted in mount-only tests; the
    # save-shadowing test mounts both via setWriteDir + mount.
    [@archive, @write].each do |dir|
      begin
        PhysFS.unmount(dir)
      rescue StandardError
        # Not mounted (or already unmounted) — fine.
      end
    end
    FileUtils.remove_entry(@archive) if File.exist?(@archive)
    FileUtils.remove_entry(@write)   if File.exist?(@write)
  end

  def write_fixture(root, relative, content)
    full = File.join(root, relative)
    FileUtils.mkdir_p(File.dirname(full))
    File.binwrite(full, content)
  end

  def test_mount_then_exist_and_directory
    write_fixture(@archive, "hello.txt", "world")
    write_fixture(@archive, "nested/deep/file.dat", "payload")
    PhysFS.mount(@archive)

    assert PhysFS.exist?("hello.txt")
    assert PhysFS.exist?("nested/deep/file.dat")
    refute PhysFS.exist?("missing.txt")

    assert PhysFS.directory?("nested")
    assert PhysFS.directory?("nested/deep")
    refute PhysFS.directory?("hello.txt")
  end

  def test_mtime_is_real_not_zero
    # The old monkey-patch hardcoded File.mtime to 0; this regression-guards
    # the contract that mtime now surfaces the real on-disk value so any
    # cache-invalidation logic in pokemonsdk works correctly.
    write_fixture(@archive, "stamp.txt", "x")
    PhysFS.mount(@archive)

    assert_operator PhysFS.mtime("stamp.txt"), :>, 0
    assert_equal 0, PhysFS.mtime("missing.txt")
  end

  def test_read_returns_binary_string
    write_fixture(@archive, "blob.bin", "abc\0def")
    PhysFS.mount(@archive)

    bytes = PhysFS.read("blob.bin")
    assert_equal 7, bytes.bytesize
    assert_equal "abc\0def", bytes
  end

  def test_enumerate_lists_direct_children
    write_fixture(@archive, "a.txt", "")
    write_fixture(@archive, "b.txt", "")
    write_fixture(@archive, "sub/c.txt", "")
    PhysFS.mount(@archive)

    entries = PhysFS.enumerate("/")
    assert_includes entries, "a.txt"
    assert_includes entries, "b.txt"
    assert_includes entries, "sub"
  end

  def test_glob_supports_patterns_the_old_patch_could_not
    write_fixture(@archive, "alpha/x.png", "")
    write_fixture(@archive, "beta/x.png", "")
    write_fixture(@archive, "gamma/x.png", "")
    write_fixture(@archive, "alpha/y.txt", "")
    write_fixture(@archive, "alpha/sub/z.png", "")
    PhysFS.mount(@archive)

    # Brace expansion — old patch had no concept of this.
    brace = PhysFS.glob("{alpha,beta}/*.png")
    assert_equal 2, brace.size
    assert_includes brace, "alpha/x.png"
    assert_includes brace, "beta/x.png"

    # Recursive ** — old patch raised "UNSUPPORTED" on this shape.
    deep = PhysFS.glob("**/*.png")
    assert_includes deep, "alpha/x.png"
    assert_includes deep, "alpha/sub/z.png"
    assert_includes deep, "gamma/x.png"
    refute_includes deep, "alpha/y.txt"
  end

  def test_write_dir_shadows_archive_for_subsequent_reads
    # Save-file invariant: a stock save in the archive is shadowed by a
    # later real-FS save in the write dir, and removing the save falls
    # back to the stock copy.
    write_fixture(@archive, "save01.dat", "stock-from-archive")

    PhysFS.write_dir = @write
    PhysFS.mount(@archive)

    # Initially: archive copy is visible.
    assert_equal "stock-from-archive", PhysFS.read("save01.dat")

    # Player saves locally (native fopen on write_dir + path).
    File.binwrite(File.join(@write, "save01.dat"), "player-saved")
    assert_equal "player-saved", PhysFS.read("save01.dat")

    # Player deletes the save -> falls back to the stock copy.
    FileUtils.rm(File.join(@write, "save01.dat"))
    assert_equal "stock-from-archive", PhysFS.read("save01.dat")
  end

  def test_write_dir_getter_round_trip
    PhysFS.write_dir = @write
    assert_equal @write, PhysFS.write_dir
  end

  def test_unmount_removes_visibility
    write_fixture(@archive, "ephemeral.txt", "bye")
    PhysFS.mount(@archive)
    assert PhysFS.exist?("ephemeral.txt")

    PhysFS.unmount(@archive)
    refute PhysFS.exist?("ephemeral.txt")
  end

  def test_read_of_missing_file_raises
    PhysFS.mount(@archive)
    assert_raises(PhysFS::Error) { PhysFS.read("nope.bin") }
  end
end
