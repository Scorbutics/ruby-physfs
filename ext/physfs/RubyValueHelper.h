#ifndef PHYSFS_GEM_RUBY_VALUE_HELPER_H
#define PHYSFS_GEM_RUBY_VALUE_HELPER_H

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wregister"

#include "ruby.h"

#pragma GCC diagnostic pop

// Cast helper used at rb_define_method sites — Ruby's signature varies enough
// across CRuby versions that a permissive cast is the cleanest portable fix.
#define _rbf (VALUE (*)(...))

#endif
