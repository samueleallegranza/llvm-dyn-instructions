
# LLVM Dynamic Instruction Counter

**llvm-dyn-counter** implements a pass for LLVM which counts the number of instructions a program calls at runtime.
To achieve so, it performs a dynamic analysis over the resulting LLVM-IR of a given program.

## Requirements
This project has been tested on **Ubuntu 22.04**. In order to build llvm-dyn-counter you will need:
  * LLVM 17
  * C++ compiler that supports C++17
  * CMake 3.20 or higher

In order to run this pass, you will need:
  * **clang-17** (to generate input LLVM files)
  * [**opt**](http://llvm.org/docs/CommandGuide/opt.html) (to run the passes)

## Building
First of all, clone this repository.
```
git clone https://github.com/samueleallegranza/llvm-dyn-instructions/
```
It's important to set `LLVM_DIR` and `DYNINST_DIR` system variables which should both point to the root of the llvm installation directory and the llvm-dyn-counter directory (this repository).
```
export LLVM_DIR=<path/to/llvm/17>
export DYNINST_DIR=<path/to/llvm-dyn-counter>
```
Tip: The default llvm installation directory is `/usr/lib/llvm-17` on linux-based systems.

Next step is to build the project with cmake. Assming you are inside the root folder of this repository:
```
mkdir build
cd build
cmake -DLT_LLVM_INSTALL_DIR=$LLVM_DIR ..
make
```

## Usage
Let's try to run this pass on the `inputs/input_demo.c` program.
```
# Generate an LLVM file to analyze
$LLVM_DIR/bin/clang -emit-llvm -c $DYNINST_DIR/inputs/input_demo.c -o input.bc
# Run the pass through opt
$LLVM_DIR/bin/opt -load-pass-plugin=$DYNINST_DIR/build/lib/libdynamicInstCounter.so -passes="dynamic-ic" input.bc -o input
```
llvm-dyn-counter will print out a simple static analysis of the code listing every type of instruction found inside the C program. To perform the dynamic analysis:
```
$LLVM_DIR/bin/lli  ./input
```
Which should generate the following output:
```
=================================================
INST                 #N CALLS (runtime)
-------------------------------------------------
add                  7         
mul                  1         
ret                  2         
call                 1         
load                 18        
br                   11        
alloca               9         
store                14        
icmp                 4         
```

## Implementation
This pass is implemented as follows:
- **STEP 1**: Firstly, we need to find all the different opcodes present in the given program.
  Every instruction is analyzed and the occurring opcodes are stored in a set.\
  *_Remark_*: Some opcodes could be added in the set but will never be called at runtime!

- **STEP 2**: For each opcode found and stored in the set, a global counter is injected into the module:
  it will be used to keep track of how many times the corresponding opcode is executed at runtime.
  A global string is also injected for each opcode in order to properly print the results at the end of the program.

- **STEP 3**: For each opcode found in the module, a new set of instructions is injected *immediately before* to increment the corresponding opcode counter.\
  *_Remark_*: This can be optimized with a different approach! See **Optimizations** section to learn more.

- **STEP 4/.../7**: A sequence of `printf`s are injected at the very end of the module in order to display the results of the dynamic analysis.


## Optimizations

This solution works fine to analyze small programs, but it could result very slow for large & compute-heavy programs. This happens because for **every instruction call**, the resulting binary obtained with this pass will do an increment over the counter of the corresponding opcode.
However, this is not necessary. LLVM decomposes every function into different Basic Blocks, which are a straight-line code sequence with no branches in except to the entry and no branches out except at the exit.
To optimize the pass:
- Perform a static analysis over each Basic Block, counting the number of each instruction call.
- Performa a dynamic analysis of the program counting, for each Basic Block, the number of times it has been executed at runtime
- Combine the results of the static analysis and the results of the dynamic analysis by multiplyig

