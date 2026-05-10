#include "Dataflow/VASCO/Support/LLVMPointsToMemory.h"

#include <llvm/IR/Attributes.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Operator.h>
#include <llvm/Support/TypeSize.h>

namespace vasco {
namespace llvmir {

MemoryLayout MemoryModel::layoutForStack(const llvm::AllocaInst *Alloca) const {
  if (Alloca == nullptr) {
    return MemoryLayout::unknown();
  }
  return layoutForPointeeType(Alloca->getAllocatedType());
}

MemoryLayout
MemoryModel::layoutForGlobal(const llvm::GlobalVariable *Global) const {
  if (Global == nullptr) {
    return MemoryLayout::unknown();
  }
  return layoutForPointeeType(Global->getValueType());
}

MemoryLayout
MemoryModel::layoutForArgument(const llvm::Argument *Argument) const {
  if (Argument == nullptr || !Argument->getType()->isPointerTy()) {
    return MemoryLayout::unknown();
  }
  return layoutForPointeeType(Argument->getType()->getPointerElementType());
}

MemoryLayout MemoryModel::layoutForFunction(const llvm::Function *) const {
  return MemoryLayout::unknown();
}

MemoryLayout MemoryModel::layoutForHeapCall(const llvm::CallBase *Call) const {
  if (Call == nullptr) {
    return MemoryLayout::unknown();
  }

  if (auto *Type = inferHeapPointeeType(Call)) {
    const bool CollapseArrays = llvm::isa<llvm::ArrayType>(Type);
    auto Layout = layoutForPointeeType(Type, CollapseArrays);
    if (auto Size = constantAllocationSize(Call)) {
      Layout.HasKnownSize = true;
      Layout.Size = *Size;
    }
    return Layout;
  }

  MemoryLayout Layout = MemoryLayout::unknown();
  if (auto Size = constantAllocationSize(Call)) {
    Layout.HasKnownSize = true;
    Layout.Size = *Size;
  }
  return Layout;
}

MemoryLayout
MemoryModel::layoutForReallocCall(const llvm::CallBase *Call,
                                  const MemoryBlock &Original) const {
  auto Layout = layoutForHeapCall(Call);
  if (Layout.PointeeType == nullptr && Original.Layout.PointeeType != nullptr) {
    Layout.PointeeType = Original.Layout.PointeeType;
    Layout.FieldSensitive = Original.Layout.FieldSensitive;
    Layout.CollapsesArrayElements = Original.Layout.CollapsesArrayElements;
  }
  if (!Layout.HasKnownSize && Original.Layout.HasKnownSize) {
    Layout.HasKnownSize = true;
    Layout.Size = Original.Layout.Size;
  }
  return Layout;
}

MemoryLayout MemoryModel::layoutForValue(const llvm::Value *Value) const {
  if (auto *Alloca = llvm::dyn_cast_or_null<llvm::AllocaInst>(Value)) {
    return layoutForStack(Alloca);
  }
  if (auto *Global = llvm::dyn_cast_or_null<llvm::GlobalVariable>(Value)) {
    return layoutForGlobal(Global);
  }
  if (auto *Argument = llvm::dyn_cast_or_null<llvm::Argument>(Value)) {
    return layoutForArgument(Argument);
  }
  if (auto *Function = llvm::dyn_cast_or_null<llvm::Function>(Value)) {
    return layoutForFunction(Function);
  }
  if (auto *Call = llvm::dyn_cast_or_null<llvm::CallBase>(Value)) {
    return layoutForHeapCall(Call);
  }
  if (Value != nullptr && Value->getType()->isPointerTy()) {
    return layoutForPointeeType(Value->getType()->getPointerElementType());
  }
  return MemoryLayout::unknown();
}

std::optional<MemoryLocation>
MemoryModel::getFieldLocation(const MemoryLocation &Base,
                              const llvm::Value *PointerOperand) const {
  if (!Base.Object.Layout.FieldSensitive || Base.IsSummary) {
    return MemoryLocation::summary(Base.Object, Base.Offset);
  }

  auto Offset = computeOffset(PointerOperand, Base.Object.Layout);
  if (!Offset.has_value()) {
    return MemoryLocation::summary(Base.Object, Base.Offset);
  }

  return MemoryLocation::exact(Base.Object, Base.Offset + *Offset);
}

std::optional<MemoryLocation>
MemoryModel::getFieldLocationFromOffset(const MemoryLocation &Base,
                                        std::int64_t AdditionalOffset) const {
  if (!Base.Object.Layout.FieldSensitive || Base.IsSummary) {
    return MemoryLocation::summary(Base.Object, Base.Offset);
  }

  auto FinalOffset = Base.Offset + AdditionalOffset;
  if (Base.Object.Layout.CollapsesArrayElements &&
      Base.Object.Layout.HasKnownSize && Base.Object.Layout.Size != 0) {
    FinalOffset %= static_cast<std::int64_t>(Base.Object.Layout.Size);
  }

  return MemoryLocation::exact(Base.Object, FinalOffset);
}

bool MemoryModel::isHeapAllocator(const llvm::CallBase *Call) const {
  if (Call == nullptr || !Call->getType()->isPointerTy()) {
    return false;
  }

  if (Call->hasRetAttr(llvm::Attribute::NoAlias) ||
      Call->hasFnAttr(llvm::Attribute::NoAlias)) {
    return true;
  }

  auto *Callee = Call->getCalledFunction();
  if (Callee == nullptr) {
    return false;
  }

  if (Callee->hasFnAttribute(llvm::Attribute::AllocSize) ||
      Call->hasFnAttr(llvm::Attribute::AllocSize)) {
    return true;
  }

  const auto Name = Callee->getName();
  return Name == "malloc" || Name == "calloc" || Name == "realloc" ||
         Name == "reallocarray" || Name == "memalign" ||
         Name == "aligned_alloc" || Name == "valloc" || Name == "pvalloc" ||
         Name == "posix_memalign" || Name == "mmap" || Name == "mmap64" ||
         Name == "strdup" || Name == "strndup" || Name == "getline" ||
         Name == "getdelim" || Name == "_Znwm" || Name == "_Znam" ||
         Name == "_ZnwmRKSt9nothrow_t" || Name == "_ZnamRKSt9nothrow_t";
}

bool MemoryModel::isReallocLikeAllocator(const llvm::CallBase *Call) const {
  auto *Callee = Call != nullptr ? Call->getCalledFunction() : nullptr;
  if (Callee == nullptr) {
    return false;
  }
  const auto Name = Callee->getName();
  return Name == "realloc" || Name == "reallocarray";
}

bool MemoryModel::isMemcpyLikeCall(const llvm::CallBase *Call) const {
  auto *Callee = Call != nullptr ? Call->getCalledFunction() : nullptr;
  return Callee != nullptr && Callee->getName().startswith("llvm.memcpy");
}

bool MemoryModel::isMemmoveLikeCall(const llvm::CallBase *Call) const {
  auto *Callee = Call != nullptr ? Call->getCalledFunction() : nullptr;
  return Callee != nullptr && Callee->getName().startswith("llvm.memmove");
}

bool MemoryModel::isMemsetLikeCall(const llvm::CallBase *Call) const {
  auto *Callee = Call != nullptr ? Call->getCalledFunction() : nullptr;
  return Callee != nullptr && Callee->getName().startswith("llvm.memset");
}

ExternalCallSummary
MemoryModel::classifyExternalCall(const llvm::CallBase *Call) const {
  ExternalCallSummary Summary;
  if (Call == nullptr) {
    return Summary;
  }

  // Direct dispatch to the existing classifiers first so callers can rely on a
  // single classify call.
  if (isHeapAllocator(Call)) {
    Summary.Effect = ExternalCallEffect::Alloc;
    return Summary;
  }
  if (isMemcpyLikeCall(Call) || isMemmoveLikeCall(Call)) {
    Summary.Effect = ExternalCallEffect::Memcopy;
    Summary.DestArgIndex = 0;
    Summary.SrcArgIndex = 1;
    Summary.ReturnsDestArg =
        Call->getType()->isPointerTy(); // memcpy returns dst in libc, void in
                                        // intrinsic; safe either way.
    return Summary;
  }
  if (isMemsetLikeCall(Call)) {
    Summary.Effect = ExternalCallEffect::Memset;
    Summary.DestArgIndex = 0;
    Summary.ReturnsDestArg = Call->getType()->isPointerTy();
    return Summary;
  }

  auto *Callee = Call->getCalledFunction();
  if (Callee == nullptr || !Callee->isDeclaration()) {
    Summary.Effect = ExternalCallEffect::Unknown;
    return Summary;
  }

  // Skip LLVM intrinsics that have already been claimed by the dedicated
  // helpers above; everything else (debug, lifetime, etc.) is a no-op.
  if (Callee->isIntrinsic()) {
    if (Callee->getName().startswith("llvm.va_") ||
        Callee->getName().startswith("llvm.lifetime") ||
        Callee->getName().startswith("llvm.dbg") ||
        Callee->getName().startswith("llvm.assume") ||
        Callee->getName().startswith("llvm.expect") ||
        Callee->getName().startswith("llvm.invariant")) {
      Summary.Effect = ExternalCallEffect::Noop;
      return Summary;
    }
    // Treat unknown intrinsics conservatively.
    Summary.Effect = ExternalCallEffect::Unknown;
    return Summary;
  }

  const auto Name = Callee->getName();

  // strdup-family: returns a fresh heap object whose contents summarize source.
  if (Name == "strdup" || Name == "strndup" || Name == "wcsdup" ||
      Name == "__strdup") {
    Summary.Effect = ExternalCallEffect::StringDup;
    Summary.SrcArgIndex = 0;
    return Summary;
  }

  // Library-level memcpy/strcpy/strncpy/strcat/strncat all copy bytes from the
  // second pointer argument into the first.
  if (Name == "strcpy" || Name == "stpcpy" || Name == "strncpy" ||
      Name == "stpncpy" || Name == "strcat" || Name == "strncat" ||
      Name == "wcscpy" || Name == "wcsncpy" || Name == "wcscat" ||
      Name == "wcsncat" || Name == "memcpy" || Name == "memmove" ||
      Name == "mempcpy" || Name == "bcopy") {
    Summary.Effect = ExternalCallEffect::Memcopy;
    if (Name == "bcopy") {
      // bcopy(src, dst, len)
      Summary.SrcArgIndex = 0;
      Summary.DestArgIndex = 1;
    } else {
      Summary.DestArgIndex = 0;
      Summary.SrcArgIndex = 1;
    }
    Summary.ReturnsDestArg = Call->getType()->isPointerTy();
    return Summary;
  }

  if (Name == "memset" || Name == "bzero" || Name == "explicit_bzero" ||
      Name == "memset_s" || Name == "wmemset") {
    Summary.Effect = ExternalCallEffect::Memset;
    Summary.DestArgIndex = 0;
    Summary.ReturnsDestArg = Call->getType()->isPointerTy();
    return Summary;
  }

  // Functions that return a pointer aliasing one of their arguments.
  if (Name == "strchr" || Name == "strrchr" || Name == "strstr" ||
      Name == "strcasestr" || Name == "strpbrk" || Name == "strtok" ||
      Name == "strtok_r" || Name == "memchr" || Name == "memrchr" ||
      Name == "rawmemchr" || Name == "wcschr" || Name == "wcsrchr" ||
      Name == "wcsstr" || Name == "index" || Name == "rindex" ||
      Name == "wmemchr") {
    Summary.Effect = ExternalCallEffect::ReturnsArgument;
    Summary.SrcArgIndex = 0;
    return Summary;
  }

  // Functions that return status codes but publish a fresh heap pointer
  // through an output parameter.
  if (Name == "posix_memalign" || Name == "getline" || Name == "getdelim" ||
      Name == "asprintf" || Name == "vasprintf") {
    Summary.Effect = ExternalCallEffect::AllocatesIntoArgument;
    Summary.DestArgIndex = 0;
    return Summary;
  }

  // Functions known to return an opaque pointer that does not alias their
  // arguments.
  if (Name == "fopen" || Name == "fdopen" || Name == "freopen" ||
      Name == "tmpfile" || Name == "popen" || Name == "getenv" ||
      Name == "secure_getenv" || Name == "dlopen" || Name == "dlsym" ||
      Name == "opendir" || Name == "fdopendir" || Name == "ttyname" ||
      Name == "ctime" || Name == "asctime" || Name == "gmtime" ||
      Name == "localtime" || Name == "setlocale" || Name == "nl_langinfo") {
    Summary.Effect = ExternalCallEffect::OpaqueReturn;
    return Summary;
  }

  if (Name == "exit" || Name == "_exit" || Name == "_Exit" ||
      Name == "abort" || Name == "__assert_fail" || Name == "__stack_chk_fail" ||
      Name == "longjmp" || Name == "siglongjmp" || Name == "pthread_exit") {
    Summary.Effect = ExternalCallEffect::Exit;
    return Summary;
  }

  // I/O / pure functions that do not interact with pointer state in a
  // meaningful way (no pointer return, arguments only consumed).
  if (Name == "puts" || Name == "perror" || Name == "fputs" ||
      Name == "fputc" || Name == "putc" || Name == "putchar" ||
      Name == "fflush" || Name == "fclose" || Name == "fseek" ||
      Name == "ftell" || Name == "rewind" || Name == "feof" ||
      Name == "ferror" || Name == "clearerr" || Name == "setvbuf" ||
      Name == "setbuf" || Name == "fileno" || Name == "isatty" ||
      Name == "atoi" || Name == "atol" || Name == "atoll" ||
      Name == "atof" || Name == "strtod" || Name == "strtof" ||
      Name == "strtol" || Name == "strtoll" || Name == "strtoul" ||
      Name == "strtoull" || Name == "strlen" || Name == "wcslen" ||
      Name == "strcmp" || Name == "strncmp" || Name == "strcasecmp" ||
      Name == "strncasecmp" || Name == "memcmp" || Name == "wcscmp" ||
      Name == "wcsncmp" || Name == "abs" || Name == "labs" ||
      Name == "llabs" || Name == "rand" || Name == "srand" ||
      Name == "getpid" || Name == "getppid" || Name == "geteuid" ||
      Name == "getuid" || Name == "getgid" || Name == "getegid" ||
      Name == "time" || Name == "clock" || Name == "sleep" ||
      Name == "usleep" || Name == "system") {
    Summary.Effect = ExternalCallEffect::Noop;
    return Summary;
  }

  // printf-family: reads (possibly through pointers) but does not write to the
  // analyzed program's memory through them.
  if (Name == "printf" || Name == "fprintf" || Name == "vprintf" ||
      Name == "vfprintf" || Name == "dprintf" || Name == "vdprintf" ||
      Name == "asprintf" || Name == "vasprintf" || Name == "sprintf" ||
      Name == "snprintf" || Name == "vsprintf" || Name == "vsnprintf" ||
      Name == "fwrite" || Name == "fread" || Name == "write" ||
      Name == "read" || Name == "open" || Name == "close" ||
      Name == "stat" || Name == "fstat" || Name == "lstat" ||
      Name == "unlink" || Name == "rename" || Name == "remove" ||
      Name == "mkdir" || Name == "rmdir" || Name == "access" ||
      Name == "chmod" || Name == "chown" || Name == "fcntl" ||
      Name == "ioctl" || Name == "kill" || Name == "raise" ||
      Name == "signal" || Name == "sigaction" || Name == "sigprocmask") {
    // Many of these write to user-supplied buffers via pointer arguments
    // (sprintf, fread, read, stat, ...). Mark them as Unknown so the analyzer
    // summarizes their pointer arguments.
    if (Name == "sprintf" || Name == "snprintf" || Name == "vsprintf" ||
        Name == "vsnprintf" || Name == "asprintf" || Name == "vasprintf" ||
        Name == "fread" || Name == "read" || Name == "stat" ||
        Name == "fstat" || Name == "lstat") {
      Summary.Effect = ExternalCallEffect::Unknown;
      return Summary;
    }
    Summary.Effect = ExternalCallEffect::Noop;
    return Summary;
  }

  Summary.Effect = ExternalCallEffect::Unknown;
  return Summary;
}

MemoryLayout MemoryModel::layoutForPointeeType(const llvm::Type *Type,
                                               bool CollapseArrays) const {
  MemoryLayout Layout;
  Layout.PointeeType = Type;
  Layout.FieldSensitive = supportsFieldSensitivity(Type);
  Layout.CollapsesArrayElements =
      CollapseArrays && llvm::isa_and_nonnull<llvm::ArrayType>(Type);
  if (auto Size = allocSizeOf(Type)) {
    Layout.HasKnownSize = true;
    Layout.Size = *Size;
  }
  return Layout;
}

llvm::Type *
MemoryModel::inferHeapPointeeType(const llvm::CallBase *Call) const {
  if (auto *Type = nextBitcastPointeeType(Call)) {
    if (Type->isSized()) {
      return Type;
    }
  }

  if (auto *Type = inferTypedAllocationFromReturnUses(Call)) {
    if (Type->isSized()) {
      return Type;
    }
  }

  if (Call != nullptr && Call->getType()->isPointerTy()) {
    auto *Pointee = Call->getType()->getPointerElementType();
    if (Pointee->isSized() && !Pointee->isIntegerTy(8)) {
      return Pointee;
    }
  }
  return nullptr;
}

llvm::Type *MemoryModel::inferTypedAllocationFromReturnUses(
    const llvm::CallBase *Call) const {
  if (Call == nullptr) {
    return nullptr;
  }

  auto CandidateFromPointer = [](llvm::Type *PointerTy) -> llvm::Type * {
    if (PointerTy == nullptr || !PointerTy->isPointerTy()) {
      return nullptr;
    }
    auto *Pointee = PointerTy->getPointerElementType();
    if (Pointee == nullptr || !Pointee->isSized() || Pointee->isIntegerTy(8)) {
      return nullptr;
    }
    return Pointee;
  };

  for (const auto *User : Call->users()) {
    if (auto *BitCast = llvm::dyn_cast<llvm::BitCastInst>(User)) {
      if (auto *Type = CandidateFromPointer(BitCast->getDestTy())) {
        return Type;
      }
      continue;
    }
    if (auto *AddrCast = llvm::dyn_cast<llvm::AddrSpaceCastInst>(User)) {
      if (auto *Type = CandidateFromPointer(AddrCast->getDestTy())) {
        return Type;
      }
      continue;
    }
    if (auto *GEP = llvm::dyn_cast<llvm::GetElementPtrInst>(User)) {
      if (auto *Type = GEP->getSourceElementType()) {
        if (Type->isSized() && !Type->isIntegerTy(8)) {
          return Type;
        }
      }
      continue;
    }
    if (auto *Store = llvm::dyn_cast<llvm::StoreInst>(User)) {
      auto *PointerTy = llvm::dyn_cast<llvm::PointerType>(
          Store->getPointerOperand()->getType());
      if (PointerTy == nullptr) {
        continue;
      }
      auto *StoredTy = PointerTy->getPointerElementType();
      if (auto *Type = CandidateFromPointer(StoredTy)) {
        return Type;
      }
      continue;
    }
    if (auto *CallUser = llvm::dyn_cast<llvm::CallBase>(User)) {
      auto *Callee = CallUser->getCalledFunction();
      if (Callee == nullptr) {
        continue;
      }
      for (unsigned I = 0; I < CallUser->arg_size() && I < Callee->arg_size();
           ++I) {
        if (CallUser->getArgOperand(I) != Call) {
          continue;
        }
        auto *ArgIt = Callee->arg_begin();
        std::advance(ArgIt, I);
        if (auto *Type = CandidateFromPointer(ArgIt->getType())) {
          return Type;
        }
      }
    }
  }

  return nullptr;
}

llvm::Type *MemoryModel::nextBitcastPointeeType(
    const llvm::Instruction *Instruction) const {
  if (Instruction == nullptr) {
    return nullptr;
  }

  const llvm::Instruction *Next = nullptr;
  if (auto *Call = llvm::dyn_cast<llvm::CallInst>(Instruction)) {
    Next = Call->getNextNode();
  } else if (auto *Invoke = llvm::dyn_cast<llvm::InvokeInst>(Instruction)) {
    Next = Invoke->getNormalDest()->getFirstNonPHIOrDbgOrLifetime();
  }

  if (auto *BitCast = llvm::dyn_cast_or_null<llvm::BitCastInst>(Next)) {
    auto *DestTy = BitCast->getDestTy();
    if (DestTy->isPointerTy()) {
      return DestTy->getPointerElementType();
    }
  }
  return nullptr;
}

std::optional<std::uint64_t>
MemoryModel::constantAllocationSize(const llvm::CallBase *Call) const {
  if (Call == nullptr || Call->arg_empty()) {
    return std::nullopt;
  }

  auto *Callee = Call->getCalledFunction();
  const auto Name = Callee != nullptr ? Callee->getName() : llvm::StringRef();
  auto ConstantArg = [&](unsigned Index) -> std::optional<std::uint64_t> {
    if (Index >= Call->arg_size()) {
      return std::nullopt;
    }
    if (auto *Const =
            llvm::dyn_cast<llvm::ConstantInt>(Call->getArgOperand(Index))) {
      return Const->getZExtValue();
    }
    return std::nullopt;
  };

  if (Name == "calloc" || Name == "reallocarray") {
    auto Count = ConstantArg(0);
    auto Size = ConstantArg(1);
    if (Count && Size) {
      return *Count * *Size;
    }
    return std::nullopt;
  }

  if (Name == "realloc") {
    return ConstantArg(1);
  }

  if (Name == "aligned_alloc" || Name == "memalign") {
    return ConstantArg(1);
  }

  if (Callee != nullptr && Callee->hasFnAttribute(llvm::Attribute::AllocSize)) {
    const auto Attr = Callee->getFnAttribute(llvm::Attribute::AllocSize);
    const auto Indices = Attr.getAllocSizeArgs();
    auto Size0 = ConstantArg(Indices.first);
    if (!Size0) {
      return std::nullopt;
    }
    if (Indices.second.hasValue()) {
      auto Size1 = ConstantArg(*Indices.second);
      if (!Size1) {
        return std::nullopt;
      }
      return (*Size0) * (*Size1);
    }
    return *Size0;
  }

  return ConstantArg(0);
}

std::optional<std::int64_t>
MemoryModel::computeOffset(const llvm::Value *PointerOperand,
                           const MemoryLayout &LayoutInfo) const {
  if (PointerOperand == nullptr || LayoutInfo.PointeeType == nullptr) {
    return std::nullopt;
  }

  if (auto *GEP = llvm::dyn_cast<llvm::GEPOperator>(PointerOperand)) {
    llvm::APInt OffsetBits(
        Layout.getIndexTypeSizeInBits(PointerOperand->getType()), 0, true);
    if (!GEP->accumulateConstantOffset(Layout, OffsetBits)) {
      return std::nullopt;
    }

    auto Offset = static_cast<std::int64_t>(OffsetBits.getSExtValue());
    if (LayoutInfo.CollapsesArrayElements && LayoutInfo.HasKnownSize &&
        LayoutInfo.Size != 0) {
      Offset %= static_cast<std::int64_t>(LayoutInfo.Size);
    }
    return Offset;
  }

  return 0;
}

bool MemoryModel::supportsFieldSensitivity(const llvm::Type *Type) const {
  if (Type == nullptr) {
    return false;
  }

  if (auto *Struct = llvm::dyn_cast<llvm::StructType>(Type)) {
    return Struct->isSized();
  }
  if (auto *Array = llvm::dyn_cast<llvm::ArrayType>(Type)) {
    return Array->getElementType()->isSized();
  }
  return Type->isSized();
}

std::optional<std::uint64_t>
MemoryModel::allocSizeOf(const llvm::Type *Type) const {
  if (Type == nullptr || !Type->isSized()) {
    return std::nullopt;
  }
  return Layout.getTypeAllocSize(const_cast<llvm::Type *>(Type));
}

} // namespace llvmir
} // namespace vasco
