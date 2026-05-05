#ifndef PHYSFS_GEM_BINDINGS_H
#define PHYSFS_GEM_BINDINGS_H

#include "RubyValueHelper.h"

// Defines the PhysFS Ruby module's instance methods (mount, read, glob, etc.)
// and wires the Shim's install/uninstall lifecycle into mount/unmount.
// Called once from Init_physfs().
void PhysFSGem_DefineModuleMethods();

#endif
