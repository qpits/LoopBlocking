#ifndef LOOPBLOCKING_HPP
#define LOOPBLOCKING_HPP

#include <llvm/ADT/STLExtras.h>
#ifndef L1_DCACHE_LINESIZE
#define L1_CACHE_LINESIZE 64
#endif

#ifndef L1_DCACHE_SIZE
#define L1_DCACHE_SIZE 65536
#endif

#ifndef MAX_NEST_SIZE
#define MAX_NEST_SIZE 3u
#endif

#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/ScalarEvolution.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Analysis/DomTreeUpdater.h>
#include <llvm/Support/raw_ostream.h>
#include <vector>
#include <optional>
#include <llvm/Analysis/LoopNestAnalysis.h>
#include <llvm/Analysis/DependenceAnalysis.h>

namespace llvm {

class LoopBlockingPass : public llvm::PassInfoMixin<LoopBlockingPass> {
public:
    PreservedAnalyses run(Function& F, FunctionAnalysisManager& AM);
};

struct BlockingInfo
{
    BlockingInfo(unsigned int Factor) : BlockingFactor(Factor) {}
    unsigned int getBlockingFactor() const { return BlockingFactor; }
private:
    unsigned int BlockingFactor;
};

class BlockingNest
{
    using NestIter = SmallVectorTemplateCommon<Loop*>::iterator;
    using RevNestIter = SmallVectorTemplateCommon<Loop*>::reverse_iterator;
public:
    BlockingNest() = delete;
    BlockingNest(SmallVectorImpl<Loop*> &Nest) = delete;
    BlockingNest(SmallVectorImpl<Loop*> &&Nest) : Nest(std::move(Nest)) {}

    Loop *&topLoop()    { return Nest.front(); }
    NestIter begin()    { return Nest.begin(); }
    NestIter end()      { return Nest.end(); }
    unsigned size() const    { return Nest.size(); }
    SmallVector<Loop*> innerLoops() { return SmallVector<Loop*>(Nest.begin() + 1, Nest.end()); }

    bool areAllLoopsSimplified() { return all_of(Nest, [](Loop* L) { return L->isLoopSimplifyForm(); }); }
    bool areAllLoopsRotated() { return all_of(Nest, [](Loop* L) { return L->isRotatedForm(); }); }
private:
    SmallVector<Loop*, 8> Nest;
};

class LoopBlocking {
public:
    LoopBlocking(
        LoopInfo& LI, DominatorTree& DT, ScalarEvolution& SE, 
        DependenceInfo &DI, AAResults &AA, TargetTransformInfo &TTI, Function &F): 
        LI(LI), DT(DT), SE(SE), DI(DI), AA(AA), TTI(TTI), ParentFunc(F) {}
    bool execute();
private:
    LoopInfo& LI;
    DominatorTree& DT;
    ScalarEvolution& SE;
    DependenceInfo &DI;
    AAResults &AA;
    TargetTransformInfo &TTI;
    SmallVector<DominatorTree::UpdateType, 8> DomTreeUpdateVect;

    Function& ParentFunc;
    
    bool dominantBound(DominatorTree& DT, Value* Bound, BasicBlock* BB);
    bool checkBoundaryValuesDominance(Loop::LoopBounds &Bounds, BasicBlock *BB, DominatorTree &DT, ScalarEvolution& SE);
    bool transform(BlockingNest& C);
    SmallVector<BlockingNest> collectCandidates(ArrayRef<Loop*> const& loopsVector);
    Optional<BlockingInfo> blockingAnalysis(BlockingNest &L);
    Loop* createBlockingLoop(Loop *Target, Loop *Outer, Loop::LoopBounds const& TargetBounds, BlockingInfo &Info);
};

}

#endif