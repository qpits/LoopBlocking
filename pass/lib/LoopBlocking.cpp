
#include "LoopBlocking.hpp"
#include <cassert>
#include <llvm/ADT/APInt.h>
#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/STLExtras.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Analysis/DomTreeUpdater.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/LoopNestAnalysis.h>
#include <llvm/Analysis/ScalarEvolution.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constant.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Use.h>
#include <llvm/Passes/PassPlugin.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/ADT/Statistic.h>
#include <llvm/Support/Casting.h>
#include <llvm/Analysis/LoopCacheAnalysis.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Analysis/TargetTransformInfo.h>
#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/Analysis/DependenceAnalysis.h>
#include <llvm/IR/IntrinsicInst.h>

#include <llvm/Support/Debug.h>
#include <llvm/Support/Format.h>
#include <memory>

using namespace llvm;
#define DEBUG_TYPE "loop-blocking"

static cl::opt<unsigned> BlockingFactor(
    "blk-f", cl::init(16), cl::Hidden, 
    cl::desc("Specify the L1 data cache size in bytes"));

static cl::opt<unsigned> MaxPerfectNestDepth(
    "max-nest-depth", cl::init(3), cl::Hidden,
    cl::desc("Specify the maximum depth of the perfect nests to consider"));

static cl::opt<unsigned> FirstLoopDepth(
    "first-depth", cl::init(0), cl::Hidden,
    cl::desc("Specify the depth of the first loop to block, sarting from the root at level 0."));

STATISTIC(CandidateLoops, "Candidate loops");
STATISTIC(TransformedLoops, "Loops transformed");
STATISTIC(InvalidLoops, "Invalid loops");
STATISTIC(FoundRotated, "How many times a loop in rotated form was encountered");
STATISTIC(BoundsNotDominant, "Candidate loop bounds did not dominate its Parent's header");



bool LoopBlocking::execute()
{
    /*analyze loop forest to find possible candidates to block*/
    std::vector<Loop*> const &LoopsVector = LI.getTopLevelLoops();
    /*collect all candidate loops*/
    LLVM_DEBUG(dbgs() << "Collecting loops...\n");
    SmallVector<BlockingNest> Nests = collectCandidates(LoopsVector);
    bool Changed = false;
    for (BlockingNest& N : Nests) {
        if ((Changed |= transform(N))) {
            LLVM_DEBUG(dbgs() << "Candidate successfully transofrmed.\n");
            TransformedLoops++;
        }
    }

    return Changed;
}

