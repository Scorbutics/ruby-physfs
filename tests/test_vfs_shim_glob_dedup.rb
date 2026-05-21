# Glob dedup tests — proves that Dir.glob through the PhysFS shim returns each
# logical file EXACTLY ONCE, even when the file is reachable through both the
# mount (VFS) and the real filesystem (native).
#
# Before the fix, the shim:
#   - asked PhysFSShim_Glob for VFS-side matches (returned as mount-relative
#     strings, e.g. "scripts/foo.rb"),
#   - asked native Dir.glob for real-FS matches (returned shaped like the
#     input pattern; absolute when the caller passed an absolute pattern),
#   - concatenated the two arrays and called Array#uniq.
#
# String-level uniq doesn't catch the case where the same file lives in both
# the mount and the real FS — its VFS form ("scripts/foo.rb") and its native
# form ("/tmp/.../scripts/foo.rb") are different strings — so callers iterating
# the result processed the same file twice. PSDK's ScriptCollector on Android
# hit exactly this: every project script was packed into Scripts.dat twice,
# the second iseq.eval re-aliased PFM::Options#initialize, and the alias chain
# closed into a circular dispatch that infinite-looped at Options.new.
#
# Fix: merge_glob_results_dedup in Shim.cpp now dedups by canonical absolute
# path via File.expand_path(entry, write_dir), so VFS-relative and native-
# absolute paths for the same file collide on the same key.
#
# Run with: ruby tests/test_vfs_shim_glob_dedup.rb

$LOAD_PATH.unshift(File.expand_path("../lib", __dir__))

require "minitest/autorun"
require "tmpdir"
require "fileutils"
require "physfs"

PhysFS.install_shim!

