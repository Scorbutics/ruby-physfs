# Top-level loader: pulls in the compiled C-extension which defines the
# `PhysFS` module and its methods. Users `require 'physfs'` and then call
# `PhysFS.mount(...)` etc.

require 'physfs/physfs'
