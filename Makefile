LLVM_BUILD_LIB = /home/liz/pgo/llvm-project-10.0.0/build

NPROC = $(shell nproc)

build_func = \
	(cp LLCHintLLVMPass/LLCHintLLVMPass.cpp $(LLVM_BUILD_LIB)/../llvm/lib/Transforms/LLCHintLLVMPass/ \
	&& cp GetCFGPass/GetCFGPass.cpp $(LLVM_BUILD_LIB)/../llvm/lib/Transforms/GetCFGPass/ \
	&& cd $(LLVM_BUILD_LIB) \
	&& make -s -f CMakeFiles/Makefile2 lib/Transforms/GetCFGPass/all -j$(NPROC) \
	&& make -s -f CMakeFiles/Makefile2 lib/Transforms/LLCHintLLVMPass/all -j$(NPROC)) \

all: llchint

llchint:
	$(call build_func)

clean:
	rm $(LLVM_BUILD_LIB)/lib/LLCHintLLVMPass.so $(LLVM_BUILD_LIB)/lib/GetCFGPass.so
