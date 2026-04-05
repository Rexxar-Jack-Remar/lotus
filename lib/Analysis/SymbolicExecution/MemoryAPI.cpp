
#include "Analysis/SymbolicExecution/MemoryAPI.h"

#include "llvm/Support/CommandLine.h"

#include "Analysis/SymbolicExecution/SegUtility.h"

#include <map>

static cl::opt<bool>
    KernelAnalysis("symex-analyze-kernel",
                   cl::desc("run the bof analysis for Linux kernel"),
                   cl::init(false), cl::ReallyHidden);

using namespace SymbolicExecution;

std::string AllocatorAPI::toString() const {
  std::string Desc = "alloc(size=";
  if (isArrayAlloc()) {
    Desc += "arg" + std::to_string(SizeArgs[0]);
    Desc += "*";
    Desc += "arg" + std::to_string(SizeArgs[1]);
  } else {
    Desc += "arg" + std::to_string(SizeArgs[0]);
  }

  if (ArrayAllocation) {
    Desc += ", array";
  }

  if (hasValidGFPFlag()) {
    Desc += ", gfp=arg" + std::to_string(GFPFlag);
  }

  if (Zeroed) {
    Desc += ", zeroed";
  }

  Desc += ")";
  return Desc;
}

static AllocatorAPI LibcAllocators[] = {
    // void *malloc( size_t size );
    AllocatorAPI("malloc", {0}),
    // void *calloc( size_t num, size_t size );
    AllocatorAPI("calloc", {0, 1}, true, -1, true),
    // void *realloc( void *ptr, size_t new_size );
    AllocatorAPI("realloc", {1}),
    AllocatorAPI("memalign", {1}),
    AllocatorAPI("aligned_alloc", {1}),
    AllocatorAPI("pvalloc", {0}),
    AllocatorAPI("valloc", {0}),
    AllocatorAPI("strndup", {1}),

    //"_Znwj", "_ZnwjRKSt9nothrow_t": operator new(unsigned int)
    //"_Znwm", "_ZnwmRKSt9nothrow_t": operator new(unsigned long)
    //"_Znaj", "_ZnajRKSt9nothrow_t": operator new[](unsigned int)
    //"_Znam", "_ZnamRKSt9nothrow_t": operator new[](unsigned long)
    // "_ZnwmPv"
    AllocatorAPI("_Znwj", {0}),
    AllocatorAPI("_ZnwjRKSt9nothrow_t", {0}),
    AllocatorAPI("_Znwm", {0}),
    AllocatorAPI("_ZnwmRKSt9nothrow_t", {0}),
    AllocatorAPI("_Znaj", {0}),
    AllocatorAPI("_ZnajRKSt9nothrow_t", {0}),
    AllocatorAPI("_Znam", {0}),
    AllocatorAPI("_ZnamRKSt9nothrow_t", {0}),
};

