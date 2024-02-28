//========================================================================
// FILE:
//    DynamicInstCounter.cpp
//
// DESCRIPTION:
//    Counts dynamic instruction calls in a module. `Dynamic` in this context means
//    runtime instruction calls (as opposed to static, i.e. compile time).
//
//    This is achieved by first doing a static analysis over the module in order to
//    find all the different opcodes present in the given program.
//    Then, for each opcode found, a global counter is injected into the module and
//    it will be incremented every time the corresponding opcode is executed at 
//    runtime.
//    Finally, results are printed by injecting a sequence of printf calls at the
//    end of the program.
//
// USAGE:
//      $ opt -load-pass-plugin <BUILD_DIR>/lib/libdynamicInstCounter.so `\`
//        -passes=-"dynamic-ic" <bitcode-file> -o instrumentend.bin
//      $ lli instrumented.bin
//
// License: MIT
//========================================================================
#include "dynamicInstCounter.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/StringMap.h"

using namespace llvm;

#define DEBUG_TYPE "dynamic-ic"

//-----------------------------------------------------------------------------
// Function for global counter injection. It declares a new global variable
// of type INT and initializes it to 0.
//-----------------------------------------------------------------------------
Constant *CreateGlobalCounter(Module &M, StringRef GlobalVarName) {
  auto &CTX = M.getContext();

  // This will insert a declaration into M
  Constant *NewGlobalVar = M.getOrInsertGlobal(GlobalVarName, IntegerType::getInt32Ty(CTX));

  // This will change the declaration into definition (and initialize to 0)
  GlobalVariable *NewGV = M.getNamedGlobal(GlobalVarName);
  NewGV->setLinkage(GlobalValue::CommonLinkage);
  NewGV->setAlignment(MaybeAlign(4));
  NewGV->setInitializer(llvm::ConstantInt::get(CTX, APInt(32, 0)));

  return NewGlobalVar;
}

