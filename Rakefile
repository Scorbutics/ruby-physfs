require 'rake/extensiontask'

ext_name = 'physfs'

spec = Gem::Specification.new do |s|
  s.name        = ext_name
  s.platform    = Gem::Platform::RUBY
  s.version     = '0.1.0'
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
  s.extensions  = FileList['ext/physfs/extconf.rb']
  s.files       = FileList['ext/physfs/*.{h,cpp,rb}', 'lib/*.rb', 'README.md']
end

Gem::PackageTask.new(spec) {}

Rake::ExtensionTask.new(ext_name, spec) do |ext|
  ext.lib_dir = 'lib/physfs'
end

desc 'Run all unit tests'
task :test => :compile do
  Dir['tests/test_*.rb'].each { |f| ruby "-W0 #{f}" or fail "test failed: #{f}" }
end

task :default => :test
