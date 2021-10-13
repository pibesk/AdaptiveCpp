/*
 * This file is part of hipSYCL, a SYCL implementation based on CUDA/HIP
 *
 * Copyright (c) 2021 Aksel Alpay and contributors
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "hipSYCL/compiler/LoopSplitterInlining.hpp"
#include "hipSYCL/compiler/IRUtils.hpp"
#include "hipSYCL/compiler/SplitterAnnotationAnalysis.hpp"

#include "hipSYCL/common/debug.hpp"

#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/Analysis/LoopAccessAnalysis.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/IR/Dominators.h>
#include <llvm/Transforms/Utils/Cloning.h>

namespace {

bool inlineCallsInBasicBlock(llvm::BasicBlock &BB, const llvm::SmallPtrSet<llvm::Function *, 8> &SplitterCallers,
                             hipsycl::compiler::SplitterAnnotationInfo &SAA) {
  bool Changed = false;
  bool LastChanged;

  do {
    LastChanged = false;
    for (auto &I : BB) {
      if (auto *CallI = llvm::dyn_cast<llvm::CallBase>(&I)) {
        if (CallI->getCalledFunction()) {
          if (SplitterCallers.find(CallI->getCalledFunction()) != SplitterCallers.end() &&
              !SAA.isSplitterFunc(CallI->getCalledFunction())) {
            LastChanged = hipsycl::compiler::utils::checkedInlineFunction(CallI, "[LoopSplitterInlining]");
            if (LastChanged)
              break;
          } else if (SAA.isSplitterFunc(CallI->getCalledFunction()) &&
                     CallI->getCalledFunction()->getName() != hipsycl::compiler::BarrierIntrinsicName) {
            HIPSYCL_DEBUG_INFO << "[LoopSplitterInlining] Replace barrier with intrinsic: "
                               << CallI->getCalledFunction()->getName() << "\n";
            hipsycl::compiler::utils::createBarrier(CallI, SAA);
            CallI->eraseFromParent();
            LastChanged = true;
            break;
          }
        }
      }
    }
    if (LastChanged)
      Changed = true;
  } while (LastChanged);

  return Changed;
}

//! \pre all contained functions are non recursive!
// todo: have a recursive-ness termination
bool inlineCallsInLoop(llvm::Loop *&L, const llvm::SmallPtrSet<llvm::Function *, 8> &SplitterCallers,
                       hipsycl::compiler::SplitterAnnotationInfo &SAA, llvm::LoopInfo &LI, llvm::DominatorTree &DT) {
  bool Changed = false;
  bool LastChanged = false;

  llvm::BasicBlock *B = L->getBlocks()[0];
  llvm::Function &F = *B->getParent();

  do {
    LastChanged = false;
    for (auto *BB : L->getBlocks()) {
      LastChanged = inlineCallsInBasicBlock(*BB, SplitterCallers, SAA);
      if (LastChanged)
        break;
    }
    if (LastChanged) {
      Changed = true;
      L = hipsycl::compiler::utils::updateDtAndLi(LI, DT, B, F);
    }
  } while (LastChanged);

  return Changed;
}

//! \pre all contained functions are non recursive!
// todo: have a recursive-ness termination
bool inlineCallsInFunction(llvm::Function &F, const llvm::SmallPtrSet<llvm::Function *, 8> &SplitterCallers,
                           hipsycl::compiler::SplitterAnnotationInfo &SAA, llvm::LoopInfo &LI,
                           llvm::DominatorTree &DT) {
  bool Changed = false;
  bool LastChanged;

  do {
    LastChanged = false;
    for (auto &BB : F) {
      LastChanged = inlineCallsInBasicBlock(BB, SplitterCallers, SAA);
      Changed |= LastChanged;
      if (LastChanged)
        break;
    }
  } while (LastChanged);

  return Changed;
}

// todo: have a recursive-ness termination
bool fillTransitiveSplitterCallers(llvm::Function &F, const hipsycl::compiler::SplitterAnnotationInfo &SAA,
                                   llvm::SmallPtrSet<llvm::Function *, 8> &FuncsWSplitter);
bool fillTransitiveSplitterCallers(llvm::ArrayRef<llvm::BasicBlock *> Blocks,
                                   const hipsycl::compiler::SplitterAnnotationInfo &SAA,
                                   llvm::SmallPtrSet<llvm::Function *, 8> &FuncsWSplitter) {
  bool Found = false;
  for (auto *BB : Blocks) {
    for (auto &I : *BB) {
      if (auto *CallI = llvm::dyn_cast<llvm::CallBase>(&I)) {
        if (CallI->getCalledFunction() &&
            fillTransitiveSplitterCallers(*CallI->getCalledFunction(), SAA, FuncsWSplitter))
          Found = true;
      }
    }
  }
  return Found;
}

//! \pre \a F is not recursive!
// todo: have a recursive-ness termination
bool fillTransitiveSplitterCallers(llvm::Function &F, const hipsycl::compiler::SplitterAnnotationInfo &SAA,
                                   llvm::SmallPtrSet<llvm::Function *, 8> &FuncsWSplitter) {
  if (F.isDeclaration() && !F.isIntrinsic()) {
    HIPSYCL_DEBUG_WARNING << "[LoopSplitterInlining] " << F.getName() << " is not defined!\n";
  }
  if (SAA.isSplitterFunc(&F)) {
    FuncsWSplitter.insert(&F);
    return true;
  } else if (FuncsWSplitter.find(&F) != FuncsWSplitter.end())
    return true;

  llvm::SmallVector<llvm::BasicBlock *, 8> Blocks;
  std::transform(F.begin(), F.end(), std::back_inserter(Blocks), [](auto &BB) { return &BB; });

  if (fillTransitiveSplitterCallers(Blocks, SAA, FuncsWSplitter)) {
    FuncsWSplitter.insert(&F);
    return true;
  }
  return false;
}

bool fillTransitiveSplitterCallers(llvm::Loop &L, const hipsycl::compiler::SplitterAnnotationInfo &SAA,
                                   llvm::SmallPtrSet<llvm::Function *, 8> &FuncsWSplitter) {
  return fillTransitiveSplitterCallers(L.getBlocks(), SAA, FuncsWSplitter);
}

bool inlineSplitter(llvm::Loop *L, llvm::LoopInfo &LI, llvm::DominatorTree &DT,
                    hipsycl::compiler::SplitterAnnotationInfo &SAA) {
  if (!hipsycl::compiler::utils::isWorkItemLoop(*L)) {
    HIPSYCL_DEBUG_INFO << "[LoopSplitterInlining] not work-item loop!" << L << "\n";
    return false;
  }

  llvm::SmallPtrSet<llvm::Function *, 8> SplitterCallers;
  if (!fillTransitiveSplitterCallers(*L, SAA, SplitterCallers)) {
    HIPSYCL_DEBUG_INFO << "[LoopSplitterInlining] transitively no splitter found." << L << "\n";
    return false;
  }
  return inlineCallsInLoop(L, SplitterCallers, SAA, LI, DT);
}

bool inlineSplitter(llvm::Function &F, llvm::LoopInfo &LI, llvm::DominatorTree &DT,
                    hipsycl::compiler::SplitterAnnotationInfo &SAA) {
  bool Changed = false;

  if (auto *L = hipsycl::compiler::utils::getSingleWorkItemLoop(LI))
    Changed |= inlineSplitter(L, LI, DT, SAA);
  else {
    llvm::SmallPtrSet<llvm::Function *, 8> SplitterCallers;
    if (!fillTransitiveSplitterCallers(F, SAA, SplitterCallers)) {
      HIPSYCL_DEBUG_INFO << "[LoopSplitterInlining] transitively no splitter found in kernel." << F.getName() << "\n";
      return Changed;
    }
    Changed |= inlineCallsInFunction(F, SplitterCallers, SAA, LI, DT);
  }

  return Changed;
}
} // namespace

void hipsycl::compiler::LoopSplitterInliningPassLegacy::getAnalysisUsage(llvm::AnalysisUsage &AU) const {
  AU.addRequired<SplitterAnnotationAnalysisLegacy>();
  AU.addPreserved<SplitterAnnotationAnalysisLegacy>();
  AU.addRequired<llvm::LoopInfoWrapperPass>();
  AU.addRequired<llvm::DominatorTreeWrapperPass>();
}

bool hipsycl::compiler::LoopSplitterInliningPassLegacy::runOnFunction(llvm::Function &F) {
  auto &LI = getAnalysis<llvm::LoopInfoWrapperPass>().getLoopInfo();
  auto &DT = getAnalysis<llvm::DominatorTreeWrapperPass>().getDomTree();
  auto &SAA = getAnalysis<SplitterAnnotationAnalysisLegacy>().getAnnotationInfo();

  if (!SAA.isKernelFunc(&F))
    return false;

  return inlineSplitter(F, LI, DT, SAA);
}

char hipsycl::compiler::LoopSplitterInliningPassLegacy::ID = 0;
llvm::PreservedAnalyses hipsycl::compiler::LoopSplitterInliningPass::run(llvm::Function &F,
                                                                         llvm::FunctionAnalysisManager &AM) {
  const auto *MAMProxy = AM.getCachedResult<llvm::ModuleAnalysisManagerFunctionProxy>(F);
  auto *SAA = MAMProxy->getCachedResult<SplitterAnnotationAnalysis>(*F.getParent());
  if (!SAA) {
    llvm::errs() << "[LoopSplitterInlining] SplitterAnnotationAnalysis not cached.\n";
    return llvm::PreservedAnalyses::all();
  }
  if (!SAA->isKernelFunc(&F))
    return llvm::PreservedAnalyses::all();

  auto &LI = AM.getResult<llvm::LoopAnalysis>(F);
  auto &DT = AM.getResult<llvm::DominatorTreeAnalysis>(F);

  if (!inlineSplitter(F, LI, DT, *SAA))
    return llvm::PreservedAnalyses::all();

  llvm::PreservedAnalyses PA;
  PA.preserve<SplitterAnnotationAnalysis>();
  return PA;
}
