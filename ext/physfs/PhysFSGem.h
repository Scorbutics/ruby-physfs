#ifndef PHYSFS_GEM_H
#define PHYSFS_GEM_H

#include "RubyValueHelper.h"

// Top-level Ruby module for the gem and its error class. Defined once in
// Init_physfs(); referenced by RubyVFS.cpp and RubyVFSShim.cpp to register
// methods + sub-modules under it.
extern VALUE rb_mPhysFS;
extern VALUE rb_ePhysFSError;

#endif