bool LoopBlocking::transform(BlockingNest& BN)
{
    // Before starting with the transformation, we have to check that it is actually LEGAL to transform a candidate loop.
    // The transformation is legal if:
    //      - Parent exit block contains just the terminator instruction;
    //      - Parent header and Candidate are adjacent: only the candidate preheader with branch instruction between them;
    //      - Candidate preheader contains just the terminator instruction;
    //      - Candidate exit block contains just the terminator instruction;
    //      - Candidate exit block and Parent latch are adjacent;
    //      - the Value used to check the Candidate bounds dominates the Parent header block.

    if (FirstLoopDepth < 0 || FirstLoopDepth >= BN.size()) {
        LLVM_DEBUG(dbgs() << "First loop depth out of range. Aborting.\n");
        return false;
    }

    if (!BN.areAllLoopsSimplified()) {
        LLVM_DEBUG(dbgs() << "Not all loops are in simplified form!\n");
        return false;
    }

    if (!BN.areAllLoopsRotated()) {
        LLVM_DEBUG(dbgs() << "Not all loops are in rotated form!\n");
        return false;
    }

    SmallVector<Loop*> Inner(BN.begin()+FirstLoopDepth, BN.end());
    DenseMap<Loop*, std::unique_ptr<Loop::LoopBounds>> BoundsMap;
    // Finally check dominance for bounds
    for (Loop *L : Inner) {
        Optional<Loop::LoopBounds> Bounds = L->getBounds(SE);
        if (!Bounds) {
            LLVM_DEBUG(dbgs() << "Loop: " << L->getName() << "bounds info could not be computed!\n");
            return false;
        }
        if (!checkBoundaryValuesDominance(*Bounds, L->getHeader(), DT, SE)) {
            LLVM_DEBUG(dbgs() << "Loop: " << L->getName() << "bounds do not dominate parent header!\n");
            BoundsNotDominant++;
            return false;
        }
        if ((*Bounds).getDirection() == Loop::LoopBounds::Direction::Unknown) {
            LLVM_DEBUG(dbgs() << "Loop: " <<  L->getName() << ": direction unknown\n");
            return false;
        }
        BoundsMap[L] = std::make_unique<Loop::LoopBounds>(std::move(*Bounds));
    }
    
    blockingAnalysis(BN);

    Loop *TopLoop = BN.topLoop();


    // All the legality checks are complete, now we can create the new blocking loop.
    
    for (auto it = Inner.rbegin(); it != Inner.rend(); it++) {
        LLVM_DEBUG(dbgs() << "Considering loop: \n"; (*it)->print(dbgs().indent(4), false, false););
        auto info = BlockingInfo(BlockingFactor);
        
        Loop* BlockingLoop = createBlockingLoop(*it, TopLoop, *(BoundsMap[*it]), info);
        // Update the analysis
        if (!TopLoop->getParentLoop())
            LI.addTopLevelLoop(BlockingLoop);

        for (auto it = LI.begin(); it != LI.end(); it++) {
            if (*it == TopLoop) {
                LI.removeLoop(it);
                break;
            }
        }
        for (auto* BB : TopLoop->blocks())
            BlockingLoop->addBlockEntry(BB);
        
        BlockingLoop->addChildLoop(TopLoop);
        SE.forgetLoop(*it);
        TopLoop = BlockingLoop;
        #ifndef NDEBUG
        BlockingLoop->verifyLoop();
        #endif
    }
    
    
    #ifndef NDEBUG
    LI.verify(DT);
    assert(DT.verify() && "DomTree is broken!");
    #endif

    return true;
}

SmallVector<BlockingNest> LoopBlocking::collectCandidates(ArrayRef<Loop*> const& loopsVector)
{
    // Collect all loops that may be candidate for blocking
    LLVM_DEBUG(dbgs() << "Checking candidates...\n");
    SmallVector<BlockingNest> nests;
    for (Loop* L : loopsVector) {
        std::unique_ptr<LoopNest> Nest = LoopNest::getLoopNest(*L, SE);
        // take all the perfect loop nest of depth 3 max
        auto PerfetcNests = Nest->getPerfectLoops(SE);
        for (LoopVectorTy &N : PerfetcNests) {
            unsigned PerfectNestDepth = N.size();
            if (PerfectNestDepth >= 2U && PerfectNestDepth <= MaxPerfectNestDepth)
                nests.push_back(BlockingNest(std::move(N)));
        }
    }
    LLVM_DEBUG(dbgs() << "Collected " << nests.size() << " candidates\n");
    return nests;
}

