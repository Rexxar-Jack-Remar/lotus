cmake_minimum_required(VERSION 3.22)

# 项目名称
project(SlotProject)

# 设置C++标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# 定义依赖路径
set(LLVM_PATH "/home/ljc/NEW/llvm-project")
set(Z3_PATH "/home/ljc/NEW/z3")

set(LLVM_INCLUDE_PATH ${LLVM_PATH}/llvm/include)
set(LLVM_BUILD_INCLUDE_PATH ${LLVM_PATH}/build/include)
set(LLVM_LIB_PATH ${LLVM_PATH}/build/lib)
set(Z3_API_PATH ${Z3_PATH}/src/api)
set(Z3_CXX_API_PATH ${Z3_PATH}/src/api/c++)


# 包含头文件的路径
include_directories(${PROJECT_SOURCE_DIR}/include 
                    ${Z3_CXX_API_PATH}
                    ${Z3_API_PATH}
                    ${LLVM_BUILD_INCLUDE_PATH}
                    ${LLVM_INCLUDE_PATH})

# 源文件目录
file(GLOB SOURCES ${PROJECT_SOURCE_DIR}/src/*.cpp)

# 定义编译选项
set(COMPILER_OPTIONS
    -fno-rtti
    -D_GNU_SOURCE
    -D_DEBUG
    -D_GLIBCXX_ASSERTIONS
    -D_LIBCPP_ENABLE_ASSERTIONS
    -D__STDC_CONSTANT_MACROS
    -D__STDC_FORMAT_MACROS
    -D__STDC_LIMIT_MACROS
    -g
    -O3
    -fexceptions
)

# 定义链接选项
set(LINK_OPTIONS
    /usr/lib/libz3.so z3
    ${LLVM_LIB_PATH}/libLLVMPasses.a
    ${LLVM_LIB_PATH}/libLLVMIRPrinter.a
    ${LLVM_LIB_PATH}/libLLVMCoroutines.a
    ${LLVM_LIB_PATH}/libLLVMipo.a
    ${LLVM_LIB_PATH}/libLLVMInstrumentation.a
    ${LLVM_LIB_PATH}/libLLVMVectorize.a
    ${LLVM_LIB_PATH}/libLLVMLinker.a
    ${LLVM_LIB_PATH}/libLLVMFrontendOpenMP.a
    ${LLVM_LIB_PATH}/libLLVMCodeGen.a
    ${LLVM_LIB_PATH}/libLLVMTarget.a
    ${LLVM_LIB_PATH}/libLLVMScalarOpts.a
    ${LLVM_LIB_PATH}/libLLVMInstCombine.a
    ${LLVM_LIB_PATH}/libLLVMObjCARCOpts.a
    ${LLVM_LIB_PATH}/libLLVMBitWriter.a
    ${LLVM_LIB_PATH}/libLLVMAggressiveInstCombine.a
    ${LLVM_LIB_PATH}/libLLVMTransformUtils.a
    ${LLVM_LIB_PATH}/libLLVMAnalysis.a
    ${LLVM_LIB_PATH}/libLLVMProfileData.a
    ${LLVM_LIB_PATH}/libLLVMSymbolize.a
    ${LLVM_LIB_PATH}/libLLVMDebugInfoPDB.a
    ${LLVM_LIB_PATH}/libLLVMDebugInfoMSF.a
    ${LLVM_LIB_PATH}/libLLVMDebugInfoDWARF.a
    ${LLVM_LIB_PATH}/libLLVMObject.a
    ${LLVM_LIB_PATH}/libLLVMTextAPI.a
    ${LLVM_LIB_PATH}/libLLVMMCParser.a
    ${LLVM_LIB_PATH}/libLLVMIRReader.a
    ${LLVM_LIB_PATH}/libLLVMAsmParser.a
    ${LLVM_LIB_PATH}/libLLVMMC.a
    ${LLVM_LIB_PATH}/libLLVMDebugInfoCodeView.a
    ${LLVM_LIB_PATH}/libLLVMBitReader.a
    ${LLVM_LIB_PATH}/libLLVMCore.a
    ${LLVM_LIB_PATH}/libLLVMRemarks.a
    ${LLVM_LIB_PATH}/libLLVMBitstreamReader.a
    ${LLVM_LIB_PATH}/libLLVMBinaryFormat.a
    ${LLVM_LIB_PATH}/libLLVMTargetParser.a
    ${LLVM_LIB_PATH}/libLLVMSupport.a
    ${LLVM_LIB_PATH}/libLLVMDemangle.a
    rt dl pthread m z zstd tinfo xml2
)


# 生成可执行文件
add_executable(slot ${SOURCES}
        src/tools/main.cpp)
# 添加编译选项
target_compile_options(slot PRIVATE ${COMPILER_OPTIONS})
# 链接库
target_link_libraries(slot PRIVATE ${LINK_OPTIONS})


add_executable(smt2llvm ${SOURCES}
        src/tools/SMT2LLVM.cpp)
target_compile_options(smt2llvm PRIVATE ${COMPILER_OPTIONS})
target_link_libraries(smt2llvm PRIVATE ${LINK_OPTIONS})

add_executable(llvm2smt ${SOURCES}
        src/tools/LLVM2SMT.cpp)
target_compile_options(llvm2smt PRIVATE ${COMPILER_OPTIONS})
target_link_libraries(llvm2smt PRIVATE ${LINK_OPTIONS})

add_executable(llvm2feat ${SOURCES}
        src/tools/LLVM2FEAT.cpp)
target_compile_options(llvm2feat PRIVATE ${COMPILER_OPTIONS})
target_link_libraries(llvm2feat PRIVATE ${LINK_OPTIONS})

add_executable(fastslot ${SOURCES}
        src/tools/fastslot.cpp)
target_compile_options(fastslot PRIVATE ${COMPILER_OPTIONS})
target_link_libraries(fastslot PRIVATE ${LINK_OPTIONS})