# physfs

Ruby bindings for [PhysicsFS](https://icculus.org/physfs/) plus an opt-in shim
that transparently re-routes the stdlib's `File` / `Dir` / `IO` / `Kernel#require`
through the mounted VFS.

## What it solves

You ship a game (or any Ruby app) packaged as a zip archive and want existing
code — including pokemonsdk-style scripts that do `File.read("graphics/foo.png")`
or `require "scripts/bar"` — to "just work" against the zip, **without modifying
any of the call sites**. Saves go to a real-FS write directory and shadow the
archive's stock copy on subsequent reads.

## Install

Requires a system-installed PhysicsFS:

```bash
# Debian/Ubuntu
sudo apt install libphysfs-dev

# macOS
brew install physfs
```

Then build and install the gem:

```bash
rake compile        # build the C-extension
rake install        # gem install the local build (or `gem build physfs.gemspec`)
```

If PhysFS lives at a non-standard prefix, set `PHYSFS_DIR=/path/to/prefix`
before `rake compile`.

## Usage

```ruby
require 'physfs'

# Mount the game archive at PhysFS root.
PhysFS.mount('/path/to/game.zip')

# Point writes (saves) at a real-FS dir. This dir is also mounted at
# priority 0 so any save written there shadows the corresponding stock
# entry in the archive on subsequent reads.
PhysFS.write_dir = '/path/to/saves'

# That's it — the shim is auto-activated on the first mount.
File.exist?('graphics/foo.png')   # true if it's in the archive
File.read('scripts/bar.rb')       # reads from archive, returns Ruby String
require 'scripts/bar'             # loads .rb from archive into TOPLEVEL_BINDING
Dir.glob('graphics/**/*.png')     # archive results merged with real-FS results
```

### Direct API (no stdlib shim)

If you want to query the VFS without overriding stdlib behaviour:

```ruby
PhysFS.exist?('foo.txt')
PhysFS.directory?('graphics')
PhysFS.read('config.yml')         # binary string
PhysFS.glob('graphics/*.png')     # Ruby-equivalent matching via File.fnmatch?
PhysFS.enumerate('graphics')      # direct children of the dir
```

### Lifecycle control

```ruby
PhysFS.shim_installed?            # default false; true after first mount
PhysFS.install_shim!              # force-on (idempotent)
PhysFS.uninstall_shim!            # force-off — File/Dir behave like stock Ruby again
                                  # (the prepended modules stay in the MRO but
                                  #  every override short-circuits to super)
```

The shim is ALSO auto-activated on the first `PhysFS.mount` and
auto-deactivated when the last mount goes away, so you usually don't need to
touch these.

## What the shim covers

- `File.exist?`, `File.directory?`, `File.file?`, `File.mtime`
- `File.read`, `File.binread`, `File.readlines`, `File.open`, `File.copy_stream`
- `Dir.[]` / `Dir.glob`, `Dir.entries`, `Dir.exist?`, `Dir.chdir` (real + virtual)
- `IO.copy_stream`
- `Kernel#require`, `Kernel#require_relative`

`Dir.glob` semantics match stock Ruby exactly — pattern matching is delegated
to `File.fnmatch?` so dotfile exclusion, backslash escapes, `FNM_*` flags,
character classes, brace expansion, and recursive `**` all behave the same way
they do on real-FS paths.

## Testing

```bash
rake test
```

Runs the suite (Ruby surface, shim integration, lifecycle, glob equivalence,
super-forwarding edge cases).

## License

MIT.
