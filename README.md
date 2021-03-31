# Build instructions

1. Clone LLVM from https://github.com/llvm/llvm-project, not in this directory.

2. Softlink (ln -s) the PrimeBortDetector subdirectory into llvm-project/llvm/lib/Transforms 
and llvm-project/llvm/include/llvm/Transforms.

3. Add the following lines to various config files near similar-looking lines (all under llvm-project):
- `add_subdirectory(PrimeBortDetector)` -> llvm/lib/Transforms/CMakeLists.txt
- `#include "llvm/Transforms/PrimeBortDetector/PrimeBortDetector.h` AND
- `(void) llvm::createPrimeBortDetectorPass();` -> llvm/include/llvm/LinkAllPasses.h
- `#include "llvm/Transforms/PrimeBortDetector/PrimeBortDetector.h` -> llvm/lib/Passes/PassBuilder.cpp
- `MODULE_PASS("primebort", PrimeBortDetectorPass())` -> llvm/lib/Passes/PassRegistry.def
- Add PrimeBort to the `LLVM_LINK_COMPONENTS` entry in llvm/tools/bugpoint/CMakeLists.txt

4. Follow the instructions at https://llvm.org/docs/GettingStarted.html to build LLVM with
the pass attached. NB: I'd recommend running the build with as many jobs as you have logical cores until it inevitably runs out of memory in the linker stage, then running the rest of the
build with either one or two jobs in parallel.

5. If the build fails because it cannot link to the PrimeBortDetector module, add "PrimeBort"
to the `required_libraries` list in llvm/lib/Passes/LLVMBuild.txt and
llvm/tools/bugpoint/LLVMBuild.txt, and add "PrimeBortDetector" to the `subdirectories` list
in llvm/lib/Transforms/LLVMBuild.txt.

