#!/bin/bash
set -e
cd ../../
pwd
mkdir build
cd build

# conda create -n py311 python=3.11
# conda activate py311
# conda install libboost=1.73.0

cmake ../ \
   -DLLVM_BUILD_PATH=/usr/local/llvm-14.0.6 \
   -DZ3_DIR=/usr/local/z3-4.11.2 \
   -DLOTUS_CUSTOM_BOOST_ROOT=$CONDA_PREFIX \
   -DLOTUS_ENABLE_CLAM=OFF \
   -DLOTUS_ENABLE_SEAHORN=OFF \
   -DLOTUS_BUILD_TESTS=OFF

make -j$(nproc)
