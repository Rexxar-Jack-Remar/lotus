#pragma once
namespace concurrency::runtime {
  enum class RuntimeKind { Unknown, PThread, OpenMP, MPI, Cpp, CUDA, LinuxKernel, Hare, Custom };
} // namespace concurrency::runtime