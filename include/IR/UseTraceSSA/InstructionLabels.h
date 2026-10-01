#pragma once

#include <llvm/IR/AssemblyAnnotationWriter.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/FormattedStream.h>
#include <functional>
#include <unordered_map>

namespace lotus {
namespace usetracessa {
namespace detail {

// Value::print creates an AssemblyWriter for every instruction, even with a
// shared slot tracker. Its constructor scans all global objects. Capture labels
// during one module print instead, discarding everything outside selected
// instructions. Annotation boundaries also handle multiline instructions.
class InstructionLabels : private llvm::AssemblyAnnotationWriter {
  class CaptureStream : public llvm::raw_ostream {
    std::uint64_t Position = 0;
    void write_impl(const char *data, std::size_t size) override {
      if (Current) Current->append(data, size);
      Position += size;
    }
    std::uint64_t current_pos() const override { return Position; }
  public:
    std::string *Current = nullptr;
  } Output;
  std::unordered_map<const llvm::Instruction *, std::string> Labels;
  bool SourceLocations = true;

  void emitInstructionAnnot(const llvm::Instruction *instruction,
                             llvm::formatted_raw_ostream &out) override {
    out.flush();
    auto found = Labels.find(instruction);
    Output.Current = found == Labels.end() ? nullptr : &found->second;
  }
  void printInfoComment(const llvm::Value &value,
                         llvm::formatted_raw_ostream &out) override {
    if (!Output.Current) return;
    out.flush();
    const auto &instruction = llvm::cast<llvm::Instruction>(value);
    if (const llvm::DebugLoc &location = instruction.getDebugLoc();
        SourceLocations && location) {
      llvm::raw_string_ostream label(*Output.Current);
      label << " [" << location->getFilename() << ':' << location.getLine() << ':'
            << location.getCol() << ']';
    }
    Output.Current = nullptr;
  }

public:
  InstructionLabels(
      const llvm::Module &module,
      const std::function<bool(const llvm::Instruction &)> &select,
      bool instructionText = true, bool sourceLocations = true)
      : SourceLocations(sourceLocations) {
    for (const auto &function : module) {
      std::size_t site = 0;
      for (const auto &block : function)
        for (const auto &instruction : block) {
          ++site;
          if (!select(instruction))
            continue;
          std::string label;
          if (!instructionText) {
            llvm::raw_string_ostream out(label);
            out << instruction.getOpcodeName() << " site " << site;
            if (const llvm::DebugLoc &location = instruction.getDebugLoc();
                SourceLocations && location)
              out << " [" << location->getFilename() << ':'
                  << location.getLine() << ':' << location.getCol() << ']';
          }
          Labels.emplace(&instruction, std::move(label));
        }
    }
    if (!instructionText || Labels.empty())
      return;
    module.print(Output, this);
    Output.flush();
  }
  const std::string &get(const llvm::Instruction &instruction) const {
    return Labels.at(&instruction);
  }
};

} // namespace detail
} // namespace usetracessa
} // namespace lotus
