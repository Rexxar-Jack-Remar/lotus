#pragma once

namespace llvm {
class raw_ostream;
} // namespace llvm

namespace annotation {

class ExternalPointerTable;

class ExternalPointerTablePrinter {
private:
  llvm::raw_ostream &os;

public:
  ExternalPointerTablePrinter(llvm::raw_ostream &o) : os(o) {}

  void printTable(const ExternalPointerTable &);
};

} // namespace annotation

