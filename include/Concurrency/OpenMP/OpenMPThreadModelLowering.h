#pragma once

namespace lotus::concurrency::OpenMP {
    class OpenMPSemantics;
} // namespace lotus::concurrency::OpenMP
namespace mhp {
    class MHPAnalysis;
} // namespace mhp

namespace lotus::concurrency::OpenMP {
    class OpenMPThreadModelLowering {
    public:
        // Scaffolding: the actual lowering logic will be migrated here once
        // ThreadModelBuilder exposes the necessary graph construction primitives.
        static void lowerTasks(mhp::MHPAnalysis& mhp, const OpenMPSemantics& semantics) {
            // currently handled inside MHPAnalysis::lowerOpenMPTasks
        }
    };
} // namespace lotus::concurrency::OpenMP