# Glob equivalence tests — proves that Dir.glob over a mounted archive matches
# native Ruby Dir.glob semantics for the cases where the previous in-house
# C++ matcher (cgss::vfs::glob) diverged from Ruby. Specifically:
#
#   - dotfile exclusion (default Ruby: `*` does NOT match `.foo`)
#   - dotfile inclusion via File::FNM_DOTMATCH
#   - backslash escapes (`\*` matches a literal `*`)
#   - trailing-slash patterns are dir-only and emit results with `/`
#   - case folding via File::FNM_CASEFOLD
#
# The strategy is to fixture identical files on a real-FS scratch dir AND
# inside a mounted archive, then assert that Dir.glob over the archive
# returns the same set as Dir.glob over the real-FS scratch dir (modulo
# the path prefix). If they ever diverge again we'll see it here.
#
# Run with: ruby tests/test_vfs_shim_glob.rb

$LOAD_PATH.unshift(File.expand_path("../lib", __dir__))
$LOAD_PATH.unshift(File.expand_path("../lib", __dir__))

require "minitest/autorun"
require "tmpdir"
require "fileutils"
require "physfs"

class TestVFSShimGlob < Minitest::Test
  FIXTURE_FILES = %w[
    a.txt
    b.txt
    .hidden.txt
    nested/c.txt
    nested/.hidden_inner.txt
    nested/sub/d.txt
    UPPER.TXT
    star_literal_*.txt
  ].freeze

  # Detect case-insensitive scratch FS (default on macOS APFS / HFS+). When
  # the host is case-insensitive, native Dir.glob is implicitly case-folding,
  # which makes it useless as an oracle for case-sensitivity assertions and
  # corrupts the unrelated comparisons that happen to include UPPER.TXT.
  def case_insensitive_fs?(dir)
    probe = File.join(dir, ".__case_probe__")
    File.binwrite(probe, "x")
    insensitive = File.exist?(File.join(dir, ".__CASE_PROBE__"))
    File.delete(probe)
    insensitive
  end

  def setup
    @archive = Dir.mktmpdir("vfs_glob_arch_")
    @real    = Dir.mktmpdir("vfs_glob_real_")
    @case_insensitive = case_insensitive_fs?(@real)
    fixtures = @case_insensitive ? FIXTURE_FILES - %w[UPPER.TXT] : FIXTURE_FILES
    fixtures.each do |rel|
      [@archive, @real].each do |root|
        full = File.join(root, rel)
        FileUtils.mkdir_p(File.dirname(full))
        File.binwrite(full, rel)
      end
    end
    PhysFS.mount(@archive)
  end

  def teardown
    PhysFS.unmount(@archive) rescue nil
    FileUtils.remove_entry(@archive)
    FileUtils.remove_entry(@real)
  end

  # Compare {pattern, flags} → archive results vs native results, with the
  # native results stripped of their @real prefix so the candidate sets are
  # comparable (both rooted at "").
  def assert_archive_matches_native(pattern, flags = 0)
    Dir.chdir(@real) do
      native  = flags.zero? ? Dir.glob(pattern) : Dir.glob(pattern, flags)
      archive = flags.zero? ? PhysFS.glob(pattern) : PhysFS.glob(pattern, flags)
      assert_equal native.sort, archive.sort,
                   "Dir.glob and PhysFS.glob diverged for #{pattern.inspect} flags=#{flags}"
    end
  end

  # ---------- dotfile semantics ----------

  def test_dotfiles_excluded_by_default
    # Old in-house matcher matched `.hidden.txt` here; native Ruby does not.
    assert_archive_matches_native("*.txt")
  end

  def test_dotfiles_included_with_FNM_DOTMATCH
    assert_archive_matches_native("*.txt", File::FNM_DOTMATCH)
  end

  def test_recursive_dotfile_exclusion
    assert_archive_matches_native("**/*.txt")
  end

  def test_recursive_dotfile_inclusion_with_flag
    assert_archive_matches_native("**/*.txt", File::FNM_DOTMATCH)
  end

  # ---------- backslash escape ----------

  def test_backslash_escape_matches_literal_star
    # File literally named "star_literal_*.txt" — the pattern `\*.txt`
    # should match it but NOT match a.txt / b.txt etc. Old matcher had
    # no escape handling.
    assert_archive_matches_native('star_literal_\*.txt')
  end

  # ---------- trailing slash dir-only ----------

  def test_trailing_slash_matches_only_directories
    # `nested/` should match if `nested` is a directory; `*.txt` candidates
    # should not surface here.
    assert_archive_matches_native("nested/")
  end

  def test_recursive_trailing_slash_lists_all_directories
    assert_archive_matches_native("**/")
  end

  # ---------- char class ----------

  def test_char_class
    assert_archive_matches_native("[ab].txt")
  end

  def test_negated_char_class
    assert_archive_matches_native("[!ab].txt")
  end

  # ---------- brace expansion ----------

  def test_brace_expansion
    assert_archive_matches_native("{a,b,nested}.txt")
  end

  def test_nested_brace_expansion
    assert_archive_matches_native("{a,nested/{c,sub/d}}.txt")
  end

  # ---------- case folding ----------

  def test_case_sensitive_by_default
    skip "Host FS is case-insensitive; native Dir.glob is not a valid oracle" if @case_insensitive
    # `*.txt` should NOT match UPPER.TXT
    assert_archive_matches_native("*.txt")
  end

  def test_case_insensitive_with_FNM_CASEFOLD
    skip "Host FS is case-insensitive; UPPER.TXT cannot be fixtured distinctly" if @case_insensitive
    assert_archive_matches_native("*.txt", File::FNM_CASEFOLD)
  end

  # ---------- recursive ** at multiple positions ----------

  def test_double_star_at_root
    assert_archive_matches_native("**/d.txt")
  end

  def test_double_star_in_middle
    assert_archive_matches_native("nested/**/d.txt")
  end

  # ---------- the cache is invalidated correctly ----------

  def test_cache_invalidated_on_unmount
    # Sanity: after unmount, glob over the archive must see nothing.
    assert_includes PhysFS.glob("a.txt"), "a.txt"
    PhysFS.unmount(@archive)
    assert_empty PhysFS.glob("a.txt"),
                 "Cache must invalidate on unmount"
  end

  def test_cache_invalidated_on_remount
    second = Dir.mktmpdir("vfs_glob_b_")
    File.binwrite(File.join(second, "only_in_second.txt"), "x")
    begin
      PhysFS.mount(second)
      results = PhysFS.glob("only_in_second.txt")
      assert_includes results, "only_in_second.txt",
                      "Cache must rebuild after a new mount"
    ensure
      PhysFS.unmount(second) rescue nil
      FileUtils.remove_entry(second)
    end
  end
end