class TestVFSShimGlobDedup < Minitest::Test
  def setup
    # Single root that acts as BOTH the PhysFS mount source AND the real FS
    # write_dir. This is the exact shape PSDK on Android sees: the project's
    # `scripts/` directory is mount-mounted via PhysFS *and* sits on disk at
    # the same location, so the same file is reachable through both layers.
    @root = Dir.mktmpdir("vfs_shim_glob_dedup_")
    PhysFS.write_dir = @root
    PhysFS.mount(@root)
  end

  def teardown
    PhysFS.unmount(@root) rescue nil
    # The C-side setter requires a String, so we can't unset write_dir here.
    # Each test's setup sets it again before doing anything, so no state
    # leaks across tests within this class.
    FileUtils.remove_entry(@root) if @root && File.directory?(@root)
  end

  def fixture(relative, content = relative)
    full = File.join(@root, relative)
    FileUtils.mkdir_p(File.dirname(full))
    File.binwrite(full, content)
    full
  end

  # --------------------------------------------------------------------
  # Core dedup: each logical file emitted exactly once, no matter how
  # many layers it's reachable through.
  # --------------------------------------------------------------------

  def test_same_file_in_mount_and_realfs_emitted_once
    fixture("scripts/foo.rb")
    fixture("scripts/bar.rb")

    # The pattern is absolute — native Dir.glob returns absolute paths;
    # PhysFSShim_Glob returns mount-relative paths. Before the fix the
    # merged array had each file twice with two different strings.
    matches = Dir.glob(File.join(@root, "scripts/*.rb"))

    canonical = matches.map { |m| File.expand_path(m, @root) }.sort
    assert_equal canonical, canonical.uniq,
                 "Dir.glob returned duplicates by canonical path: #{matches.inspect}"
    assert_equal 2, matches.size,
                 "Expected exactly 2 results, got #{matches.size}: #{matches.inspect}"
  end

  def test_relative_pattern_also_dedups
    fixture("scripts/foo.rb")
    Dir.chdir(@root) do
      matches = Dir.glob("scripts/*.rb")
      canonical = matches.map { |m| File.expand_path(m, @root) }
      assert_equal canonical, canonical.uniq,
                   "Dir.glob with relative pattern returned duplicates: #{matches.inspect}"
      assert_equal 1, matches.size
    end
  end

  def test_recursive_glob_dedups_per_file
    fixture("scripts/a.rb")
    fixture("scripts/sub/b.rb")
    fixture("scripts/sub/deeper/c.rb")

    matches = Dir.glob(File.join(@root, "scripts/**/*.rb"))
    canonical = matches.map { |m| File.expand_path(m, @root) }
    assert_equal canonical.sort, canonical.uniq.sort,
                 "Recursive Dir.glob returned duplicates: #{matches.inspect}"
    assert_equal 3, matches.size
  end

  def test_trailing_slash_dir_pattern_dedups
    # The shape PSDK ScriptCollector's recursive walk uses:
    # Dir[File.join(root, '*/')] — returns subdirs with trailing slash.
    # Subdir paths from both layers must dedup the same way as files.
    fixture("scripts/12 Options/a.rb")
    fixture("scripts/00000 Plugins/AutoRunOption/b.rb")
    fixture("scripts/12 Options/c.rb")
    matches = Dir.glob(File.join(@root, "scripts/*/"))
    canonical = matches.map { |m| File.expand_path(m, @root) }
    assert_equal canonical.sort, canonical.uniq.sort,
                 "Dir-only glob returned duplicate subdirs: #{matches.inspect}"
    assert_equal 2, matches.size,
                 "Expected exactly 2 subdirs, got #{matches.size}: #{matches.inspect}"
  end

  # --------------------------------------------------------------------
  # Format precedence: native paths win the format duel. Callers that
  # iterate the result and pass each entry back into File.open / require
  # don't have to translate VFS-relative paths to absolute first.
  # --------------------------------------------------------------------

  def test_native_path_form_is_preferred_when_present
    full = fixture("scripts/foo.rb")
    matches = Dir.glob(File.join(@root, "scripts/*.rb"))
    assert_equal [full], matches,
                 "When file exists on real FS, native (absolute) form should be emitted, " \
                 "not the VFS-relative form. Got: #{matches.inspect}"
  end

  def test_vfs_only_files_still_surfaced_in_relative_form
    # Simulate a file that ONLY lives in the mount, not on the disk under
    # the write_dir. We achieve this by mounting a separate archive dir
    # whose contents aren't mirrored under write_dir.
    archive_only = Dir.mktmpdir("vfs_shim_glob_archive_only_")
    archive_full = File.join(archive_only, "vfs_only.rb")
    File.binwrite(archive_full, "x")
    begin
      PhysFS.mount(archive_only)
      Dir.chdir(@root) do
        matches = Dir.glob("*.rb")
        # Native side sees nothing under @root for this name; VFS side
        # surfaces it from the archive_only mount in mount-relative form.
        assert_includes matches, "vfs_only.rb",
                        "VFS-only files must still surface; got #{matches.inspect}"
      end
    ensure
      PhysFS.unmount(archive_only) rescue nil
      FileUtils.remove_entry(archive_only)
    end
  end

  # --------------------------------------------------------------------
  # Regression: the old `.uniq` only caught LITERAL string duplicates.
  # If the SAME string ever appeared twice in the merged array (e.g. two
  # mounts both contributing the same VFS-relative name), uniq still
  # collapsed them. Make sure our canonical-key path preserves that.
  # --------------------------------------------------------------------

  def test_literal_string_duplicates_still_collapse
    # Mount the same root twice via separate dirs to provoke duplicate
    # VFS results with identical strings.
    fixture("dup.rb")
    second = Dir.mktmpdir("vfs_shim_glob_dup_")
    File.binwrite(File.join(second, "dup.rb"), "x")
    begin
      PhysFS.mount(second)
      Dir.chdir(@root) do
        matches = Dir.glob("dup.rb")
        assert_equal matches, matches.uniq,
                     "Identical strings in the merged array must collapse: #{matches.inspect}"
      end
    ensure
      PhysFS.unmount(second) rescue nil
      FileUtils.remove_entry(second)
    end
  end
end
