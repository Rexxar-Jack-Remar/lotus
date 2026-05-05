# Analysis Profile Unit Tests

add_lotus_targeted_test(hot_profile_test
               HotProfileTest.cpp
               LINK_LIBS
                   CanaryProfile
                   LLVMAnalysis)