static AllocatorAPI KernelAllocators[] = {
    // void *kmalloc(size_t size, gfp_t flags)
    // void *kzalloc(size_t size, gfp_t flags)
    AllocatorAPI("kmalloc", {0}, false, 1),
    AllocatorAPI("kzalloc", {0}, false, 1, true),
    // void *devm_kmalloc(struct device *dev, size_t size, gfp_t gfp)
    // void *devm_kzalloc(struct device *dev, size_t size, gfp_t gfp)
    AllocatorAPI("devm_kmalloc", {1}, false, 2),
    AllocatorAPI("devm_kzalloc", {1}, false, 2, true),
    // void *kmalloc_array(size_t n, size_t size, gfp_t flags)
    AllocatorAPI("kmalloc_array", {0, 1}, true, 2),
    // void *kcalloc(size_t n, size_t size, gfp_t flags)
    AllocatorAPI("kcalloc", {0, 1}, true, 2, true),
    // void *krealloc(const void *p, size_t new_size, gfp_t flags)
    AllocatorAPI("krealloc", {1}, false, 2),
    // void * krealloc_array(void *p, size_t new_n, size_t new_size, gfp_t
    // flags)
    AllocatorAPI("krealloc_array", {1, 2}, true, 3),

    // void *kmalloc_node(size_t size, gfp_t flags, int node)
    AllocatorAPI("kmalloc_node", {0}, false, 1),
    AllocatorAPI("__kmalloc_node", {0}, false, 1),
    // void *kmalloc_array_node(size_t n, size_t size, gfp_t flags, int node)
    AllocatorAPI("kmalloc_array_node", {0, 1}, true, 2),
    // void *kcalloc_node(size_t n, size_t size, gfp_t flags, int node)
    AllocatorAPI("kcalloc_node", {0, 1}, true, 2, true),
    // void *kzalloc_node(size_t size, gfp_t flags, int node)
    AllocatorAPI("kzalloc_node", {0}, false, 1, true),
    // void *kvmalloc(size_t size, gfp_t flags)
    // void *kvmalloc_node(size_t size, gfp_t flags, int node)
    AllocatorAPI("kvmalloc", {0}, false, 1),
    AllocatorAPI("kvmalloc_node", {0}, false, 1),
    // void *kvzalloc_node(size_t size, gfp_t flags, int node)
    AllocatorAPI("kvzalloc_node", {0}, false, 1),
    // void *kvzalloc(size_t size, gfp_t flags)
    AllocatorAPI("kvzalloc", {0}, false, 1),
    // void *kvmalloc_array(size_t n, size_t size, gfp_t flags)
    AllocatorAPI("kvmalloc_array", {0, 1}, true, 2),
    // void *kvcalloc(size_t n, size_t size, gfp_t flags)
    AllocatorAPI("kvcalloc", {0, 1}, true, 2, true),
    // void *kvrealloc(const void *p, size_t oldsize, size_t newsize, gfp_t
    // flags)
    AllocatorAPI("kvrealloc", {2}, false, 3),
};

const AllocatorAPI *AllocatorAPI::get(Function *F) {
  if (!F || !F->getReturnType()->isPointerTy()) {
    return nullptr;
  }

  std::string FunName = F->getName().str();
  std::vector<Argument *> Params;
  for (auto &V : F->args()) {
    Params.emplace_back(&V);
  }

  auto checkIntIdx = [&Params](unsigned Idx) {
    return Idx < Params.size() && Params[Idx]->getType()->isIntegerTy();
  };

  auto *APIBegin = KernelAnalysis ? std::begin(KernelAllocators)
                                  : std::begin(LibcAllocators);
  auto *APIEnd =
      KernelAnalysis ? std::end(KernelAllocators) : std::end(LibcAllocators);

  for (auto *Iter = APIBegin; Iter != APIEnd; ++Iter) {
    const auto &API = *Iter;

    std::string APIName = API.getName();
    bool Match = false;

    if (KernelAnalysis) {
      if (APIName == FunName || ("__" + APIName == FunName)) { // __kmalloc
        Match = true;
      } else if (FunName.rfind(APIName, 0) == 0) {
        // The kernel has many functions of the form kvmalloc1223959, i.e.,
        // prefix+numbers
        if (std::find_if(FunName.begin() + APIName.size(), FunName.end(),
                         [](unsigned char c) { return !std::isdigit(c); }) ==
            FunName.end()) {
          Match = true;
        }
      }
    } else {
      Match = (FunName.find(APIName) != std::string::npos);
    }

    if (Match) {
      for (auto Idx : API.getSizeArgs()) {
        if (!checkIntIdx(Idx)) {
          Match = false;
          break;
        }
      }

      if (Match && API.hasValidGFPFlag()) {
        Match = checkIntIdx(API.getGFPFlag());
      }
    }

    if (Match) {
      return &API;
    }
  }

  return nullptr;
}
