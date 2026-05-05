Gem::Specification.new do |s|
  s.name        = 'physfs'
  s.version     = '0.1.0'
  s.platform    = Gem::Platform::RUBY
  s.summary     = 'Ruby bindings for PhysicsFS, with an opt-in stdlib shim.'
  s.description = <<~DESC
    Ruby C-extension wrapping the PhysicsFS (libphysfs) virtual filesystem.
    Mount zip archives or directories, then transparently re-route
    File / Dir / IO / Kernel#require through the VFS via the opt-in
    `PhysFS.install_shim!` (or auto-on first PhysFS.mount).
  DESC
  s.authors     = ['Scorbutics']
  s.license     = 'MIT'
  s.required_ruby_version = '>= 3.0.0'
  s.extensions  = ['ext/physfs/extconf.rb']
  s.files       = Dir['ext/physfs/*.{h,cpp,rb}'] +
                  Dir['lib/**/*.rb'] +
                  ['README.md', 'Rakefile', 'physfs.gemspec']
  s.test_files  = Dir['tests/test_*.rb']
end
