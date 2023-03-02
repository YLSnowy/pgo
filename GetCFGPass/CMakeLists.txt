add_llvm_library(GetCFGPass MODULE
  GetCFGPass.cpp
  PLUGIN_TOOL
  opt
  )
add_dependencies(GetCFGPass intrinsics_gen)
