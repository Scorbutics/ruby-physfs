#ifndef PHYSFS_GEM_SHIM_H
#define PHYSFS_GEM_SHIM_H

#include "RubyValueHelper.h"

// Defines PhysFS.install_shim! / .uninstall_shim! / .shim_installed?
// — these toggle the C-implemented overrides for File / Dir / IO and
// Kernel#require that re-route filesystem calls through the VFS.
//
// Lifecycle (driven by Bindings.cpp's mount/unmount):
//   - Requiring 'physfs' alone has zero effect on File/Dir behaviour.
//   - The first PhysFS.mount() auto-activates the shim.
//   - The last PhysFS.unmount() auto-deactivates it.
//   - install_shim! / uninstall_shim! are explicit overrides callable any
//     time (useful for tests, or for tools that want the shim before any
//     mount, or that want to force-disable it while mounts remain).
//
// Implementation note: Ruby's prepended-module chain is append-only — there
// is no rb_unprepend_module. Activation prepends once on first install;
// subsequent toggles flip a boolean that every override checks at the top
// (one branch + rb_call_super when inactive, which is exactly the stock
// Ruby path). Cost when inactive is invisible in real workloads.
void PhysFSShim_DefineRubyMethods();

// C++ surface for the mount/unmount lifecycle hooks in Bindings.cpp.
// Returns true on state transition, false if already in the requested state.
bool PhysFSShim_Activate();
bool PhysFSShim_Deactivate();

// Reserved hook: called after every mount/unmount to drop any cached
// path enumerations. Currently a no-op (no caching at the moment), but
// kept in the C++ ABI so a future optimization can plug in here.
void PhysFSShim_InvalidatePathCache();

// Ruby-equivalent glob over the VFS. Walks segment-by-segment via
// physfs_gem::enumerate and matches each segment with File.fnmatch?, so
// per-segment semantics (dotfile exclusion, FNM_* flags, backslash escapes,
// char classes) match Ruby's Dir.glob exactly. The `**` segment recurses
// with Dir.glob semantics (zero-or-more directories). Trailing-slash
// patterns are dir-only and emit results with `/` appended.
VALUE PhysFSShim_Glob(VALUE pattern, int extra_flags);

#endif