Loop* LoopBlocking::createBlockingLoop(Loop *Target, Loop *Outer, Loop::LoopBounds const& TargetBounds, BlockingInfo& Info)
{
    assert(Target && "Target loop not valid!");
    assert(Outer && "Outer loop not valid!");
    assert(Target->isRotatedForm() && "Target loop is not in rotated form!");
    assert(Target->isLoopSimplifyForm() && "Target loop is not in simplified form!");
    BasicBlock *OuterHeader = Outer->getHeader();
    BasicBlock *OuterPreheader = Outer->getLoopPreheader();
    BasicBlock *OuterLatch = Outer->getLoopLatch();
    BasicBlock *OuterExit = Outer->getExitBlock();
    assert(OuterHeader && "Invalid header for outer loop.");
    assert(OuterPreheader && "Invalid preheader for outer loop.");
    assert(OuterLatch && "Invalid latch for outer loop.");
    assert(OuterPreheader && "Invalid preheader for outer loop.");

    BasicBlock *TargetLatch = Target->getLoopLatch();
    BasicBlock *TargetPreheader = Target->getLoopPreheader();
    assert(TargetLatch && "Invalid latch for target loop.");
    assert(TargetPreheader && "Invalid preheader for target loop.");

    BasicBlock *OuterExiting = Outer->getExitingBlock();
    assert(OuterExiting && "Outer loop has more than one exiting block.");

    PHINode *TargetIv = Target->getInductionVariable(SE);
    assert(TargetIv && "Target induction variable not available");
    // Tell LoopInfo to allocate a new loop: this loop will provide the blocking to the candidate
    Loop* NL = LI.AllocateLoop();

    // This new loop needs a header and a latch
    // Outer loop will need a new preheader too
    BasicBlock* NewHeader = BasicBlock::Create(ParentFunc.getContext(), "blocking.loop.header", &ParentFunc, OuterHeader);
    BasicBlock* NewLatch = BasicBlock::Create(ParentFunc.getContext(), "blocking.loop.latch", &ParentFunc, OuterExit);
    BasicBlock *NewOuterPreheader = BasicBlock::Create(ParentFunc.getContext(), "ph", &ParentFunc, NewHeader);
    NL->addBlockEntry(NewHeader);
    LLVM_DEBUG(dbgs() << "Added header to new loop.\n");
    NL->addBlockEntry(NewLatch);
    LLVM_DEBUG(dbgs() << "Added latch to new loop.\n");
    NL->addBlockEntry(NewOuterPreheader);
    LLVM_DEBUG(dbgs() << "Added new preheader to outer loop.\n");

    
    // the old Parent preheader will now fallthrough to the new loop header, practically becoming its preheader
    BranchInst* OldParentPreheaderTerminator = cast<BranchInst>(OuterPreheader->getTerminator());
    assert(OldParentPreheaderTerminator->isUnconditional() && "Old parent preheader terminator is not an unconditional branch!");
    OldParentPreheaderTerminator->replaceSuccessorWith(OuterHeader, NewHeader);
    DT.addNewBlock(NewHeader, OuterPreheader);
    LLVM_DEBUG(dbgs() << "adding new block to domtree: Block: ";
                    NewHeader->printAsOperand(dbgs()); dbgs() << " IDom: ";
                    OuterPreheader->printAsOperand(dbgs()); dbgs() << '\n';);

    // new preheader for outer loop is empty and jumps directly to outer header
    BranchInst::Create(OuterHeader, NewOuterPreheader);

    // Phis in parent header have to be updated too!
    OuterHeader->replacePhiUsesWith(OuterPreheader, NewOuterPreheader);

    // We will insert this new latch just before the exit block of the parent loop.
    // Thus such exit block will be the new loop's one and the new latch will become the updated parent exit block
    //
    // In order to preserve the simplified form, a dedicated exit for the target loop needs to be created:
    // in the case where the target block is the root of a nest, its exiting block will jump into a latch of a created loop,
    // which is non in rotated form, so its latch will have a header as predecessor
    OuterExiting->getTerminator()->replaceSuccessorWith(OuterExit, NewLatch);

    DT.addNewBlock(NewLatch, OuterExiting);
    LLVM_DEBUG(dbgs() << "adding new block to domtree: Block: ";
                    NewLatch->printAsOperand(dbgs()); dbgs() << " IDom: ";
                    OuterExiting->printAsOperand(dbgs()); dbgs() << '\n';);

    
    // New latch branches to the new header
    BranchInst* NewLatchTerminator = BranchInst::Create(NewHeader, NewLatch);
    // Latch is only missing the update instruction on the IV
    // we create the iv and momentarily put it at the end of the new loop header
    PHINode* NewIV = PHINode::Create(TargetIv->getType(), 2, "blocking.loop.IV", NewHeader);
    // update lower bound of target induction variable
    TargetIv->setIncomingValueForBlock(TargetPreheader, NewIV);
    // create update and insert it before the new latch terminator
    // blocking factor should have the same type as the new IV
    Constant* BlockingFactor = ConstantInt::getIntegerValue(NewIV->getType(), 
                                                                APInt(NewIV->getType()->getIntegerBitWidth(), Info.getBlockingFactor()));
    BinaryOperator* UpdateIVInst = BinaryOperator::CreateAdd(NewIV, BlockingFactor, "blocking.loop.update.IV", NewLatchTerminator);

    // NOTE: C.ParentPreheader has become the blocking loop preheader!
    NewIV->addIncoming(&TargetBounds.getInitialIVValue(), OuterPreheader);
    NewIV->addIncoming(UpdateIVInst, NewLatch);

    // Now we need the compare instruction for the new loop exit condition.
    // the comparison will be: NewIV < C.UB
    CmpInst* NewHeaderExitCond = CmpInst::Create(Instruction::OtherOps::ICmp, 
                                        TargetBounds.getCanonicalPredicate(), 
                                        NewIV, &TargetBounds.getFinalIVValue(), 
                                        "new.header.exit.cond",
                                        NewHeader);
    // this comparison will determine the condition of the branch instruction
    // blocking loop header terminator points to the parent loop header if true or to the parent loop exit if false
    BranchInst::Create(NewOuterPreheader, OuterExit, NewHeaderExitCond, NewHeader);
    DT.addNewBlock(NewOuterPreheader, NewHeader);
    DT.insertEdge(NewOuterPreheader, OuterHeader);
    DT.insertEdge(NewHeader, OuterExit);
    DT.deleteEdge(OuterExiting, OuterExit);
    DT.deleteEdge(OuterPreheader, OuterHeader);

    // Starting in the candidate preheader, a new value has to be create to provide an additional boundary check in the candiate header.
    // This boundary is that of the "end" of the iteration block in which the loop is currently iterating inside.
    // get the update instruction for TargetIV
    Instruction::BinaryOps BoundOp;
    switch (TargetBounds.getDirection()) {
        case Loop::LoopBounds::Direction::Increasing:
            BoundOp = Instruction::BinaryOps::Add;
            break;
        case Loop::LoopBounds::Direction::Decreasing:
            BoundOp = Instruction::BinaryOps::Sub;
            break;
        case Loop::LoopBounds::Direction::Unknown:
        default:
            llvm_unreachable("Cannot handle unknown loop direction!");
    }
    BinaryOperator *BoundValue = BinaryOperator::Create(BoundOp,
                                                        NewIV, BlockingFactor, 
                                                        "blocking.bound.value", 
                                                        TargetPreheader->getTerminator());
    // select the right intrinsic function ID to call based on sign of types
    Intrinsic::ID IntrFuncMinID;
    if (Target->getLatchCmpInst()->isSigned())
        IntrFuncMinID = Intrinsic::smin;
    else
        IntrFuncMinID = Intrinsic::umin;
    // use min intrinsic to calculate the minimum between the block bound and the loop bound
    SmallVector<Type*, 2> Types = {BoundValue->getType()};
    Function *MinFuncIntrinsic = Intrinsic::getDeclaration(ParentFunc.getParent(), IntrFuncMinID, Types);
    SmallVector<Value*, 2> Args;
    Args.push_back(BoundValue);
    Args.push_back(&TargetBounds.getFinalIVValue());
    CallInst *MinIntrCall = CallInst::Create(MinFuncIntrinsic, Args, "min.val", TargetLatch->getTerminator());
    
    // create the new compare instruction: this will be added in the latch.
    // need to erase the old one...
    // FIXME: check if getCanonicalPredicate will return the right predicate for this case
    // NOTE: OldLatchCompInst->getOperand(0) represents the update of TargetIV that happens before the bound check
    auto OldLatchCompInst = Target->getLatchCmpInst();
    CmpInst* BlockBoundCond = CmpInst::Create(Target->getLatchCmpInst()->getOpcode(),
                                         TargetBounds.getCanonicalPredicate(), 
                                         OldLatchCompInst->getOperand(0), MinIntrCall, 
                                         "blocking.bound.check", 
                                         TargetLatch->getTerminator());
    // set it as the condition in the terminator instr
    cast<BranchInst>(TargetLatch->getTerminator())->setCondition(BlockBoundCond);
    OldLatchCompInst->eraseFromParent();
    return NL;
}

