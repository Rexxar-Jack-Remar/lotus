# Monotone DataFlow Unit Tests

# Mono Test
add_lotus_mono_test(mono_test MonoTest.cpp)
target_sources(mono_test PRIVATE
  MonoTestPart2.cpp
  MonoTestPart3.cpp)
