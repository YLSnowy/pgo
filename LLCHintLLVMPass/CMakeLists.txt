add_llvm_library(LLCHintLLVMPass MODULE
  LLCHintLLVMPass.cpp
  PLUGIN_TOOL
  opt
  )
add_dependencies(LLCHintLLVMPass intrinsics_gen)
