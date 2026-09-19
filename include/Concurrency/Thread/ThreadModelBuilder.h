#pragma once
#include "Concurrency/Thread/ThreadModel.h"
#include <llvm/IR/Module.h>

namespace mhp {
class JoinTargetAnalysis;
} // namespace mhp

namespace concurrency {
class ThreadMultiplicityAnalysis;
} // namespace concurrency

namespace lotus::concurrency {
    class ThreadModelBuilder {
    public:
        ThreadModelBuilder& setJoinTargetAnalysis(mhp::JoinTargetAnalysis* jta) {
            m_join_target_analysis = jta;
            return *this;
        }

        ThreadModelBuilder&
        setMultiplicityAnalysis(::concurrency::ThreadMultiplicityAnalysis* tma) {
            m_multiplicity_analysis = tma;
            return *this;
        }

        ThreadModel build(llvm::Module& M);

    private:
        mhp::JoinTargetAnalysis* m_join_target_analysis = nullptr;
        ::concurrency::ThreadMultiplicityAnalysis* m_multiplicity_analysis =
            nullptr;
    };
} // namespace lotus::concurrency