// Copyright (c) 2026 XiangZhang
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>

#include "api/stabilizer_api.h"
#include "api/stabilizer_c_api.h"

struct stabilizer_options {
    stabilizer::api::SMTStabilizerOptions value;
};

struct stabilizer_handle {
    stabilizer::api::SMTStabilizer stabilizer;
    std::string last_error;

    explicit stabilizer_handle(const stabilizer_options *options)
        : stabilizer(options ? options->value : stabilizer::api::SMTStabilizerOptions()) {}
};

namespace {

template <typename Fn>
stabilizer_status capture(stabilizer_handle *handle, char **output, Fn &&fn) {
    if (output != nullptr) {
        *output = nullptr;
    }
    if (handle == nullptr || output == nullptr) {
        if (handle != nullptr) {
            handle->last_error = "invalid argument";
        }
        return STABILIZER_STATUS_INVALID_ARGUMENT;
    }

    try {
        std::string result = fn();
        char *buffer = static_cast<char *>(std::malloc(result.size() + 1));
        if (buffer == nullptr) {
            throw std::bad_alloc();
        }
        std::memcpy(buffer, result.c_str(), result.size() + 1);
        *output = buffer;
        handle->last_error.clear();
        return STABILIZER_STATUS_OK;
    }
    catch (const std::invalid_argument &e) {
        handle->last_error = e.what();
        return STABILIZER_STATUS_INVALID_ARGUMENT;
    }
    catch (const std::exception &e) {
        handle->last_error = e.what();
        return STABILIZER_STATUS_RUNTIME_ERROR;
    }
    catch (...) {
        handle->last_error = "unknown error";
        return STABILIZER_STATUS_RUNTIME_ERROR;
    }
}

stabilizer::api::SMTStabilizerOptions current_options(const stabilizer_handle *handle) {
    return handle->stabilizer.options();
}

void apply_options(stabilizer_handle *handle, const stabilizer::api::SMTStabilizerOptions &options) {
    handle->stabilizer.set_options(options);
}

}  // namespace

stabilizer_options *stabilizer_options_create(void) {
    return new stabilizer_options();
}

void stabilizer_options_destroy(stabilizer_options *options) {
    delete options;
}

void stabilizer_options_set_rewrite(stabilizer_options *options, bool value) {
    if (options != nullptr) {
        options->value.set_rewrite(value);
    }
}

bool stabilizer_options_get_rewrite(const stabilizer_options *options) {
    return options != nullptr ? options->value.get_rewrite() : false;
}

void stabilizer_options_set_context_propagation(stabilizer_options *options, bool value) {
    if (options != nullptr) {
        options->value.set_context_propagation(value);
    }
}

bool stabilizer_options_get_context_propagation(const stabilizer_options *options) {
    return options != nullptr ? options->value.get_context_propagation() : false;
}

void stabilizer_options_set_subgraph_pruning(stabilizer_options *options, bool value) {
    if (options != nullptr) {
        options->value.set_subgraph_pruning(value);
    }
}

bool stabilizer_options_get_subgraph_pruning(const stabilizer_options *options) {
    return options != nullptr ? options->value.get_subgraph_pruning() : false;
}

stabilizer_handle *stabilizer_create(const stabilizer_options *options) {
    return new stabilizer_handle(options);
}

void stabilizer_destroy(stabilizer_handle *handle) {
    delete handle;
}

void stabilizer_set_rewrite(stabilizer_handle *handle, bool value) {
    if (handle == nullptr) {
        return;
    }
    auto options = current_options(handle);
    options.set_rewrite(value);
    apply_options(handle, options);
}

bool stabilizer_get_rewrite(const stabilizer_handle *handle) {
    return handle != nullptr ? handle->stabilizer.options().get_rewrite() : false;
}

void stabilizer_set_context_propagation(stabilizer_handle *handle, bool value) {
    if (handle == nullptr) {
        return;
    }
    auto options = current_options(handle);
    options.set_context_propagation(value);
    apply_options(handle, options);
}

bool stabilizer_get_context_propagation(const stabilizer_handle *handle) {
    return handle != nullptr ? handle->stabilizer.options().get_context_propagation() : false;
}

void stabilizer_set_subgraph_pruning(stabilizer_handle *handle, bool value) {
    if (handle == nullptr) {
        return;
    }
    auto options = current_options(handle);
    options.set_subgraph_pruning(value);
    apply_options(handle, options);
}

bool stabilizer_get_subgraph_pruning(const stabilizer_handle *handle) {
    return handle != nullptr ? handle->stabilizer.options().get_subgraph_pruning() : false;
}

stabilizer_status stabilizer_apply_file(stabilizer_handle *handle, const char *file_path, char **output) {
    if (handle == nullptr || file_path == nullptr || output == nullptr) {
        if (handle != nullptr) {
            handle->last_error = "invalid argument";
        }
        if (output != nullptr) {
            *output = nullptr;
        }
        return STABILIZER_STATUS_INVALID_ARGUMENT;
    }
    return capture(handle, output, [handle, file_path]() { return handle->stabilizer.apply_file(file_path); });
}

stabilizer_status stabilizer_apply_text(stabilizer_handle *handle, const char *smt2_text, char **output) {
    if (handle == nullptr || smt2_text == nullptr || output == nullptr) {
        if (handle != nullptr) {
            handle->last_error = "invalid argument";
        }
        if (output != nullptr) {
            *output = nullptr;
        }
        return STABILIZER_STATUS_INVALID_ARGUMENT;
    }
    return capture(handle, output, [handle, smt2_text]() { return handle->stabilizer.apply_text(smt2_text); });
}

const char *stabilizer_last_error(const stabilizer_handle *handle) {
    return handle != nullptr ? handle->last_error.c_str() : "invalid handle";
}

void stabilizer_free_string(char *value) {
    std::free(value);
}