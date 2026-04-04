# CLAM and CRAB configuration

if(ENABLE_CLAM)
    set(LOTUS_VENDORED_CRAB_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third-party/crab")

    # Determine CRAB root directory
    if(CUSTOM_CRAB_ROOT)
        set(CRAB_ROOT ${CUSTOM_CRAB_ROOT})
        message(STATUS "Using custom CRAB at: ${CRAB_ROOT}")
    elseif(EXISTS "${LOTUS_VENDORED_CRAB_ROOT}/CMakeLists.txt")
        set(CRAB_ROOT "${LOTUS_VENDORED_CRAB_ROOT}")
        message(STATUS "Using vendored CRAB from third-party: ${CRAB_ROOT}")
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/crab/CMakeLists.txt")
        # Backward-compatible fallback for older checkouts.
        set(CRAB_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/crab")
        message(WARNING
            "Using legacy CRAB source tree at ${CRAB_ROOT}. "
            "Please move it to third-party/crab.")
    elseif(DOWNLOAD_CRAB)
        # Download CRAB to build/deps at configure time
        set(CRAB_ROOT "${CMAKE_BINARY_DIR}/deps/crab")
        if(NOT EXISTS "${CRAB_ROOT}/CMakeLists.txt")
            find_package(Git REQUIRED)
            message(STATUS "CRAB not found, downloading to ${CRAB_ROOT}...")
            
            # Clone CRAB at configure time
            execute_process(
                COMMAND ${GIT_EXECUTABLE} clone https://github.com/seahorn/crab.git ${CRAB_ROOT}
                RESULT_VARIABLE GIT_RESULT
                OUTPUT_VARIABLE GIT_OUTPUT
                ERROR_VARIABLE GIT_ERROR
            )
            
            if(NOT GIT_RESULT EQUAL 0)
                message(FATAL_ERROR "Failed to clone CRAB: ${GIT_ERROR}")
            endif()
            
            if(NOT EXISTS "${CRAB_ROOT}/CMakeLists.txt")
                message(FATAL_ERROR "CRAB clone succeeded but CMakeLists.txt not found")
            endif()
            
            # Patch CRAB's CMakeLists.txt to update cmake_minimum_required for newer CMake versions
            file(READ "${CRAB_ROOT}/CMakeLists.txt" CRAB_CMAKE_CONTENT)
            # Match cmake_minimum_required with various formats (e.g., VERSION 2.8, 2.8.12, with FATAL_ERROR, etc.)
            string(REGEX REPLACE "cmake_minimum_required\\(VERSION [0-9]+\\.[0-9]+[^)]*\\)" 
                                 "cmake_minimum_required(VERSION 3.10)" 
                                 CRAB_CMAKE_CONTENT "${CRAB_CMAKE_CONTENT}")
            file(WRITE "${CRAB_ROOT}/CMakeLists.txt" "${CRAB_CMAKE_CONTENT}")
            message(STATUS "Patched CRAB CMakeLists.txt to require CMake 3.10+")
            
            message(STATUS "CRAB successfully downloaded to ${CRAB_ROOT}")
        else()
            message(STATUS "Using previously downloaded CRAB at: ${CRAB_ROOT}")
        endif()
    else()
        message(WARNING 
            "CRAB not found. Either:\n"
            "  1. Set -DCUSTOM_CRAB_ROOT=/path/to/crab, or\n"
            "  2. Vendor CRAB into third-party/crab, or\n"
            "  3. Enable auto-download with -DDOWNLOAD_CRAB=ON\n"
            "CLAM will be disabled.")
        set(ENABLE_CLAM OFF)
    endif()
    
    # Configure CRAB if found
    if(ENABLE_CLAM AND EXISTS "${CRAB_ROOT}/CMakeLists.txt")
        message(STATUS "Configuring CRAB at: ${CRAB_ROOT}")
        
        # Set CRAB options before including it
        set(CRAB_BUILD_LIBS_SHARED OFF)
        set(CRAB_USE_LDD OFF)
        set(CRAB_USE_APRON OFF)
        set(CRAB_USE_ELINA OFF)
        
        # Add CRAB as subdirectory
        add_subdirectory(${CRAB_ROOT} ${CMAKE_BINARY_DIR}/crab)
        
        # Include CRAB headers (legacy global include for compatibility)
        # Individual targets should use target_include_directories with SYSTEM
        # to suppress warnings from third-party CRAB library
        include_directories(BEFORE ${CRAB_ROOT}/include)
        
        # Configure CLAM config.h
        set(HAVE_LLVM_SEAHORN FALSE)
        set(USE_DBM_BIGNUM FALSE)
        set(USE_DBM_SAFEINT FALSE)
        set(INCLUDE_ALL_DOMAINS TRUE)
        set(CLAM_IS_TOPLEVEL FALSE)
        
        configure_file(
            ${CMAKE_CURRENT_SOURCE_DIR}/include/Verification/clam/config.h.cmake
            ${CMAKE_BINARY_DIR}/include/Verification/clam/config.h
        )
        
        include_directories(BEFORE ${CMAKE_BINARY_DIR}/include ${CMAKE_BINARY_DIR}/include/Verification)
        add_definitions(-DHAVE_CLAM)
        
        # Set CLAM libraries - must include both ClamAnalysis and Crab
        set(CLAM_LIBS ClamAnalysis Crab)
        
        message(STATUS "CLAM integration enabled with CRAB")
    endif()
endif()
