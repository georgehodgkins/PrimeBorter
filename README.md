# Build instructions

1. Clone LLVM from https://github.com/llvm/llvm-project, not in this directory.
2. Softlink (ln -s) this directory into llvm-project/llvm/lib/Transforms, and add the line
`add_subdirectory(PrimeBort)` to the end of that directory's CMakeLists.txt.
3. Follow the instructions at https://llvm.org/docs/GettingStarted.html to build LLVM with
the pass attached. NB: I'd recommend running the build with as many jobs as you have logical cores until it inevitably runs out of memory in the linker stage, then running the rest of the
build with either one or two jobs in parallel.

