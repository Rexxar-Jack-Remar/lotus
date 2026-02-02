/**
 * @file IRPreProcessor.cpp
 * @brief IR preprocessing pass manager for AserPTA.
 *
 * Runs a sequence of LLVM passes to prepare the IR for pointer analysis.
 * Sets up target information and executes function-level and module-level
 * preprocessing passes configured by PreProcPassManagerBuilder.
 *
 * @author peiming
 */
#include <llvm/ADT/Triple.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/Analysis/TargetTransformInfo.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>

#include "Alias/AserPTA/PreProcessing/IRPreProcessor.h"
#include "Alias/AserPTA/PreProcessing/PreProcPassManagerBuilder.h"

using namespace llvm;
using namespace aser;

/**
 * @brief Get a TargetMachine for the given triple.
 *
 * Looks up the target in the LLVM target registry and creates a TargetMachine
 * instance. Returns nullptr if the target is not found or not specified.
 *
 * @param TheTriple The target triple
 * @param CPUStr CPU string (empty for default)
 * @param FeaturesStr Features string (empty for default)
 * @param Options Target options
 * @return TargetMachine pointer or nullptr if not found
 */
static TargetMachine* GetTargetMachine(Triple TheTriple, StringRef CPUStr,
                                       StringRef FeaturesStr,
                                       const TargetOptions &Options) {
    std::string Error;
    const Target *TheTarget = TargetRegistry::lookupTarget("", TheTriple, Error);
    // Some modules don't specify a triple, and this is okay.
    if (!TheTarget) {
        return nullptr;
    }

    return TheTarget->createTargetMachine(TheTriple.getTriple(), CPUStr, FeaturesStr, Options, None);
}


/**
 * @brief Run preprocessing passes on a module.
 *
 * Sets up target information, creates pass managers, and runs function-level
 * and module-level preprocessing passes to prepare the IR for pointer analysis.
 *
 * @param M The module to preprocess
 */
void IRPreProcessor::runOnModule(llvm::Module &M) {
    // TODO: Do we really need to know the target machine information?
    Triple ModuleTriple(M.getTargetTriple());
    std::string CPUStr = "", FeaturesStr = "";
    TargetMachine *Machine = nullptr;
    const TargetOptions Options;

    if (ModuleTriple.getArch()) {
        Machine = GetTargetMachine(ModuleTriple, CPUStr, FeaturesStr, Options);
    } else if (ModuleTriple.getArchName() != "unknown" &&
               ModuleTriple.getArchName() != "") {
        // err: do not know target machine type
        return;
    }
    std::unique_ptr<TargetMachine> TM(Machine);

    // Override function attributes based on CPUStr, FeaturesStr, and command line
    // flags.
    // setFunctionAttributes(CPUStr, FeaturesStr, M);

    // Create a PassManager to hold and optimize the collection of passes we are
    // about to build.
    legacy::PassManager Passes;
    legacy::FunctionPassManager FPasses(&M);
    PreProcPassManagerBuilder builder;

    // Add an appropriate TargetLibraryInfo pass for the module's triple.
    // target-info and target transfromInfo
    TargetLibraryInfoImpl TLII(ModuleTriple);
    Passes.add(new TargetLibraryInfoWrapperPass(TLII));
    Passes.add(createTargetTransformInfoWrapperPass(TM ? TM->getTargetIRAnalysis() : TargetIRAnalysis()));

    FPasses.add(new TargetLibraryInfoWrapperPass(TLII));
    FPasses.add(createTargetTransformInfoWrapperPass(TM ? TM->getTargetIRAnalysis() : TargetIRAnalysis()));
    builder.populateFunctionPassManager(FPasses);
    builder.populateModulePassManager(Passes);

    FPasses.doInitialization();
    for (Function &F : M) {
        FPasses.run(F);
    }
    FPasses.doFinalization();

    Passes.run(M);
}