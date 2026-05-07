require 'mkmf'

# physfs gem — Ruby C-extension wrapping PhysicsFS plus an opt-in shim that
# transparently routes File / Dir / IO / Kernel#require through the VFS.

ext_name = 'physfs'

# C++17 — uses std::filesystem, std::string_view, [[noreturn]], etc.
$CXXFLAGS += ' -std=c++17 -Wall '

# Locate libphysfs. Resolution strategies in order:
#   1. PHYSFS_DIR env var (prefix containing include/ and lib/)
#   2. Homebrew prefix on macOS (Apple Silicon installs to /opt/homebrew,
#      which mkmf does not search by default)
#   3. system path (apt: libphysfs-dev)
#   4. fail with a clear message
physfs_root = ENV['PHYSFS_DIR']
if (physfs_root.nil? || physfs_root.empty?) && RUBY_PLATFORM.include?('darwin')
  brew_prefix = `brew --prefix physfs 2>/dev/null`.strip
  physfs_root = brew_prefix unless brew_prefix.empty?
end
if physfs_root && !physfs_root.empty?
  $INCFLAGS << " -I'#{physfs_root}/include'"
  $LDFLAGS  << " -L'#{physfs_root}/lib'"
end

unless have_header('physfs.h')
  abort <<~MSG
    physfs.h not found.
    Install PhysFS headers (Debian/Ubuntu: `apt install libphysfs-dev`;
    macOS: `brew install physfs`) or set PHYSFS_DIR=<prefix> where
    <prefix>/include/physfs.h exists.
  MSG
end

unless have_library('physfs')
  abort <<~MSG
    libphysfs not found.
    Install PhysFS (Debian/Ubuntu: `apt install libphysfs-dev`;
    macOS: `brew install physfs`) or set PHYSFS_DIR=<prefix> where
    <prefix>/lib/libphysfs.{a,so,dylib} exists.
  MSG
end

# Debug build: rake compile -- --enable-debug
if enable_config('debug')
  CONFIG['debugflags'] << ' -ggdb3 -O0'
  CONFIG['optflags']    = '-O0 -fno-omit-frame-pointer'
end

create_makefile(ext_name)
