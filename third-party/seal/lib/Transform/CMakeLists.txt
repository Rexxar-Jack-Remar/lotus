add_library(PPYTransform STATIC
        InlineFunction.cpp
        LowerConstantExpr.cpp
        LowerGlobalConstantArraySelect.cpp
        LowerSelect.cpp
        MergeReturn.cpp
        NameBlock.cpp
        RemoveDeadBlock.cpp
        RemoveIrreducibleFunction.cpp
        RemoveNoRetFunction.cpp
        SeparateMergingBlock.cpp
        SimplifyLatch.cpp
        )