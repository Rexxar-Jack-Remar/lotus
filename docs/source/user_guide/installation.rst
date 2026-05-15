Installation Guide
===================

Installation guide for Lotus and its dependencies.

Prerequisites
-------------

* LLVM 14.0.0
* Z3 4.11
* CMake 3.10+
* C++17 compatible compiler
* Boost 1.65+ (auto-downloaded if not found)

Building Lotus
--------------

.. code-block:: bash

   git clone https://github.com/ZJU-PL/lotus
   cd lotus
   mkdir build && cd build
   cmake ..
   make -j$(nproc)

Configuration Options
---------------------

**Optional:**
* ``-DLLVM_BUILD_PATH``: Path to the directory containing ``LLVMConfig.cmake`` (for example,
  ``/usr/lib/llvm-14/lib/cmake/llvm`` or a local LLVM build/install). Only needed if CMake
  cannot find an appropriate system LLVM automatically.
* ``-DLOTUS_CUSTOM_BOOST_ROOT=/path/to/boost``: Path to a custom Boost installation
* ``-DLOTUS_CUSTOM_CRAB_ROOT=/path/to/crab``: Path to a custom CRAB checkout
* ``-DLOTUS_BUILD_TESTS=OFF``: Disable building tests
* ``-DLOTUS_BUILD_EXAMPLES=ON``: Build examples
* ``-DLOTUS_ENABLE_CLAM=OFF``: Disable CLAM tools
* ``-DLOTUS_ENABLE_SEAHORN=OFF``: Disable SeaHorn tools
* ``-DLOTUS_ENABLE_SMACK=ON``: Enable SMACK tools
* ``-DLOTUS_ENABLE_HORN_ICE=ON``: Build ICE learning tools for CHC
* ``-DLOTUS_ENABLE_DYNAA=ON``: Build dynamic alias-analysis tools
* ``-DLOTUS_ENABLE_CFL=ON``: Build CFL tool family
* ``-DLOTUS_ENABLE_CSR=ON``: Build the CSR CFL solver
* ``-DLOTUS_ENABLE_OWL=ON``: Build the Owl SMT solver
* ``-DLOTUS_SEADSA_ENABLE_SANITY_CHECKS=ON``: Enable Sea-DSA sanity checks
* ``-DLOTUS_DOWNLOAD_BOOST=OFF``: Disable Boost auto-download
* ``-DLOTUS_DOWNLOAD_CRAB=ON``: Allow CRAB auto-download when needed

Typical configurations:

.. code-block:: bash

   # Lean local build
   cmake -S . -B build -DLOTUS_BUILD_TESTS=OFF

   # Enable optional analysis families
   cmake -S . -B build \
     -DLOTUS_ENABLE_DYNAA=ON \
     -DLOTUS_ENABLE_HORN_ICE=ON \
     -DLOTUS_ENABLE_CFL=ON \
     -DLOTUS_ENABLE_CSR=ON

   # Disable heavyweight verifier integrations
   cmake -S . -B build \
     -DLOTUS_ENABLE_CLAM=OFF \
     -DLOTUS_ENABLE_SEAHORN=OFF \
     -DLOTUS_ENABLE_SMACK=OFF

Z3 Installation
---------------

.. code-block:: bash

   # Ubuntu/Debian
   sudo apt-get install libz3-dev

   # macOS with Homebrew
   brew install z3

   # Or build from source
   git clone https://github.com/Z3Prover/z3.git
   cd z3 && python scripts/mk_make.py
   cd build && make && sudo make install


Troubleshooting
---------------

* **LLVM not found**: Install LLVM 14.x via your package manager (or from source).
  If you use a non-standard installation location, set ``LLVM_BUILD_PATH`` to the directory
  that contains ``LLVMConfig.cmake`` and re-run CMake.
* **Z3 not found**: Install Z3 or set ``Z3_DIR``
* **Boost issues**: Use ``LOTUS_CUSTOM_BOOST_ROOT`` or let Lotus auto-download Boost
* **Build errors**: Use supported LLVM version (14.x)