bool LoopBlocking::checkBoundaryValuesDominance(Loop::LoopBounds &Bounds, BasicBlock *BB, DominatorTree &DT, ScalarEvolution& SE)
{
    LLVM_DEBUG(dbgs() << "Checking upper bound:"; Bounds.getFinalIVValue().printAsOperand(dbgs()); dbgs() << '\n');
    if (!dominantBound(DT, &Bounds.getFinalIVValue(), BB)) {
        return false;
    }
    LLVM_DEBUG(dbgs() << "Checking lower bound:"; Bounds.getInitialIVValue().printAsOperand(dbgs()); dbgs() << '\n');
    if (!dominantBound(DT, &Bounds.getInitialIVValue(), BB)) {
        return false;
    }
    return true;
}

PreservedAnalyses LoopBlockingPass::run(Function &F, FunctionAnalysisManager &AM)
{
    //Required analysis for this pass
    LoopInfo& LI = AM.getResult<LoopAnalysis>(F);
    DominatorTree& DT = AM.getResult<DominatorTreeAnalysis>(F);
    ScalarEvolution& SE = AM.getResult<ScalarEvolutionAnalysis>(F);
    TargetTransformInfo &TTI = AM.getResult<TargetIRAnalysis>(F);
    AAResults &AA = AM.getResult<AAManager>(F);
    DependenceInfo &DI = AM.getResult<DependenceAnalysis>(F);
    
    LoopBlocking LB(LI, DT, SE, DI, AA, TTI, F);
    LLVM_DEBUG(dbgs() << "Starting Loop Blocking pass execution.\n");

    bool Changed = LB.execute();

    if (!Changed) {
        LLVM_DEBUG(dbgs() << "No change made by the pass.\n");
        return PreservedAnalyses::all();
    }
    PreservedAnalyses Pres;
    Pres.preserve(LoopAnalysis::ID());
    Pres.preserve(DominatorTreeAnalysis::ID());
    Pres.preserve(ScalarEvolutionAnalysis::ID());
    return Pres;
}

