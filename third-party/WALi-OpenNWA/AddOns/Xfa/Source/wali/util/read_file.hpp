#ifndef WALI_UTIL_READ_FILE_HPP_INCLUDED
#define WALI_UTIL_READ_FILE_HPP_INCLUDED

#include <cstdio>
#include <vector>

namespace wali {
    namespace util {
        /// Read all of the contents of 'file', returning the result.
        ///
        /// The use of FILE* is (at least originally) for compatibility with
        /// popen().
        extern
        std::vector<char>
        read_file(std::FILE* file);
    } // namespace util
} // namespace wali

#endif /* WALI_UTIL_READ_FILE_HPP_INCLUDED */
