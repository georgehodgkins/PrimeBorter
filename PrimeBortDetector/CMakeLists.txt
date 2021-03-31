add_llvm_component_library( LLVMPrimeBort
  PrimeBortDetector.cpp

  ADDITIONAL_HEADER_DIRS
  ${LLVM_MAIN_INCLUDE_DIR}/llvm/Transforms
  ${LLVM_MAIN_INCLUDE_DIR}/llvm/Transforms/PrimeBortDetector

  DEPENDS
  intrinsics_gen
  )

add_llvm_library( PrimeBortDetector MODULE
	PrimeBortDetector.cpp
	)
