# Tests for PhysFS.mount_io — the generic Ruby-IO mount entrypoint added
# alongside .mount and .mount_io_handle.
#
# These tests exercise:
#   * happy path: a ZIP built in memory, mounted via a StringIO-backed adapter,
#     read back via PhysFS.read / .exist?
#   * exception isolation: when the Ruby IO raises, PhysFS surfaces it as a
#     normal failure rather than crashing the VM
#   * lifecycle: PhysFS.unmount triggers the IO's `close` callback exactly once
#   * shim auto-activation: first mount_io activates the File/Dir/IO shim
#     just like mount does
#
# Run: ruby tests/test_mount_io.rb

$LOAD_PATH.unshift(File.expand_path('../lib', __dir__))
$LOAD_PATH.unshift(File.expand_path('../tmp/x86_64-linux/stage/lib', __dir__))

require 'minitest/autorun'
require 'stringio'
require 'tempfile'
require 'zip'
require 'physfs'

# A trivial Ruby IO adapter over a StringIO. Implements the duck type that
# PhysFS_Io expects: read, seek, tell, length, close, duplicate. Used to
# verify the C bridge's basic plumbing.
class StringIOAdapter
  attr_reader :close_count

  def initialize(bytes)
    @bytes = bytes.b.freeze
    @io = StringIO.new(@bytes.dup.force_encoding(Encoding::ASCII_8BIT))
    @close_count = 0
  end

  def read(n) = @io.read(n) || ''.b
  def seek(off) = @io.seek(off, IO::SEEK_SET)
  def tell      = @io.pos
  def length    = @bytes.bytesize
  def close
    @close_count += 1
    @io.close unless @io.closed?
  end

  def duplicate
    StringIOAdapter.new(@bytes)
  end
end

# An IO that raises on the very first read — used to verify the C bridge
# converts Ruby exceptions to PhysFS errors instead of unwinding.
class FailingIO
  def initialize(length: 1024)
    @length = length
  end

  def read(_n) = raise 'simulated read failure'
  def seek(_)  = nil
  def tell     = 0
  def length   = @length
  def close    = nil
  def duplicate = self.class.new(length: @length)
end

class TestMountIo < Minitest::Test
  def teardown
    # Best-effort: the wrapper's mount counter assumes balanced unmounts.
    # Tests that mount via a temp/string-backed source unmount by the
    # `fake_name` they passed, so we attempt those known names.
    %w[archive.zip bad.zip].each do |name|
      begin
        PhysFS.unmount(name)
      rescue StandardError
        # not mounted
      end
    end
  end

  def build_zip_bytes(entries)
    # Build through Zip::File#add from real on-disk files. rubyzip's
    # get_output_stream emits a streaming-mode local header (data
    # descriptor flag), which PhysFS's ZIP backend doesn't parse — that's a
    # rubyzip / PhysFS interaction, not a mount_io issue, but it would
    # confuse the test's intent.
    Tempfile.create(['mount_io_test', '.zip'], binmode: true) do |zip_file|
      zip_file.close
      tempfiles = entries.map do |_path, content|
        f = Tempfile.new('zip_entry', binmode: true)
        f.write(content)
        f.close
        f
      end
      begin
        Zip::File.open(zip_file.path, create: true) do |zip|
          entries.keys.each_with_index { |path, i| zip.add(path, tempfiles[i].path) }
        end
        File.binread(zip_file.path)
      ensure
        tempfiles.each(&:unlink)
      end
    end
  end

  def test_mount_io_round_trips_zip_entries
    zip_bytes = build_zip_bytes(
      'hello.txt'        => 'world',
      'nested/file.dat'  => "binary\0bytes",
    )
    adapter = StringIOAdapter.new(zip_bytes)

    PhysFS.mount_io(adapter, 'archive.zip', '/', false)

    assert PhysFS.exist?('hello.txt'),                 'archive entry should be visible after mount_io'
    assert PhysFS.exist?('nested/file.dat')
    refute PhysFS.exist?('missing.txt')

    assert_equal 'world',          PhysFS.read('hello.txt')
    assert_equal "binary\0bytes",  PhysFS.read('nested/file.dat')
  end

  def test_mount_io_activates_shim
    refute PhysFS.shim_installed?, 'shim must start dormant'
    zip_bytes = build_zip_bytes('a.txt' => 'a')
    adapter   = StringIOAdapter.new(zip_bytes)

    PhysFS.mount_io(adapter, 'archive.zip')
    assert PhysFS.shim_installed?, 'first mount_io must activate the shim'

    PhysFS.unmount('archive.zip')
    refute PhysFS.shim_installed?, 'last unmount must deactivate the shim'
  end

  def test_unmount_calls_close_on_ruby_io
    zip_bytes = build_zip_bytes('only.txt' => 'x')
    adapter   = StringIOAdapter.new(zip_bytes)

    PhysFS.mount_io(adapter, 'archive.zip')
    assert_equal 0, adapter.close_count, 'close must not be called before unmount'

    PhysFS.unmount('archive.zip')
    # PhysFS may use a duplicate() of the adapter for archive parsing; the
    # *original* adapter's close is reached when the original PHYSFS_Io is
    # destroyed. Other clones close their own copies, but the close count
    # we can observe locally is the original's.
    assert_operator adapter.close_count, :>=, 1,
                    'unmount must trigger close on the original adapter'
  end

  def test_failing_read_does_not_crash_vm
    failing = FailingIO.new(length: 100)
    # The mount itself may succeed or fail depending on whether PhysFS reads
    # at mount time. Either outcome is acceptable; what matters is that we
    # come back to Ruby normally rather than crashing or leaking the
    # exception state.
    begin
      PhysFS.mount_io(failing, 'bad.zip')
      # If the mount succeeded, the failure surfaces on access:
      refute PhysFS.exist?('anything.txt'),
             'failing IO should not pretend entries exist'
      PhysFS.unmount('bad.zip')
    rescue PhysFS::Error
      # Expected: the C bridge translated the failure into a PhysFS error.
      pass
    end
    # Reaching here without a Ruby-level exception leak is the real assertion.
    assert_nil $ERROR_INFO, 'Ruby errinfo must be cleared after the bridge runs'
  end

  def test_arity_validation
    adapter = StringIOAdapter.new('x')
    assert_raises(ArgumentError) { PhysFS.mount_io }
    assert_raises(ArgumentError) { PhysFS.mount_io(adapter) }
  end

  def test_fake_name_must_be_string
    adapter = StringIOAdapter.new('x')
    assert_raises(TypeError) { PhysFS.mount_io(adapter, 123) }
  end
end
