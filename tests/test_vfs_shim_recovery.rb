# Recovery tests for the C-extension shim — verifies that
# ensureModulesPrepended()'s two-phase gate (C-static cache + Ruby-state
# probe) re-establishes the prepend when an embedder snapshot/restore
# pass rips the shim modules out of PhysFS / Dir.singleton_class.
#
# The bug this guards against: the gem caches "shim modules already
# prepended" in a process-lifetime C++ static. If an embedder (e.g.
# PSDK-Android's PSDKVMSnapshot.restore!) calls remove_const + unprepend
# on our shim modules between mounts, the C-static still says "done" so
# the next auto-activate flips the active flag without re-prepending.
# Result: PhysFS.shim_installed? reports true, but File / Dir / Kernel#
# require silently route to stock Ruby — archive lookups miss entirely.
#
# We can't unprepend a module from Ruby alone (no rb_unprepend), so the
# tests below simulate the embedder pass by remove_const'ing the shim
# modules. That alone is enough to exercise the Ruby-state probe: the
# fix's `shimModulesIntact()` checks PhysFS::DirShim's const-defined
# status before its ancestor membership, and a missing const must
# trigger a full re-prepend pass.
#
# Run with: ruby tests/test_vfs_shim_recovery.rb

$LOAD_PATH.unshift(File.expand_path("../lib", __dir__))

require "minitest/autorun"
require "tmpdir"
require "fileutils"
require "physfs"

class TestVFSShimRecovery < Minitest::Test
  SHIM_CONSTS = %i[FileShim DirShim IOShim KernelShim].freeze

  def setup
    @archive = Dir.mktmpdir("vfs_recov_")
    File.binwrite(File.join(@archive, "marker.txt"), "in_archive")
    Dir.mkdir(File.join(@archive, "sub_dir"))
    # Reset to a known-inactive state regardless of prior test order.
    PhysFS.uninstall_shim!
    # And to a known "no shim consts" state, so the pre-mount assertions
    # in test_first_activation_* hold whether or not an earlier test in
    # this file already activated the shim. The orphaned old prepend
    # stays in Dir.singleton_class.ancestors (Ruby has no rb_unprepend),
    # but that's fine — the assertions probe identity (equal?) against
    # the current PhysFS::DirShim, not against the orphan.
    SHIM_CONSTS.each do |c|
      PhysFS.send(:remove_const, c) if PhysFS.const_defined?(c)
    end
  end

  def teardown
    begin PhysFS.unmount(@archive) rescue nil end
    PhysFS.uninstall_shim!
    FileUtils.remove_entry(@archive) if File.directory?(@archive)
  end

  # Baseline: activation creates the shim constants and prepends them
  # onto Dir / File / Kernel. We only assert on the AFTER state — the
  # BEFORE state can carry orphaned modules from earlier tests in the
  # same process (Ruby has no rb_unprepend, so old prepends linger
  # even when their owning const has been removed). The post-mount
  # assertions probe by identity against the live PhysFS::DirShim, so
  # those orphans don't contaminate the result.
  def test_activation_creates_shim_constants_and_prepends
    SHIM_CONSTS.each do |c|
      refute PhysFS.const_defined?(c),
             "precondition: setup removed #{c}, so it must not be defined"
    end

    PhysFS.mount(@archive)

    SHIM_CONSTS.each do |c|
      assert PhysFS.const_defined?(c), "#{c} should exist after mount"
    end
    assert_includes Dir.singleton_class.ancestors, PhysFS::DirShim
    assert_includes File.singleton_class.ancestors, PhysFS::FileShim
    assert_includes Kernel.ancestors,               PhysFS::KernelShim
  end

  # The core recovery contract: if the shim constants are reaped between
  # mounts (the PSDKVMSnapshot.restore! scenario), the next mount must
  # re-create them — NOT silently no-op based on the C-static cache.
  def test_const_removal_between_mounts_triggers_reprepend
    PhysFS.mount(@archive)
    assert PhysFS.shim_installed?
    original_dirshim_id = PhysFS::DirShim.object_id

    # Tear down: unmount auto-deactivates the shim; remove_const
    # simulates the embedder reaping our state. Anything left over from
    # this in Dir.singleton_class.ancestors is an orphan (Ruby has no
    # rb_unprepend), but const_defined? now returns false, which is the
    # signal the fix uses to decide a re-create is needed.
    PhysFS.unmount(@archive)
    SHIM_CONSTS.each do |c|
      PhysFS.send(:remove_const, c) if PhysFS.const_defined?(c)
    end
    SHIM_CONSTS.each do |c|
      refute PhysFS.const_defined?(c), "precondition: #{c} removed"
    end

    PhysFS.mount(@archive)

    SHIM_CONSTS.each do |c|
      assert PhysFS.const_defined?(c),
             "#{c} must be re-created on re-mount after external removal"
    end
    refute_equal original_dirshim_id, PhysFS::DirShim.object_id,
                 "DirShim must be a fresh module, not the orphaned original"
    assert_includes Dir.singleton_class.ancestors, PhysFS::DirShim,
                    "Fresh DirShim must be re-prepended onto Dir.singleton_class"
    assert_includes File.singleton_class.ancestors, PhysFS::FileShim
    assert_includes Kernel.ancestors,               PhysFS::KernelShim
  end

  # Functional proof: after a snapshot/restore-style reap + re-mount, the
  # shim's overrides must actually win method lookup. Stock Ruby would
  # miss the archive contents — only the freshly-prepended DirShim sees
  # them. Catches the silent-fallthrough mode the C-static cache used to
  # produce.
  def test_dir_exist_routes_through_shim_after_reap_and_remount
    PhysFS.mount(@archive)
    assert Dir.exist?("sub_dir"), "baseline: shim sees archive directories"

    PhysFS.unmount(@archive)
    SHIM_CONSTS.each do |c|
      PhysFS.send(:remove_const, c) if PhysFS.const_defined?(c)
    end

    PhysFS.mount(@archive)
    assert PhysFS.shim_installed?
    assert Dir.exist?("sub_dir"),
           "Dir.exist? must hit the re-installed shim, not stock Ruby"
    assert File.exist?("marker.txt"),
           "File.exist? must hit the re-installed shim, not stock Ruby"
  end

  # Sanity guard for the happy path: when nobody has reaped our state
  # between mounts, the C-static fast path must still short-circuit —
  # the recovery check should be free in steady-state operation, not
  # cause re-prepends that double up the MRO.
  def test_steady_state_remount_does_not_re_create_shim_modules
    PhysFS.mount(@archive)
    first_dirshim_id = PhysFS::DirShim.object_id
    dirshim_count_before = Dir.singleton_class.ancestors
                              .count { |a| a.equal?(PhysFS::DirShim) }
    PhysFS.unmount(@archive)
    PhysFS.mount(@archive)

    assert_equal first_dirshim_id, PhysFS::DirShim.object_id,
                 "Steady-state re-mount must reuse the existing DirShim"
    dirshim_count_after = Dir.singleton_class.ancestors
                             .count { |a| a.equal?(PhysFS::DirShim) }
    assert_equal dirshim_count_before, dirshim_count_after,
                 "DirShim must appear exactly once in the MRO; re-prepending would duplicate it"
  end
end
