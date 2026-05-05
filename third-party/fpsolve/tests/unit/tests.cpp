#include <gtest/gtest.h>

#ifdef USE_GENEPI
#include "test-semilinSetNdd.h"
#endif

int main(int argc, char* argv[])
{
  ::testing::InitGoogleTest(&argc, argv);

#ifdef USE_GENEPI
  SemilinSetNdd::genepi_init();
#endif

  const int result = RUN_ALL_TESTS();

#ifdef USE_GENEPI
  SemilinSetNdd::genepi_dealloc();
#endif

  return result;
}