//-----------------------------------------------------------------------------
// DynamicInstCounter implementation
//-----------------------------------------------------------------------------
bool DynamicInstCounter::runOnModule(Module &M) {

  // Declaration of hashmaps used inside this pass
  llvm::StringSet<> presentOpcodes;             // set of opcodes present in the program
  llvm::StringMap<Constant *> opcodeCounterMap; // linkage of opcode name to its injected counter
  llvm::StringMap<Constant *> opcodeNameMap;    // linkage of opcode name to its injected string

  // Get the global context (CTX) of the module
  auto &CTX = M.getContext();


  // STEP 1: Static analysis
  // ----------------------------------------
  // Firstly, we need to find all the different opcodes present in the given program.
  // Every instruction is analyzed and the occurring opcodes are stored in a set.
  // REMARK: Some opcodes could be added in the set but will never be called at runtime!

  // Iterate over all instructions in the module
  for (auto &F : M)
      for (auto &BB : F)
          for (auto &I : BB)
              if(presentOpcodes.find(I.getOpcodeName()) == presentOpcodes.end())
                presentOpcodes.insert(I.getOpcodeName());

  // Print out all opcodes present in the program
  errs() << "Opcodes found in given program (static analysis): \n\t";
  for (auto &opcode : presentOpcodes) {
    errs() << opcode.first().str().c_str() << "  ";
  }
  errs() << "\n";


  // STEP 2: Counters injection
  // ----------------------------------------
  // For each opcode found and stored in the set, a global counter is injected into the module:
  // it will be used to keep track of how many times the corresponding opcode is executed 
  // at runtime.
  // A global string is also injected for each opcode in order to properly print the results
  // at the end of the program.

  // Inject the following global variables for each present opcode <opcode_name> in the program:
  // -> LLVM_inst_counter_<opcode_name>:  counter for a specific opcode (linked in opcodeCounterMap)
  // -> LLVM_inst_str_<opcode_name>:      string for a specific opcode (linked in opcodeNameMap)
  for (auto &opcode : presentOpcodes) {
    std::string opcodeName = opcode.first().str().c_str();

    // Inject counter
    std::string counterName = "LLVM_inst_counter_" + opcodeName;
    Constant *countvar = CreateGlobalCounter(M, counterName);
    opcodeCounterMap[opcodeName] = countvar;

    // Inject string
    llvm::Constant *str = llvm::ConstantDataArray::getString(CTX, opcodeName);
    Constant *strvar = M.getOrInsertGlobal("LLVM_inst_str_" + opcodeName, str->getType());
    dyn_cast<GlobalVariable>(strvar)->setInitializer(str); // <-- ?!?!
    opcodeNameMap[opcodeName] = strvar;
  }


  // STEP 3: Increments injection
  // ----------------------------------------
  // For each opcode found in the module, a new instruction is injected *immediately before* 
  // to increment the corresponding opcode counter.
  // REMARK: This can be optimized with a different approach. See README for more informations

  for (auto &F : M) {
      for (auto &BB : F) {
          for (auto &I : BB) {
            std::string opcodeName = I.getOpcodeName();
            IRBuilder<> Builder(&I);
            LoadInst * ld_inst = Builder.CreateLoad(IntegerType::getInt32Ty(CTX), opcodeCounterMap[opcodeName]);
            Value * add_inst = Builder.CreateAdd(Builder.getInt32(1), ld_inst);
            Builder.CreateStore(add_inst, opcodeCounterMap[opcodeName]);
          }
      }
  }


  // STEP 4: Inject printf declaration
  // ----------------------------------------
  // Create (or _get_ in cases where it's already available) the following
  // declaration in the IR module:
  //    declare i32 @printf(i8*, ...)
  // It corresponds to the following C declaration:
  //    int printf(char *, ...)
  
  PointerType *PrintfArgTy = PointerType::getUnqual(Type::getInt8Ty(CTX));
  FunctionType *PrintfTy = FunctionType::get(IntegerType::getInt32Ty(CTX), PrintfArgTy, /*IsVarArgs=*/true);
  FunctionCallee Printf = M.getOrInsertFunction("printf", PrintfTy);

  // Set attributes as per inferLibFuncAttributes in BuildLibCalls.cpp
  Function *PrintfF = dyn_cast<Function>(Printf.getCallee());
  PrintfF->setDoesNotThrow();
  PrintfF->addParamAttr(0, Attribute::NoCapture);
  PrintfF->addParamAttr(0, Attribute::ReadOnly);


  // STEP 5: Inject printf strings (format & header)
  // ----------------------------------------
  llvm::Constant *ResultFormatStr = llvm::ConstantDataArray::getString(CTX, "%-20s %-10lu\n");
  Constant *ResultFormatStrVar = M.getOrInsertGlobal("ResultFormatStrIR", ResultFormatStr->getType());
  dyn_cast<GlobalVariable>(ResultFormatStrVar)->setInitializer(ResultFormatStr);

  std::string out = "";
  out += "=================================================\n";
  out += "LLVM Dynamic Instruction Counter results\n";
  out += "=================================================\n";
  out += "INST                 #N CALLS (runtime)\n";
  out += "-------------------------------------------------\n";
  llvm::Constant *ResultHeaderStr = llvm::ConstantDataArray::getString(CTX, out.c_str());
  Constant *ResultHeaderStrVar = M.getOrInsertGlobal("ResultHeaderStrIR", ResultHeaderStr->getType());
  dyn_cast<GlobalVariable>(ResultHeaderStrVar)->setInitializer(ResultHeaderStr);


  // STEP 6: Define a printf wrapper that will print the results
  // -----------------------------------------------------------
  FunctionType *PrintfWrapperTy = FunctionType::get(llvm::Type::getVoidTy(CTX), {}, /*IsVarArgs=*/false);
  Function *PrintfWrapperF = dyn_cast<Function>(M.getOrInsertFunction("printf_wrapper", PrintfWrapperTy).getCallee());

  // Create the entry basic block for printf_wrapper ...
  llvm::BasicBlock *RetBlock = llvm::BasicBlock::Create(CTX, "enter", PrintfWrapperF);
  IRBuilder<> Builder(RetBlock);

  // ... and start inserting calls to printf
  // (printf requires i8*, so cast the input strings accordingly)
  llvm::Value *ResultHeaderStrPtr = Builder.CreatePointerCast(ResultHeaderStrVar, PrintfArgTy);
  llvm::Value *ResultFormatStrPtr = Builder.CreatePointerCast(ResultFormatStrVar, PrintfArgTy);

  Builder.CreateCall(Printf, {ResultHeaderStrPtr});

  LoadInst *LoadCounter;
  for (auto &opcode : opcodeCounterMap) {
    std::string opcodeName = opcode.first().str().c_str();
    LoadCounter = Builder.CreateLoad(IntegerType::getInt32Ty(CTX), opcodeCounterMap[opcodeName]);
    Builder.CreateCall(Printf, {ResultFormatStrPtr, opcodeNameMap[opcodeName], LoadCounter});
  }

  // Finally, insert return instruction
  Builder.CreateRetVoid();


  // STEP 7: Call `printf_wrapper` at the very end of this module
  // ------------------------------------------------------------
  appendToGlobalDtors(M, PrintfWrapperF, /*Priority=*/0);

  return true;
}

PreservedAnalyses DynamicInstCounter::run(llvm::Module &M, llvm::ModuleAnalysisManager &) {
  bool Changed = runOnModule(M);
  return (Changed ? llvm::PreservedAnalyses::none() : llvm::PreservedAnalyses::all());
}

//-----------------------------------------------------------------------------
// Register the pass with the "new" pass manager
//-----------------------------------------------------------------------------
llvm::PassPluginLibraryInfo getDynamicCallCounterPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "dynamic-ic", LLVM_VERSION_STRING,
          [](PassBuilder &PB) {
            PB.registerPipelineParsingCallback(
                [](StringRef Name, ModulePassManager &MPM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  if (Name == "dynamic-ic") {
                    MPM.addPass(DynamicInstCounter());
                    return true;
                  }
                  return false;
                });
          }};
}

extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() {
  return getDynamicCallCounterPluginInfo();
}
