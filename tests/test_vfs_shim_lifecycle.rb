# Lifecycle tests for the auto-install / auto-uninstall behaviour of the
# C-extension shim — verifies that:
#
#   - requiring 'physfs' alone does NOT activate the shim (zero impact)
#   - the first PhysFS.mount auto-activates it
#   - the last PhysFS.unmount auto-deactivates it
#   - manual install_shim! / uninstall_shim! still work as overrides
#
# Each test self-resets via uninstall_shim! / unmount before asserting,
# so test ordering is irrelevant.
#
# Run with: ruby tests/test_vfs_shim_lifecycle.rb

$LOAD_PATH.unshift(File.expand_path("../lib", __dir__))
$LOAD_PATH.unshift(File.expand_path("../lib", __dir__))

require "minitest/autorun"
require "tmpdir"
require "fileutils"
require "physfs"

class TestVFSShimLifecycle < Minitest::Test
  def setup
    @archive = Dir.mktmpdir("vfs_lc_")
    File.binwrite(File.join(@archive, "marker.txt"), "in_archive")
    # Reset to a known-inactive state regardless of prior test order.
    PhysFS.uninstall_shim!
  end

  def teardown
    begin PhysFS.unmount(@archive) rescue nil end
    PhysFS.uninstall_shim!
    FileUtils.remove_entry(@archive) if File.directory?(@archive)
  end

  def test_shim_is_inactive_with_no_mounts_and_no_explicit_install
    refute PhysFS.shim_installed?
    # When inactive, File.exist? must NOT see archive contents — even with
    # PhysFS still holding the data internally.
    PhysFS.mount(@archive)
    PhysFS.uninstall_shim!  # force off after auto-activate
    refute File.exist?("marker.txt"),
           "Inactive shim must short-circuit to stock File.exist?"
    assert PhysFS.exist?("marker.txt"),
           "VFS module itself stays functional regardless of shim state"
  end

  def test_first_mount_auto_activates_the_shim
    refute PhysFS.shim_installed?
    PhysFS.mount(@archive)
    assert PhysFS.shim_installed?,
           "Mount should auto-activate the shim"
    assert File.exist?("marker.txt"),
           "After auto-activate, File.exist? must see archive contents"
  end

  def test_last_unmount_auto_deactivates_the_shim
    PhysFS.mount(@archive)
    assert PhysFS.shim_installed?
    PhysFS.unmount(@archive)
    refute PhysFS.shim_installed?,
           "Last unmount should auto-deactivate the shim"
  end

  def test_intermediate_unmount_keeps_shim_active
    second = Dir.mktmpdir("vfs_lc_b_")
    File.binwrite(File.join(second, "other.txt"), "x")
    begin
      PhysFS.mount(@archive)
      PhysFS.mount(second)
      assert PhysFS.shim_installed?

      PhysFS.unmount(@archive)  # one mount remains
      assert PhysFS.shim_installed?,
             "Shim must stay active while at least one archive is mounted"

      PhysFS.unmount(second)  # last unmount
      refute PhysFS.shim_installed?
    ensure
      FileUtils.remove_entry(second)
    end
  end

  def test_explicit_install_shim_returns_true_first_then_false
    assert_equal true,  PhysFS.install_shim!, "first activates"
    assert_equal false, PhysFS.install_shim!, "second is a no-op"
  end

  def test_explicit_uninstall_shim_force_disables_even_with_mounts_active
    PhysFS.mount(@archive)
    assert PhysFS.shim_installed?
    assert_equal true, PhysFS.uninstall_shim!,
                 "Force-disable must work even with an active mount"
    refute PhysFS.shim_installed?
    refute File.exist?("marker.txt"),
           "Stock File.exist? semantics restored despite mount being live"
  end
end
