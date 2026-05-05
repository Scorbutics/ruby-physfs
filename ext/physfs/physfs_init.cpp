// Ruby C-extension entrypoint. Defines the top-level PhysFS module and
// PhysFS::Error class, then delegates to Bindings.cpp / Shim.cpp to wire
// up methods and the optional File/Dir/Kernel#require shim.

#include "PhysFSGem.h"
#include "Bindings.h"
#include "RubyValueHelper.h"

VALUE rb_mPhysFS = Qnil;
VALUE rb_ePhysFSError = Qnil;

extern "C" {
	void Init_physfs() {
		rb_mPhysFS = rb_define_module("PhysFS");
		rb_ePhysFSError = rb_define_class_under(rb_mPhysFS, "Error", rb_eStandardError);
		PhysFSGem_DefineModuleMethods();
	}
}