bool LoopBlocking::dominantBound(DominatorTree& DT, Value* Bound, BasicBlock* BB)
{
    if (isa<ConstantInt>(Bound) || isa<ConstantFP>(Bound)) {
        LLVM_DEBUG(dbgs() << "Constant bound found in loop\n");
        return true;
    }
    else if (isa<Instruction>(Bound)) {
        if (DT.dominates(cast<Instruction>(Bound), BB)) {
            LLVM_DEBUG(dbgs() << "Found bound as instruction that dominates parent header in loop\n");
            return true;
        }
        else {
            LLVM_DEBUG(dbgs() << "Found bound as instruction that DOES NOT dominate parent header in loop\n");
            return false;
        }
    }
    else if (isa<Argument>(Bound)) {
        LLVM_DEBUG(dbgs() << "Found argument as bound: always dominates.\n");
        return true;
    }
    LLVM_DEBUG(dbgs() << "Value type of bound not recognized. Cannot infer info on dominance.\n");
    return false;
}

Optional<BlockingInfo> LoopBlocking::blockingAnalysis(BlockingNest &BN)
{
    for (auto it = BN.begin(); it != BN.end(); it++) {
        unsigned TripC = SE.getSmallConstantTripCount(*it);
        LLVM_DEBUG(dbgs().indent(2) << "Trip count:" << TripC << '\n');
    }
    std::unique_ptr<CacheCost> CacheC = std::make_unique<CacheCost>(SmallVector<Loop*, 8>(BN.begin(), BN.end()), LI, SE, TTI, AA, DI);
    auto LoopCosts = CacheC->getLoopCosts();
    Optional<unsigned> size = TTI.getCacheSize(TargetTransformInfo::CacheLevel::L1D);
    Optional<unsigned> asc = TTI.getCacheAssociativity(TargetTransformInfo::CacheLevel::L1D);
    LLVM_DEBUG(dbgs() << *CacheC);
    if (size)
        LLVM_DEBUG(dbgs().indent(4) << *size << '\n');
    if (asc)
        LLVM_DEBUG(dbgs().indent(4) << *asc << '\n');

    return Optional<BlockingInfo>();
}

extern "C" PassPluginLibraryInfo llvmGetPassPluginInfo() {
    return {
        .APIVersion = LLVM_PLUGIN_API_VERSION,
        .PluginName = "CustomLoopBlocking",
        .PluginVersion = LLVM_VERSION_STRING,
        .RegisterPassBuilderCallbacks = 
            [](PassBuilder& PB) {
                PB.registerPipelineParsingCallback(
                    [](StringRef  name, FunctionPassManager& passManager, ArrayRef<PassBuilder::PipelineElement>) -> bool {
                        if (name == "custom-loopblocking")
                        {
                            passManager.addPass(LoopBlockingPass());
                            return true;
                        }

                        return false;
                    }
                );
            }
    };
}