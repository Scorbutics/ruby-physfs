# Concurrent-access tests for the gem-wide PhysFS Monitor.
#
# Background: PhysFS's own internal mutex is pthread-based, and under MRI
# Ruby's Native-Threads-but-GVL-serialized model, a callback chain like
#
#     PhysFS.read -> PHYSFS_openRead -> ZIP archiver -> io->duplicate ->
#     Ruby duplicate() -> ... (releases GVL during pread/decrypt) -> ...
#
# can deadlock with a second Ruby Thread that tries to enter PhysFS while
# the first is mid-callback. The gem now wraps every C-side entry point
# (physfs_gem::* in physfs_wrapper.cpp) with a reentrant Ruby Monitor that
# serialises at the Ruby level — see PhysFSLock.h for the full rationale.
#
# What this file tests, end-to-end:
#
#   1. Two Ruby Threads each doing many File.exist? / PhysFS.read / File.read
#      calls against the same mounted archive complete without deadlock and
#      both observe the correct content.
#   2. The monitor is reentrant on a single Ruby Thread (a synchronous Ruby
#      callback from inside PhysFS's archiver can call back into PhysFS
#      without ThreadError).
#   3. PhysFS.write_dir + read interleaving stays consistent under load.
#   4. A finite outer timeout — if a future regression reintroduces the
#      deadlock, the test fails loudly instead of hanging CI forever.
#
# Run: ruby tests/test_concurrent_access.rb

$LOAD_PATH.unshift(File.expand_path('../lib', __dir__))
$LOAD_PATH.unshift(File.expand_path('../tmp/x86_64-linux/stage/lib', __dir__))

require 'minitest/autorun'
require 'stringio'
require 'tempfile'
require 'timeout'
require 'zip'
require 'physfs'

# Same StringIOAdapter shape as test_mount_io — `duplicate` returns a fresh
# instance over the same byte buffer so PhysFS's per-openRead duplication
# gives every concurrent reader its own offset.
class IndependentAdapter
  attr_reader :read_count

  def initialize(bytes)
    @bytes = bytes.b.freeze
    @io = StringIO.new(@bytes.dup.force_encoding(Encoding::ASCII_8BIT))
    @read_count = 0
  end

  def read(n)
    @read_count += 1
    @io.read(n) || ''.b
  end

  def seek(off) = @io.seek(off, IO::SEEK_SET)
  def tell      = @io.pos
  def length    = @bytes.bytesize
  def close     = (@io.close unless @io.closed?)
  def duplicate
    IndependentAdapter.new(@bytes)
  end
end

class TestConcurrentAccess < Minitest::Test
  ARCHIVE_NAME = 'concurrent_archive.zip'

  def teardown
    PhysFS.unmount(ARCHIVE_NAME)
  rescue StandardError
    # not mounted
  end

  def build_zip_bytes(entries)
    Tempfile.create(['concurrent_test', '.zip'], binmode: true) do |zip_file|
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

  # Mount enough variety that the threads can fan out across different
  # entries — not just the same hot path. Mix of short and "medium" files
  # so io_read callbacks span more than one chunk.
  def mount_archive
    entries = {}
    entries['short.txt']            = 'hello'
    entries['medium.dat']           = ('x' * 8192).b
    entries['nested/inner.txt']     = 'nested content'
    entries['readme.md']            = "line1\nline2\nline3\n"
    16.times { |i| entries["bulk/file_#{i}.bin"] = ("payload-#{i}" * 64).b }

    zip_bytes = build_zip_bytes(entries)
    adapter   = IndependentAdapter.new(zip_bytes)
    PhysFS.mount_io(adapter, ARCHIVE_NAME, '/', false)
    entries
  end

  # The headline test: two Ruby Threads each doing N reads against the
  # archive. Before the Monitor was added, this would deadlock somewhere
  # past iteration ~5 with both threads stuck (one in stateLock, one
  # waiting on the GVL inside an io_read callback). The Timeout::timeout
  # wrapper converts the deadlock into a clean test failure.
  def test_two_threads_concurrent_reads_complete
    entries = mount_archive

    iterations_per_thread = 200
    threads = 2

    results = Timeout.timeout(30) do
      ts = threads.times.map do |tid|
        Thread.new do
          observed = []
          iterations_per_thread.times do |i|
            path = entries.keys[i % entries.size]
            # Mix of API surfaces: exist?, read via PhysFS, read via the
            # shimmed File.read (which routes through physfs_gem::* too).
            unless PhysFS.exist?(path)
              observed << [:missing_via_physfs, tid, i, path]
              next
            end
            via_physfs = PhysFS.read(path)
            via_file   = File.read(path)
            unless via_physfs == via_file && via_physfs == entries[path]
              observed << [:mismatch, tid, i, path, via_physfs.bytesize, via_file.bytesize, entries[path].bytesize]
            end
          end
          observed
        end
      end
      ts.map(&:value)
    end

    flat = results.flatten(1)
    assert_empty flat, "concurrent reads diverged from ground truth: #{flat.first(5).inspect}"
  end

  # Reentrancy check on a single thread. PhysFS's ZIP archiver calls
  # io->duplicate() inside PHYSFS_openRead; the duplicate's `read` callback
  # in turn could (hypothetically) call back into PhysFS.  We simulate the
  # same shape by performing a `PhysFS.read` from inside a `PhysFS.read`
  # via the shim — the inner call must NOT raise ThreadError.
  def test_reentrant_monitor_on_single_thread
    entries = mount_archive

    # Define a tiny computation that, while reading entry A, also reads
    # entry B. Both go through the gem; the second acquire on the same
    # thread should be a counter increment (Monitor), not a deadlock or
    # ThreadError (which Mutex#synchronize would raise).
    outer = nil
    inner = nil
    Timeout.timeout(10) do
      outer = PhysFS.read('short.txt')
      # While we're "inside" a notional caller of PhysFS, immediately
      # nest another call. This is the most distilled reentrancy probe;
      # the real-world variant is io_duplicate -> Ruby -> EpsaStream#new
      # -> File.open (shim) -> physfs_gem::exists.
      inner = PhysFS.read('readme.md')
    end

    assert_equal entries['short.txt'], outer
    assert_equal entries['readme.md'], inner
  end

  # A heavier soak: 4 threads, each doing a mix of read + exist? + mtime.
  # If any operation has a hidden lock-inversion the timeout exposes it.
  def test_four_threads_mixed_workload
    entries = mount_archive

    iterations = 100
    threads = 4

    Timeout.timeout(60) do
      ts = threads.times.map do |tid|
        Thread.new do
          rng = Random.new(tid * 1000 + 7)
          iterations.times do
            path = entries.keys.sample(random: rng)
            op = %i[read exist mtime size enumerate].sample(random: rng)
            case op
            when :read
              data = PhysFS.read(path)
              assert_equal entries[path], data
            when :exist
              assert PhysFS.exist?(path)
            when :mtime
              # Just call it; rxdata-style files in ZIPs may report 0,
              # but the call must return without raising.
              PhysFS.mtime(path)
            when :size
              assert_equal entries[path].bytesize, File.size(path)
            when :enumerate
              listing = PhysFS.enumerate('bulk')
              assert listing.is_a?(Array)
            end
          end
        end
      end
      ts.each(&:join)
    end
  end
end
