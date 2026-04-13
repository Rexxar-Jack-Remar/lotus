// LLVM API compatibility helpers
#pragma once

#include <llvm/Config/llvm-config.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DIBuilder.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Dominators.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Support/Allocator.h>
#include <llvm/Support/FileSystem.h>
#include <string>

#if LLVM_VERSION_MAJOR >= 14
#define LOTUS_LLVM_COMPAT_GE_14 1
#else
#define LOTUS_LLVM_COMPAT_GE_14 0
#endif

#if LLVM_VERSION_MAJOR >= 15
#define LOTUS_LLVM_COMPAT_GE_15 1
#else
#define LOTUS_LLVM_COMPAT_GE_15 0
#endif

// LLVM 14 already uses most of the APIs that this header historically gated
// behind USE_LLVM_15. Keep the legacy branches intact, but route 14+ builds
// through the newer code paths while preserving truly 15-only checks below.
#if LOTUS_LLVM_COMPAT_GE_14 && !defined(USE_LLVM_6_TO_9) && !defined(USE_LLVM_15)
#define LOTUS_LLVM_COMPAT_TEMP_USE_LLVM_6_TO_9 1
#define USE_LLVM_6_TO_9 1
#endif

#if LOTUS_LLVM_COMPAT_GE_14 || defined(USE_LLVM_15)
#include <llvm/Analysis/ConstantFolding.h>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/IR/DebugInfoMetadata.h>
#elif defined(USE_LLVM_6_TO_9)
#include <llvm/IR/ConstantFold.h>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/IR/DebugInfoMetadata.h>
#else
#include <llvm/Bitcode/ReaderWriter.h>
#include <llvm/IR/DebugInfo.h>
#endif

#if LLVM_VERSION_MAJOR < 8
#include <llvm/IR/CallSite.h>
#endif

namespace lotus {
namespace utils {
namespace llvm_compat {

//------------------------------------------------------------------------------
// Metadata kind wrappers
//------------------------------------------------------------------------------
#if defined(USE_LLVM_15) || defined(USE_LLVM_6_TO_9)
static constexpr unsigned kLocationMetadataKind = llvm::Metadata::DILocationKind;
#else
static constexpr unsigned kLocationMetadataKind = llvm::Metadata::MDLocationKind;
#endif

//------------------------------------------------------------------------------
// Debug metadata handle aliases
//------------------------------------------------------------------------------
#if defined(USE_LLVM_15) || defined(USE_LLVM_6_TO_9)
using DITypeRef = llvm::DIType*;
using DICompositeTypeRef = llvm::DICompositeType*;
using DISubprogramRef = llvm::DISubprogram*;
using DIFileRef = llvm::DIFile*;
using DIVariableRef = llvm::DIVariable*;
using DIGlobalVariableRef = llvm::DIGlobalVariable*;
using DICompileUnitRef = llvm::DICompileUnit*;
#else
using DITypeRef = llvm::DIType;
using DICompositeTypeRef = llvm::DICompositeType;
using DISubprogramRef = llvm::DISubprogram;
using DIFileRef = llvm::DIFile;
using DIVariableRef = llvm::DIVariable;
using DIGlobalVariableRef = llvm::DIGlobalVariable;
using DICompileUnitRef = llvm::DICompileUnit;
#endif

inline DITypeRef nullDIType()
{
#if defined(USE_LLVM_15) || defined(USE_LLVM_6_TO_9)
    return nullptr;
#else
    return llvm::DIType();
#endif
}

inline DICompositeTypeRef nullDICompositeType()
{
#if defined(USE_LLVM_15) || defined(USE_LLVM_6_TO_9)
    return nullptr;
#else
    return llvm::DICompositeType();
#endif
}

inline DISubprogramRef nullDISubprogram()
{
#if defined(USE_LLVM_15) || defined(USE_LLVM_6_TO_9)
    return nullptr;
#else
    return llvm::DISubprogram();
#endif
}

inline DIFileRef nullDIFile()
{
#if defined(USE_LLVM_15) || defined(USE_LLVM_6_TO_9)
    return nullptr;
#else
    return llvm::DIFile();
#endif
}

inline DIVariableRef nullDIVariable()
{
#if defined(USE_LLVM_15) || defined(USE_LLVM_6_TO_9)
    return nullptr;
#else
    return llvm::DIVariable();
#endif
}

inline DIGlobalVariableRef nullDIGlobalVariable()
{
#if defined(USE_LLVM_15) || defined(USE_LLVM_6_TO_9)
    return nullptr;
#else
    return llvm::DIGlobalVariable();
#endif
}

inline DICompileUnitRef nullDICompileUnit()
{
#if defined(USE_LLVM_15) || defined(USE_LLVM_6_TO_9)
    return nullptr;
#else
    return llvm::DICompileUnit();
#endif
}

inline llvm::Metadata* createEmptyDebugExpression(llvm::LLVMContext& ctx)
{
#if defined(USE_LLVM_15) || defined(USE_LLVM_6_TO_9)
    return llvm::DIExpression::get(ctx, {});
#else
    return llvm::MDNode::get(
            ctx,
            llvm::MDString::get(ctx, "place_holder"));
#endif
}

//------------------------------------------------------------------------------
// Debug location wrappers
//------------------------------------------------------------------------------
template<typename ScopeT, typename InlinedAtT = llvm::Metadata*>
inline llvm::MDNode* createLocation(
        llvm::LLVMContext& ctx,
        unsigned line,
        unsigned column,
        ScopeT scope,
        InlinedAtT inlinedAt = nullptr)
{
    if (!scope)
    {
        return nullptr;
    }

#if defined(USE_LLVM_6_TO_9) || defined(USE_LLVM_15)
    return llvm::DILocation::get(ctx, line, column, scope, inlinedAt);
#else
    return llvm::MDLocation::get(ctx, line, column, scope, inlinedAt);
#endif
}

inline llvm::MDNode* createLineScope(
        llvm::LLVMContext& ctx,
        llvm::StringRef fileName,
        llvm::StringRef directory)
{
#if defined(USE_LLVM_6_TO_9) || defined(USE_LLVM_15)
    auto* file = llvm::DIFile::getDistinct(ctx, fileName, directory);
    return llvm::DISubprogram::getDistinct(
            ctx,
            nullptr,
            "",
            "",
            file,
            0,
            nullptr,
            0,
            nullptr,
            0,
            0,
            llvm::DINode::FlagZero,
            llvm::DISubprogram::SPFlagZero,
            nullptr);
#else
    std::string fullPath = fileName.str();
    if (!directory.empty())
    {
        fullPath = directory.str();
        if (!fullPath.empty() && fullPath.back() != '/' && fullPath.back() != '\\')
        {
            fullPath.push_back('/');
        }
        fullPath += fileName.str();
    }
    return llvm::MDNode::get(
            ctx,
            llvm::MDString::get(ctx, fullPath));
#endif
}

inline bool isLocation(const llvm::Metadata* meta)
{
#if defined(USE_LLVM_6_TO_9) || defined(USE_LLVM_15)
    return llvm::isa<llvm::DILocation>(meta);
#else
    return llvm::isa<llvm::MDLocation>(meta);
#endif
}

inline unsigned getLine(const llvm::Metadata* meta)
{
#if defined(USE_LLVM_6_TO_9) || defined(USE_LLVM_15)
    if (auto* loc = llvm::dyn_cast<llvm::DILocation>(meta))
    {
        return loc->getLine();
    }
#else
    if (auto* loc = llvm::dyn_cast<llvm::MDLocation>(meta))
    {
        return loc->getLine();
    }
#endif
    return 0;
}

inline unsigned getColumn(const llvm::Metadata* meta)
{
#if defined(USE_LLVM_6_TO_9) || defined(USE_LLVM_15)
    if (auto* loc = llvm::dyn_cast<llvm::DILocation>(meta))
    {
        return loc->getColumn();
    }
#else
    if (auto* loc = llvm::dyn_cast<llvm::MDLocation>(meta))
    {
        return loc->getColumn();
    }
#endif
    return 0;
}

inline const llvm::Metadata* getScope(const llvm::Metadata* meta)
{
#if defined(USE_LLVM_6_TO_9) || defined(USE_LLVM_15)
    if (auto* loc = llvm::dyn_cast<llvm::DILocation>(meta))
    {
        return loc->getScope();
    }
#else
    if (auto* loc = llvm::dyn_cast<llvm::MDLocation>(meta))
    {
        return loc->getScope();
    }
#endif
    return nullptr;
}

inline llvm::Metadata* getScope(llvm::Metadata* meta)
{
    return const_cast<llvm::Metadata*>(getScope(static_cast<const llvm::Metadata*>(meta)));
}

inline const llvm::Metadata* getInlinedAt(const llvm::Metadata* meta)
{
#if defined(USE_LLVM_6_TO_9) || defined(USE_LLVM_15)
    if (auto* loc = llvm::dyn_cast<llvm::DILocation>(meta))
    {
        return loc->getInlinedAt();
    }
#else
    if (auto* loc = llvm::dyn_cast<llvm::MDLocation>(meta))
    {
        return loc->getInlinedAt();
    }
#endif
    return nullptr;
}

inline llvm::Metadata* getInlinedAt(llvm::Metadata* meta)
{
    return const_cast<llvm::Metadata*>(getInlinedAt(static_cast<const llvm::Metadata*>(meta)));
}

inline uint64_t getDbgValueOffset(const llvm::DbgValueInst* dbgValue)
{
    if (dbgValue == nullptr)
    {
        return 0;
    }

#if LOTUS_LLVM_COMPAT_GE_14 || defined(USE_LLVM_15)
    if (auto* expr = dbgValue->getExpression())
    {
        int64_t offset = 0;
        if (expr->extractIfOffset(offset) && offset >= 0)
        {
            return static_cast<uint64_t>(offset);
        }
    }
    return 0;
#else
    return dbgValue->getOffset();
#endif
}

//------------------------------------------------------------------------------
// Version flags
//------------------------------------------------------------------------------
inline bool isLlvm36()
{
    return LLVM_VERSION_MAJOR < 6;
}

inline bool isLlvm6To9()
{
    return LLVM_VERSION_MAJOR >= 6 && LLVM_VERSION_MAJOR < 10;
}

inline bool isLlvm15()
{
    return LLVM_VERSION_MAJOR >= 15;
}

inline bool preferTypedPointers(llvm::LLVMContext& context)
{
#if LOTUS_LLVM_COMPAT_GE_15 || defined(USE_LLVM_15)
    if (!context.hasSetOpaquePointersValue())
    {
        context.setOpaquePointers(false);
        return true;
    }
    return context.supportsTypedPointers();
#elif LOTUS_LLVM_COMPAT_GE_14
    return context.supportsTypedPointers();
#else
    (void)context;
    return true;
#endif
}

//------------------------------------------------------------------------------
// Filesystem API wrappers
//------------------------------------------------------------------------------
inline auto fsOpenFlagNone()
{
#if defined(USE_LLVM_15) || defined(USE_LLVM_6_TO_9)
    return llvm::sys::fs::OF_None;
#else
    return llvm::sys::fs::F_None;
#endif
}

inline std::error_code openFileForWrite(const llvm::Twine& fileName, int& fd)
{
#if defined(USE_LLVM_15) || defined(USE_LLVM_6_TO_9)
    return llvm::sys::fs::openFileForWrite(fileName, fd);
#else
    return llvm::sys::fs::openFileForWrite(fileName, fd, llvm::sys::fs::F_None);
#endif
}

//------------------------------------------------------------------------------
// DataLayout access wrapper
//------------------------------------------------------------------------------
inline const llvm::DataLayout& getDataLayout(const llvm::Module* module)
{
#if defined(USE_LLVM_6_TO_9) || defined(USE_LLVM_15)
    return module->getDataLayout();
#else
    return *module->getDataLayout();
#endif
}

inline llvm::StructType* getTypeByName(
        llvm::Module* module,
        llvm::StringRef name)
{
#if defined(USE_LLVM_15) || defined(USE_LLVM_6_TO_9)
    return llvm::StructType::getTypeByName(module->getContext(), name);
#else
    return module->getTypeByName(name);
#endif
}

inline const llvm::StructType* getTypeByName(
        const llvm::Module* module,
        llvm::StringRef name)
{
    return getTypeByName(const_cast<llvm::Module*>(module), name);
}

inline llvm::ValueName* createValueName(llvm::StringRef name)
{
#if defined(USE_LLVM_15) || defined(USE_LLVM_6_TO_9)
    llvm::MallocAllocator allocator;
    return llvm::ValueName::Create(name, allocator);
#else
    return llvm::ValueName::Create(name);
#endif
}

inline void deleteValue(llvm::Value* value)
{
    if (value == nullptr)
    {
        return;
    }
#if defined(USE_LLVM_15) || defined(USE_LLVM_6_TO_9)
    value->deleteValue();
#else
    delete value;
#endif
}

//------------------------------------------------------------------------------
// Vector type wrappers
//------------------------------------------------------------------------------
#if defined(USE_LLVM_15) || defined(USE_LLVM_6_TO_9)
static constexpr llvm::Type::TypeID kVectorTypeID = llvm::Type::FixedVectorTyID;
#else
static constexpr llvm::Type::TypeID kVectorTypeID = llvm::Type::VectorTyID;
#endif

inline unsigned getVectorNumElements(const llvm::VectorType* vectorTy)
{
#if defined(USE_LLVM_15) || defined(USE_LLVM_6_TO_9)
    return llvm::cast<llvm::FixedVectorType>(vectorTy)->getNumElements();
#else
    return vectorTy->getNumElements();
#endif
}

inline unsigned getVectorNumElements(const llvm::Type* type)
{
    return getVectorNumElements(llvm::cast<llvm::VectorType>(type));
}

inline llvm::VectorType* getVectorType(
        llvm::Type* elementType,
        unsigned numElements)
{
#if defined(USE_LLVM_15) || defined(USE_LLVM_6_TO_9)
    return llvm::VectorType::get(elementType, numElements, false);
#else
    return llvm::VectorType::get(elementType, numElements);
#endif
}

inline llvm::VectorType* getVectorType(
        llvm::Type* elementType,
        const llvm::VectorType* likeVectorType)
{
#if defined(USE_LLVM_15) || defined(USE_LLVM_6_TO_9)
    return llvm::VectorType::get(elementType, likeVectorType);
#else
    return llvm::VectorType::get(elementType, likeVectorType->getNumElements());
#endif
}

inline unsigned getConstantAggregateZeroNumElements(
        const llvm::ConstantAggregateZero* aggregateZero)
{
    auto* ty = aggregateZero->getType();
    if (auto* arrayTy = llvm::dyn_cast<llvm::ArrayType>(ty))
    {
        return arrayTy->getNumElements();
    }
    if (auto* structTy = llvm::dyn_cast<llvm::StructType>(ty))
    {
        return structTy->getNumElements();
    }
    if (auto* vectorTy = llvm::dyn_cast<llvm::VectorType>(ty))
    {
        return getVectorNumElements(vectorTy);
    }

    llvm_unreachable("ConstantAggregateZero must have aggregate type");
}

//------------------------------------------------------------------------------
// Pointer element type wrappers
//------------------------------------------------------------------------------
inline llvm::Type* getPointerElementType(llvm::Type* ty)
{
#if defined(USE_LLVM_15) || defined(USE_LLVM_6_TO_9)
    return llvm::cast<llvm::PointerType>(ty)->getNonOpaquePointerElementType();
#else
    return llvm::cast<llvm::PointerType>(ty)->getPointerElementType();
#endif
}

inline const llvm::Type* getPointerElementType(const llvm::Type* ty)
{
#if defined(USE_LLVM_15) || defined(USE_LLVM_6_TO_9)
    return llvm::cast<llvm::PointerType>(ty)->getNonOpaquePointerElementType();
#else
    return llvm::cast<llvm::PointerType>(ty)->getPointerElementType();
#endif
}

inline const llvm::Type* getPointerElementType(const llvm::Value* value)
{
    return getPointerElementType(value->getType());
}

inline llvm::Type* getPointerElementType(llvm::Value* value)
{
    return const_cast<llvm::Type*>(getPointerElementType(value->getType()));
}

// https://github.com/llvm/llvm-project/commit/5548e807b5777fdda167b6795e0e05432a6163f1
inline llvm::Constant* getExtractValue(
        llvm::Constant* aggregate,
        llvm::ArrayRef<unsigned> idxs)
{
#if defined(USE_LLVM_15) || defined(USE_LLVM_6_TO_9)
    // LLVM 15 removed ConstantExpr::getExtractValue; keep old behavior via
    // constant folding on aggregate constants.
    return llvm::ConstantFoldExtractValueInstruction(aggregate, idxs);
#else
    return llvm::ConstantExpr::getExtractValue(aggregate, idxs);
#endif
}

inline llvm::Constant* getInsertValue(
        llvm::Constant* aggregate,
        llvm::Constant* value,
        llvm::ArrayRef<unsigned> idxs)
{
#if defined(USE_LLVM_15) || defined(USE_LLVM_6_TO_9)
    // LLVM 15 removed ConstantExpr::getInsertValue; use the folder entry point.
    return llvm::ConstantFoldInsertValueInstruction(aggregate, value, idxs);
#else
    return llvm::ConstantExpr::getInsertValue(aggregate, value, idxs);
#endif
}

inline llvm::Constant* getGetElementPtr(
        llvm::Constant* base,
        llvm::ArrayRef<llvm::Value*> idxs,
        bool inBounds = false)
{
#if defined(USE_LLVM_15) || defined(USE_LLVM_6_TO_9)
    return llvm::ConstantExpr::getGetElementPtr(
            getPointerElementType(base),
            base,
            idxs,
            inBounds);
#else
    return getGetElementPtr(base, idxs, inBounds);
#endif
}

inline void replaceUsesOfWithOnConstant(
        llvm::Constant* constant,
        llvm::Value* from,
        llvm::Value* to,
        llvm::Use* use = nullptr)
{
#if LOTUS_LLVM_COMPAT_GE_14 || defined(USE_LLVM_15)
    (void)use;
    constant->handleOperandChange(from, to);
#else
    constant->replaceUsesOfWithOnConstant(from, to, use);
#endif
}

inline llvm::Constant* getGetElementPtr(
        llvm::Constant* base,
        llvm::ArrayRef<llvm::Constant*> idxs,
        bool inBounds = false)
{
#if defined(USE_LLVM_15) || defined(USE_LLVM_6_TO_9)
    return llvm::ConstantExpr::getGetElementPtr(
            getPointerElementType(base),
            base,
            idxs,
            inBounds);
#else
    return llvm::ConstantExpr::getGetElementPtr(base, idxs);
#endif
}

inline llvm::Constant* getShuffleVector(
        llvm::Constant* v1,
        llvm::Constant* v2,
        llvm::Constant* mask)
{
#if defined(USE_LLVM_15) || defined(USE_LLVM_6_TO_9)
    llvm::SmallVector<int, 16> IntMask;
    llvm::ShuffleVectorInst::getShuffleMask(mask, IntMask);
    return llvm::ConstantExpr::getShuffleVector(v1, v2, IntMask);
#else
    return llvm::ConstantExpr::getShuffleVector(v1, v2, mask);
#endif
}

//------------------------------------------------------------------------------
// Function attribute wrappers
//------------------------------------------------------------------------------
#if defined(USE_LLVM_15) || defined(USE_LLVM_6_TO_9)
using FunctionAttrList = llvm::AttributeList;
#else
using FunctionAttrList = llvm::AttributeSet;
#endif

inline unsigned returnAttributeIndex()
{
#if defined(USE_LLVM_15) || defined(USE_LLVM_6_TO_9)
    return llvm::AttributeList::ReturnIndex;
#else
    return llvm::AttributeSet::ReturnIndex;
#endif
}

inline unsigned functionAttributeIndex()
{
#if defined(USE_LLVM_15) || defined(USE_LLVM_6_TO_9)
    return llvm::AttributeList::FunctionIndex;
#else
    return llvm::AttributeSet::FunctionIndex;
#endif
}

inline FunctionAttrList getFunctionAttributes(const llvm::Function* function)
{
    return function->getAttributes();
}

inline void setFunctionAttributes(
        llvm::Function* function,
        const FunctionAttrList& attrs)
{
    function->setAttributes(attrs);
}

inline void addFunctionAttribute(
        llvm::Function* function,
        llvm::Attribute::AttrKind attr)
{
#if defined(USE_LLVM_15) || defined(USE_LLVM_6_TO_9)
    function->addFnAttr(attr);
#else
    function->addAttribute(llvm::AttributeSet::FunctionIndex, attr);
#endif
}

inline void removeFunctionAttribute(
        llvm::Function* function,
        llvm::Attribute::AttrKind attr)
{
#if defined(USE_LLVM_15) || defined(USE_LLVM_6_TO_9)
    function->removeFnAttr(attr);
#else
    llvm::AttributeSet attrs = function->getAttributes();
    attrs = attrs.removeAttribute(function->getContext(), llvm::AttributeSet::FunctionIndex, attr);
    function->setAttributes(attrs);
#endif
}

inline llvm::AttributeSet getParamAttributes(
        const FunctionAttrList& attrs,
        unsigned argNo)
{
#if LOTUS_LLVM_COMPAT_GE_14 || defined(USE_LLVM_15)
    return attrs.getParamAttrs(argNo);
#elif defined(USE_LLVM_6_TO_9)
    return attrs.getParamAttributes(argNo);
#else
    return attrs.getParamAttributes(argNo + 1);
#endif
}

inline llvm::AttributeSet getReturnAttributes(const FunctionAttrList& attrs)
{
#if LOTUS_LLVM_COMPAT_GE_14 || defined(USE_LLVM_15)
    return attrs.getRetAttrs();
#elif defined(USE_LLVM_6_TO_9)
    return attrs.getRetAttributes();
#else
    return attrs.getRetAttributes();
#endif
}

inline llvm::AttributeSet getFnAttributes(const FunctionAttrList& attrs)
{
#if LOTUS_LLVM_COMPAT_GE_14 || defined(USE_LLVM_15)
    return attrs.getFnAttrs();
#elif defined(USE_LLVM_6_TO_9)
    return attrs.getFnAttributes();
#else
    return attrs.getFnAttributes();
#endif
}

inline bool hasAttributes(const llvm::AttributeSet& attrs)
{
#if defined(USE_LLVM_15) || defined(USE_LLVM_6_TO_9)
    return attrs.hasAttributes();
#else
    return !attrs.isEmpty();
#endif
}

inline FunctionAttrList addAttributesAtIndex(
        const FunctionAttrList& attrs,
        llvm::LLVMContext& ctx,
        unsigned index,
        llvm::AttributeSet toAdd)
{
#if LOTUS_LLVM_COMPAT_GE_14 || defined(USE_LLVM_15)
    llvm::AttrBuilder builder(ctx, toAdd);
    return attrs.addAttributesAtIndex(ctx, index, builder);
#elif defined(USE_LLVM_6_TO_9)
    llvm::AttrBuilder builder(toAdd);
    return attrs.addAttributes(ctx, index, builder);
#else
    return attrs.addAttributes(ctx, index, toAdd);
#endif
}

inline void addArgumentAttributes(
        llvm::Argument* arg,
        llvm::AttributeSet attrs)
{
#if LOTUS_LLVM_COMPAT_GE_14 || defined(USE_LLVM_15)
    llvm::AttrBuilder builder(arg->getContext(), attrs);
    arg->addAttrs(builder);
#elif defined(USE_LLVM_6_TO_9)
    llvm::AttrBuilder builder(attrs);
    arg->addAttrs(builder);
#else
    arg->addAttr(attrs);
#endif
}

//------------------------------------------------------------------------------
// Call-like instruction wrappers (CallSite/CallBase)
//------------------------------------------------------------------------------
#if LLVM_VERSION_MAJOR >= 8 || defined(USE_LLVM_15)
using CallSiteCompat = llvm::CallBase*;
#else
using CallSiteCompat = llvm::CallSite;
#endif

inline CallSiteCompat nullCallSite()
{
#if LLVM_VERSION_MAJOR >= 8 || defined(USE_LLVM_15)
    return nullptr;
#else
    return llvm::CallSite();
#endif
}

inline CallSiteCompat makeCallSite(llvm::Instruction* inst)
{
#if LLVM_VERSION_MAJOR >= 8 || defined(USE_LLVM_15)
    return llvm::dyn_cast<llvm::CallBase>(inst);
#else
    return llvm::CallSite(inst);
#endif
}

inline bool isValidCallSite(CallSiteCompat cs)
{
#if LLVM_VERSION_MAJOR >= 8 || defined(USE_LLVM_15)
    return cs != nullptr;
#else
    return cs.getInstruction() != nullptr;
#endif
}

inline llvm::Instruction* getCallSiteInstruction(CallSiteCompat cs)
{
#if LLVM_VERSION_MAJOR >= 8 || defined(USE_LLVM_15)
    return cs;
#else
    return cs.getInstruction();
#endif
}

inline llvm::Type* getCallSiteType(CallSiteCompat cs)
{
#if LLVM_VERSION_MAJOR >= 8 || defined(USE_LLVM_15)
    return cs ? cs->getType() : nullptr;
#else
    return cs.getInstruction() ? cs.getType() : nullptr;
#endif
}

inline unsigned getCallSiteArgSize(CallSiteCompat cs)
{
#if LLVM_VERSION_MAJOR >= 8 || defined(USE_LLVM_15)
    return cs ? cs->arg_size() : 0;
#else
    return cs.getInstruction() ? cs.arg_size() : 0;
#endif
}

inline llvm::Value* getCallSiteArgOperand(CallSiteCompat cs, unsigned idx)
{
#if LLVM_VERSION_MAJOR >= 8 || defined(USE_LLVM_15)
    return cs ? cs->getArgOperand(idx) : nullptr;
#else
    return cs.getInstruction() ? cs.getArgument(idx) : nullptr;
#endif
}

inline llvm::Function* getCallSiteCalledFunction(CallSiteCompat cs)
{
#if LLVM_VERSION_MAJOR >= 8 || defined(USE_LLVM_15)
    return cs ? cs->getCalledFunction() : nullptr;
#else
    return cs.getCalledFunction();
#endif
}

inline auto getCallSiteArgBegin(CallSiteCompat cs)
{
#if LLVM_VERSION_MAJOR >= 8 || defined(USE_LLVM_15)
    return cs->arg_begin();
#else
    return cs.arg_begin();
#endif
}

inline auto getCallSiteArgEnd(CallSiteCompat cs)
{
#if LLVM_VERSION_MAJOR >= 8 || defined(USE_LLVM_15)
    return cs->arg_end();
#else
    return cs.arg_end();
#endif
}

//------------------------------------------------------------------------------
// IR and instruction creation wrappers
// IRBuilder wrappers
//------------------------------------------------------------------------------
template<typename BuilderT>
inline llvm::LoadInst* createLoad(
        BuilderT& irb,
        llvm::Value* ptr,
        const llvm::Twine& name = "")
{
#if LOTUS_LLVM_COMPAT_GE_14 || defined(USE_LLVM_15)
    return irb.CreateLoad(getPointerElementType(ptr), ptr, name);
#else
    return irb.CreateLoad(ptr, name);
#endif
}

template<typename BuilderT>
inline llvm::LoadInst* createLoad(
        BuilderT& irb,
        llvm::Type* type,
        llvm::Value* ptr,
        const llvm::Twine& name = "")
{
#if LOTUS_LLVM_COMPAT_GE_14 || defined(USE_LLVM_15)
    return irb.CreateLoad(type, ptr, name);
#else
    (void)type;
    return irb.CreateLoad(ptr, name);
#endif
}

inline llvm::LoadInst* newLoadInst(
        llvm::Value* ptr,
        const llvm::Twine& name = "",
        llvm::Instruction* insertBefore = nullptr)
{
#if LOTUS_LLVM_COMPAT_GE_14 || defined(USE_LLVM_15)
    if (insertBefore == nullptr)
    {
        return new llvm::LoadInst(
                getPointerElementType(ptr),
                ptr,
                name,
                false,
                llvm::Align(1),
                insertBefore);
    }
    return new llvm::LoadInst(
            getPointerElementType(ptr),
            ptr,
            name,
            insertBefore);
#else
    return new llvm::LoadInst(ptr, name, insertBefore);
#endif
}

inline llvm::AllocaInst* newAllocaInst(
        llvm::Type* allocatedType,
        const llvm::Twine& name = "",
        llvm::Instruction* insertBefore = nullptr,
        unsigned addrSpace = 0)
{
#if LOTUS_LLVM_COMPAT_GE_14 || defined(USE_LLVM_15)
    return new llvm::AllocaInst(allocatedType, addrSpace, name, insertBefore);
#else
    (void)addrSpace;
    return new llvm::AllocaInst(allocatedType, name, insertBefore);
#endif
}

inline unsigned getAlignment(const llvm::AllocaInst* allocaInst)
{
#if LOTUS_LLVM_COMPAT_GE_14 || defined(USE_LLVM_15)
    return allocaInst->getAlign().value();
#else
    return allocaInst->getAlignment();
#endif
}

inline void setAlignment(llvm::AllocaInst* allocaInst, unsigned alignment)
{
#if LOTUS_LLVM_COMPAT_GE_14 || defined(USE_LLVM_15)
    if (alignment == 0)
    {
        return;
    }
    allocaInst->setAlignment(llvm::Align(alignment));
#else
    allocaInst->setAlignment(alignment);
#endif
}

#if defined(USE_LLVM_15) || defined(USE_LLVM_6_TO_9)
using AtomicSyncScope = llvm::SyncScope::ID;
#else
using AtomicSyncScope = llvm::SynchronizationScope;
#endif

inline AtomicSyncScope getSyncScopeID(const llvm::FenceInst* fenceInst)
{
#if defined(USE_LLVM_15) || defined(USE_LLVM_6_TO_9)
    return fenceInst->getSyncScopeID();
#else
    return fenceInst->getSynchScope();
#endif
}

inline AtomicSyncScope getSyncScopeID(const llvm::AtomicCmpXchgInst* atomicCmpXchgInst)
{
#if defined(USE_LLVM_15) || defined(USE_LLVM_6_TO_9)
    return atomicCmpXchgInst->getSyncScopeID();
#else
    return atomicCmpXchgInst->getSynchScope();
#endif
}

inline unsigned getAlignment(const llvm::AtomicRMWInst* atomicRMWInst)
{
#if LOTUS_LLVM_COMPAT_GE_14 || defined(USE_LLVM_15)
    return atomicRMWInst->getAlign().value();
#else
    (void)atomicRMWInst;
    return 0;
#endif
}

inline AtomicSyncScope getSyncScopeID(const llvm::AtomicRMWInst* atomicRMWInst)
{
#if defined(USE_LLVM_15) || defined(USE_LLVM_6_TO_9)
    return atomicRMWInst->getSyncScopeID();
#else
    return atomicRMWInst->getSynchScope();
#endif
}

inline unsigned getAlignment(const llvm::AtomicCmpXchgInst* atomicCmpXchgInst)
{
#if LOTUS_LLVM_COMPAT_GE_14 || defined(USE_LLVM_15)
    return atomicCmpXchgInst->getAlign().value();
#else
    (void)atomicCmpXchgInst;
    return 0;
#endif
}

inline llvm::FenceInst* newFenceInst(
        llvm::LLVMContext& context,
        llvm::AtomicOrdering ordering,
        AtomicSyncScope syncScope,
        llvm::Instruction* insertBefore = nullptr)
{
    return new llvm::FenceInst(context, ordering, syncScope, insertBefore);
}

inline llvm::AtomicCmpXchgInst* newAtomicCmpXchgInst(
        llvm::Value* ptr,
        llvm::Value* cmp,
        llvm::Value* newVal,
        llvm::AtomicOrdering successOrdering,
        llvm::AtomicOrdering failureOrdering,
        AtomicSyncScope syncScope,
        llvm::Instruction* insertBefore = nullptr,
        unsigned alignment = 1)
{
#if LOTUS_LLVM_COMPAT_GE_14 || defined(USE_LLVM_15)
    if (alignment == 0)
    {
        alignment = 1;
    }
    return new llvm::AtomicCmpXchgInst(
            ptr,
            cmp,
            newVal,
            llvm::Align(alignment),
            successOrdering,
            failureOrdering,
            syncScope,
            insertBefore);
#else
    (void)alignment;
    return new llvm::AtomicCmpXchgInst(
            ptr,
            cmp,
            newVal,
            successOrdering,
            failureOrdering,
            syncScope,
            insertBefore);
#endif
}

inline llvm::StoreInst* newStoreInst(
        llvm::Value* value,
        llvm::Value* ptr,
        llvm::Instruction* insertBefore = nullptr)
{
#if LOTUS_LLVM_COMPAT_GE_14 || defined(USE_LLVM_15)
    if (insertBefore == nullptr)
    {
        return new llvm::StoreInst(
                value,
                ptr,
                false,
                llvm::Align(1),
                insertBefore);
    }
#endif
    return new llvm::StoreInst(value, ptr, insertBefore);
}

inline llvm::StoreInst* newStoreInst(
        llvm::Value* value,
        llvm::Value* ptr,
        bool isVolatile,
        llvm::Instruction* insertBefore = nullptr)
{
#if LOTUS_LLVM_COMPAT_GE_14 || defined(USE_LLVM_15)
    if (insertBefore == nullptr)
    {
        return new llvm::StoreInst(
                value,
                ptr,
                isVolatile,
                llvm::Align(1),
                insertBefore);
    }
#endif
    return new llvm::StoreInst(value, ptr, isVolatile, insertBefore);
}

template<typename CallLikeT>
inline unsigned getNumArgOperands(const CallLikeT* call)
{
#if defined(USE_LLVM_15) || defined(USE_LLVM_6_TO_9)
    return call->arg_size();
#else
    return call->getNumArgOperands();
#endif
}

inline llvm::Value* getCalledValue(llvm::CallInst* call)
{
#if defined(USE_LLVM_15) || defined(USE_LLVM_6_TO_9)
    return call->getCalledOperand();
#else
    return call->getCalledValue();
#endif
}

inline const llvm::Value* getCalledValue(const llvm::CallInst* call)
{
#if defined(USE_LLVM_15) || defined(USE_LLVM_6_TO_9)
    return call->getCalledOperand();
#else
    return call->getCalledValue();
#endif
}

inline llvm::Value* getCalledValue(llvm::InvokeInst* invoke)
{
#if defined(USE_LLVM_15) || defined(USE_LLVM_6_TO_9)
    return invoke->getCalledOperand();
#else
    return invoke->getCalledValue();
#endif
}

inline const llvm::Value* getCalledValue(const llvm::InvokeInst* invoke)
{
#if defined(USE_LLVM_15) || defined(USE_LLVM_6_TO_9)
    return invoke->getCalledOperand();
#else
    return invoke->getCalledValue();
#endif
}

template<typename CalleeT>
inline llvm::CallInst* newCallInst(
        CalleeT callee,
        const llvm::Twine& name = "",
        llvm::Instruction* insertBefore = nullptr)
{
#if defined(USE_LLVM_15) || defined(USE_LLVM_6_TO_9)
    if (auto* fn = llvm::dyn_cast<llvm::Function>(callee))
    {
        return llvm::CallInst::Create(fn->getFunctionType(), fn, name, insertBefore);
    }
    auto* fnTy = llvm::cast<llvm::FunctionType>(getPointerElementType(callee));
    return llvm::CallInst::Create(fnTy, callee, name, insertBefore);
#else
    return llvm::CallInst::Create(callee, name, insertBefore);
#endif
}

template<typename CalleeT>
inline llvm::CallInst* newCallInst(
        CalleeT callee,
        llvm::ArrayRef<llvm::Value*> args,
        const llvm::Twine& name = "",
        llvm::Instruction* insertBefore = nullptr)
{
#if defined(USE_LLVM_15) || defined(USE_LLVM_6_TO_9)
    if (auto* fn = llvm::dyn_cast<llvm::Function>(callee))
    {
        return llvm::CallInst::Create(fn->getFunctionType(), fn, args, name, insertBefore);
    }
    auto* fnTy = llvm::cast<llvm::FunctionType>(getPointerElementType(callee));
    return llvm::CallInst::Create(fnTy, callee, args, name, insertBefore);
#else
    return llvm::CallInst::Create(callee, args, name, insertBefore);
#endif
}

template<typename CalleeT>
inline llvm::InvokeInst* newInvokeInst(
        CalleeT callee,
        llvm::BasicBlock* normalDest,
        llvm::BasicBlock* unwindDest,
        llvm::ArrayRef<llvm::Value*> args,
        const llvm::Twine& name = "",
        llvm::Instruction* insertBefore = nullptr)
{
#if defined(USE_LLVM_15) || defined(USE_LLVM_6_TO_9)
    if (auto* fn = llvm::dyn_cast<llvm::Function>(callee))
    {
        return llvm::InvokeInst::Create(
                fn->getFunctionType(),
                fn,
                normalDest,
                unwindDest,
                args,
                name,
                insertBefore);
    }
    auto* fnTy = llvm::cast<llvm::FunctionType>(getPointerElementType(callee));
    return llvm::InvokeInst::Create(
            fnTy,
            callee,
            normalDest,
            unwindDest,
            args,
            name,
            insertBefore);
#else
    return llvm::InvokeInst::Create(
            callee,
            normalDest,
            unwindDest,
            args,
            name,
            insertBefore);
#endif
}

//------------------------------------------------------------------------------
// Memory intrinsic wrappers
//------------------------------------------------------------------------------
inline llvm::CallInst* createMemCpy(
        llvm::IRBuilder<>& irb,
        llvm::Value* dst,
        llvm::Value* src,
        llvm::Value* size,
        unsigned alignment = 1,
        bool isVolatile = false)
{
#if LOTUS_LLVM_COMPAT_GE_14 || defined(USE_LLVM_15)
    return irb.CreateMemCpy(
            dst,
            llvm::MaybeAlign(alignment),
            src,
            llvm::MaybeAlign(alignment),
            size,
            isVolatile);
#else
    llvm::Type* opTypes[] = {dst->getType(), src->getType(), size->getType()};
    auto* fnc = llvm::Intrinsic::getDeclaration(
            irb.GetInsertBlock()->getParent()->getParent(),
            llvm::Intrinsic::memcpy,
            opTypes);
    llvm::Value* ops[] = {
            dst,
            src,
            size,
            irb.getInt32(alignment),
            irb.getInt1(isVolatile)};
    return irb.CreateCall(fnc, llvm::ArrayRef<llvm::Value*>(ops));
#endif
}

inline llvm::BasicBlock* findNearestCommonDominator(
        llvm::DominatorTree* DT,
        llvm::BasicBlock* A,
        llvm::BasicBlock* B)
{
    if (!DT || !A || !B || A->getParent() != B->getParent())
        return nullptr;

#if defined(USE_LLVM_15)
    if (A == B)
        return A;

    auto* nodeA = DT->getNode(A);
    auto* nodeB = DT->getNode(B);
    if (!nodeA || !nodeB)
        return nullptr;

    while (nodeA != nodeB)
    {
        if (nodeA->getLevel() < nodeB->getLevel())
            std::swap(nodeA, nodeB);

        nodeA = nodeA->getIDom();
        if (!nodeA)
            return nullptr;
    }

    return nodeA->getBlock();
#else
    return DT->findNearestCommonDominator(A, B);
#endif
}

template<typename ScopeT>
inline llvm::AtomicRMWInst* newAtomicRMWInst(
        llvm::AtomicRMWInst::BinOp operation,
        llvm::Value* ptr,
        llvm::Value* val,
        llvm::AtomicOrdering ordering,
        ScopeT synchScope,
        llvm::Instruction* insertBefore = nullptr,
        unsigned alignment = 1)
{
#if LOTUS_LLVM_COMPAT_GE_14 || defined(USE_LLVM_15)
    if (alignment == 0)
    {
        alignment = 1;
    }
    return new llvm::AtomicRMWInst(
            operation,
            ptr,
            val,
            llvm::Align(alignment),
            ordering,
            synchScope,
            insertBefore);
#else
    (void)alignment;
    return new llvm::AtomicRMWInst(
            operation,
            ptr,
            val,
            ordering,
            synchScope,
            insertBefore);
#endif
}

inline llvm::LandingPadInst* createLandingPadInst(
        llvm::Type* retType,
        llvm::Value* personalityFn,
        unsigned numReservedClauses = 0,
        const llvm::Twine& name = "",
        llvm::Instruction* insertBefore = nullptr)
{
#if defined(USE_LLVM_15) || defined(USE_LLVM_6_TO_9)
    (void)personalityFn;
    return llvm::LandingPadInst::Create(
            retType,
            numReservedClauses,
            name,
            insertBefore);
#else
    return llvm::LandingPadInst::Create(
            retType,
            personalityFn,
            numReservedClauses,
            name,
            insertBefore);
#endif
}

inline llvm::GetElementPtrInst* createGetElementPtrInst(
        llvm::Value* ptr,
        llvm::ArrayRef<llvm::Value*> idxList,
        const llvm::Twine& name = "",
        llvm::Instruction* insertBefore = nullptr)
{
#if defined(USE_LLVM_15) || defined(USE_LLVM_6_TO_9)
    return llvm::GetElementPtrInst::Create(
            getPointerElementType(ptr),
            ptr,
            idxList,
            name,
            insertBefore);
#else
    return llvm::GetElementPtrInst::Create(
            ptr,
            idxList,
            name,
            insertBefore);
#endif
}

template<typename BuilderT>
inline llvm::Value* createShuffleVector(
        BuilderT& irb,
        llvm::Value* op0,
        llvm::Value* op1,
        llvm::ArrayRef<uint32_t> intMask)
{
#if defined(USE_LLVM_15) || defined(USE_LLVM_6_TO_9)
    llvm::SmallVector<int, 16> mask;
    mask.reserve(intMask.size());
    for (uint32_t v : intMask)
    {
        mask.push_back(static_cast<int>(v));
    }
    return irb.CreateShuffleVector(op0, op1, mask);
#else
    return irb.CreateShuffleVector(
    op0,
    op1,
    llvm::ConstantDataVector::get(op0->getContext(), intMask));
#endif
}

//------------------------------------------------------------------------------
// DIBuilder wrappers
//------------------------------------------------------------------------------
inline auto createBasicType(
        llvm::DIBuilder& builder,
        llvm::StringRef name,
        uint64_t sizeInBits,
        uint64_t alignInBits,
        unsigned encoding)
{
#if defined(USE_LLVM_15) || defined(USE_LLVM_6_TO_9)
    (void)alignInBits;
    return builder.createBasicType(name, sizeInBits, encoding);
#else
    return builder.createBasicType(name, sizeInBits, alignInBits, encoding);
#endif
}

template<typename ElementsT>
inline DICompositeTypeRef createStructType(
        llvm::DIBuilder& builder,
        DIFileRef scope,
        llvm::StringRef name,
        DIFileRef file,
        unsigned lineNumber,
        uint64_t sizeInBits,
        uint32_t alignInBits,
        unsigned flags,
        DITypeRef derivedFrom,
        ElementsT elements,
        unsigned runTimeLang = 0,
        DITypeRef vTableHolder = nullDIType(),
        llvm::StringRef uniqueIdentifier = "")
{
#if defined(USE_LLVM_15) || defined(USE_LLVM_6_TO_9)
    return builder.createStructType(
            scope,
            name,
            file,
            lineNumber,
            sizeInBits,
            alignInBits,
            static_cast<llvm::DINode::DIFlags>(flags),
            derivedFrom,
            elements,
            runTimeLang,
            vTableHolder,
            uniqueIdentifier);
#else
    return builder.createStructType(
            scope,
            name,
            file,
            lineNumber,
            sizeInBits,
            alignInBits,
            flags,
            derivedFrom,
            elements,
            runTimeLang,
            vTableHolder,
            uniqueIdentifier);
#endif
}

template<typename ElementsT>
inline DICompositeTypeRef createClassType(
        llvm::DIBuilder& builder,
        DIFileRef scope,
        llvm::StringRef name,
        DIFileRef file,
        unsigned lineNumber,
        uint64_t sizeInBits,
        uint32_t alignInBits,
        uint64_t offsetInBits,
        unsigned flags,
        DITypeRef derivedFrom,
        ElementsT elements,
        DITypeRef vTableHolder = nullDIType(),
        llvm::MDNode* templateParams = nullptr,
        llvm::StringRef uniqueIdentifier = "")
{
#if defined(USE_LLVM_15) || defined(USE_LLVM_6_TO_9)
    return builder.createClassType(
            scope,
            name,
            file,
            lineNumber,
            sizeInBits,
            alignInBits,
            offsetInBits,
            static_cast<llvm::DINode::DIFlags>(flags),
            derivedFrom,
            elements,
            vTableHolder,
            templateParams,
            uniqueIdentifier);
#else
    return builder.createClassType(
            scope,
            name,
            file,
            lineNumber,
            sizeInBits,
            alignInBits,
            offsetInBits,
            flags,
            derivedFrom,
            elements,
            vTableHolder,
            templateParams,
            uniqueIdentifier);
#endif
}

template<typename ElementsT>
inline DICompositeTypeRef createUnionType(
        llvm::DIBuilder& builder,
        DIFileRef scope,
        llvm::StringRef name,
        DIFileRef file,
        unsigned lineNumber,
        uint64_t sizeInBits,
        uint32_t alignInBits,
        unsigned flags,
        ElementsT elements,
        unsigned runTimeLang = 0,
        llvm::StringRef uniqueIdentifier = "")
{
#if defined(USE_LLVM_15) || defined(USE_LLVM_6_TO_9)
    return builder.createUnionType(
            scope,
            name,
            file,
            lineNumber,
            sizeInBits,
            alignInBits,
            static_cast<llvm::DINode::DIFlags>(flags),
            elements,
            runTimeLang,
            uniqueIdentifier);
#else
    return builder.createUnionType(
            scope,
            name,
            file,
            lineNumber,
            sizeInBits,
            alignInBits,
            flags,
            elements,
            runTimeLang,
            uniqueIdentifier);
#endif
}

inline auto createInheritance(
        llvm::DIBuilder& builder,
        DITypeRef type,
        DITypeRef baseType,
        uint64_t baseOffset,
        uint32_t vbPtrOffset,
        unsigned flags = 0)
{
#if defined(USE_LLVM_15) || defined(USE_LLVM_6_TO_9)
    return builder.createInheritance(
            type,
            baseType,
            baseOffset,
            vbPtrOffset,
            static_cast<llvm::DINode::DIFlags>(flags));
#else
    (void)flags;
    return builder.createInheritance(
            type,
            baseType,
            baseOffset,
            vbPtrOffset);
#endif
}

template<typename ScopeT, typename TypeT>
inline auto createMemberType(
        llvm::DIBuilder& builder,
        ScopeT scope,
        llvm::StringRef name,
        DIFileRef file,
        unsigned lineNo,
        uint64_t sizeInBits,
        uint32_t alignInBits,
        uint64_t offsetInBits,
        unsigned flags,
        TypeT type)
{
#if defined(USE_LLVM_15) || defined(USE_LLVM_6_TO_9)
    return builder.createMemberType(
            scope,
            name,
            file,
            lineNo,
            sizeInBits,
            alignInBits,
            offsetInBits,
            static_cast<llvm::DINode::DIFlags>(flags),
            type);
#else
    return builder.createMemberType(
            scope,
            name,
            file,
            lineNo,
            sizeInBits,
            alignInBits,
            offsetInBits,
            flags,
            type);
#endif
}

template<typename ScopeT>
inline DIVariableRef createLocalVariable(
        llvm::DIBuilder& builder,
        ScopeT scope,
        llvm::StringRef name,
        DIFileRef file,
        unsigned lineNo,
        DITypeRef type,
        bool alwaysPreserve = false,
        unsigned flags = 0,
        unsigned argNo = 0)
{
#if defined(USE_LLVM_15) || defined(USE_LLVM_6_TO_9)
    (void)argNo;
    const auto diFlags = static_cast<llvm::DINode::DIFlags>(flags);
    return builder.createAutoVariable(
            scope,
            name,
            file,
            lineNo,
            type,
            alwaysPreserve,
            diFlags);
#else
    return builder.createLocalVariable(
            llvm::dwarf::LLVMConstants::DW_TAG_auto_variable,
            scope,
            name,
            file,
            lineNo,
            type,
            alwaysPreserve,
            flags,
            argNo);
#endif
}

template<typename ScopeT>
inline DIVariableRef createParameterVariable(
        llvm::DIBuilder& builder,
        ScopeT scope,
        llvm::StringRef name,
        DIFileRef file,
        unsigned lineNo,
        DITypeRef type,
        bool alwaysPreserve = false,
        unsigned flags = 0,
        unsigned argNo = 0)
{
#if defined(USE_LLVM_15) || defined(USE_LLVM_6_TO_9)
    const auto diFlags = static_cast<llvm::DINode::DIFlags>(flags);
    return builder.createParameterVariable(
            scope,
            name,
            argNo,
            file,
            lineNo,
            type,
            alwaysPreserve,
            diFlags);
#else
    return builder.createLocalVariable(
            llvm::dwarf::LLVMConstants::DW_TAG_arg_variable,
            scope,
            name,
            file,
            lineNo,
            type,
            alwaysPreserve,
            flags,
            argNo);
#endif
}

inline DIGlobalVariableRef createGlobalVariable(
        llvm::DIBuilder& builder,
        DICompileUnitRef context,
        llvm::StringRef name,
        llvm::StringRef linkageName,
        DIFileRef file,
        unsigned lineNo,
        DITypeRef type,
        bool isLocalToUnit,
        llvm::Constant* var)
{
#if defined(USE_LLVM_15) || defined(USE_LLVM_6_TO_9)
    (void)var;
    auto* globalExpr = builder.createGlobalVariableExpression(
            context,
            name,
            linkageName,
            file,
            lineNo,
            type,
            isLocalToUnit,
            true);
    return globalExpr ? globalExpr->getVariable() : nullDIGlobalVariable();
#else
    return builder.createGlobalVariable(
            context,
            name,
            linkageName,
            file,
            lineNo,
            type,
            isLocalToUnit,
            var);
#endif
}

inline DICompileUnitRef createCompileUnit(
        llvm::DIBuilder& builder,
        unsigned lang,
        llvm::StringRef fileName,
        llvm::StringRef dir,
        llvm::StringRef producer,
        bool isOptimized,
        llvm::StringRef flags,
        unsigned runtimeVersion)
{
#if defined(USE_LLVM_15) || defined(USE_LLVM_6_TO_9)
    auto* file = builder.createFile(fileName, dir);
    return builder.createCompileUnit(
            lang,
            file,
            producer,
            isOptimized,
            flags,
            runtimeVersion);
#else
    return builder.createCompileUnit(
            lang,
            fileName,
            dir,
            producer,
            isOptimized,
            flags,
            runtimeVersion);
#endif
}

template<typename ParameterTypesT>
inline auto createSubroutineType(
        llvm::DIBuilder& builder,
        DIFileRef file,
        ParameterTypesT parameterTypes)
{
#if defined(USE_LLVM_15) || defined(USE_LLVM_6_TO_9)
    (void)file;
    return builder.createSubroutineType(parameterTypes);
#else
    return builder.createSubroutineType(file, parameterTypes);
#endif
}

template<typename SubroutineTypeT, typename RetainedNodesT>
inline DISubprogramRef createFunction(
        llvm::DIBuilder& builder,
        DICompileUnitRef scope,
        llvm::StringRef name,
        llvm::StringRef linkageName,
        DIFileRef file,
        unsigned lineNo,
        SubroutineTypeT type,
        bool isLocalToUnit,
        bool isDefinition,
        unsigned scopeLine,
        unsigned flags,
        bool isOptimized,
        llvm::Function* func,
        RetainedNodesT retainedNodes)
{
#if defined(USE_LLVM_15) || defined(USE_LLVM_6_TO_9)
    (void)retainedNodes;
    auto spFlags = llvm::DISubprogram::toSPFlags(
            isLocalToUnit,
            isDefinition,
            isOptimized);
    auto* subProgram = builder.createFunction(
            scope,
            name,
            linkageName,
            file,
            lineNo,
            type,
            scopeLine,
            static_cast<llvm::DINode::DIFlags>(flags),
            spFlags);
    if (func && subProgram)
    {
        func->setSubprogram(subProgram);
    }
    return subProgram;
#else
    return builder.createFunction(
            scope,
            name,
            linkageName,
            file,
            lineNo,
            type,
            isLocalToUnit,
            isDefinition,
            scopeLine,
            flags,
            isOptimized,
            func);
#endif
}

} // namespace llvm_compat
} // namespace utils
} // namespace lotus

#ifdef LOTUS_LLVM_COMPAT_TEMP_USE_LLVM_6_TO_9
#undef USE_LLVM_6_TO_9
#undef LOTUS_LLVM_COMPAT_TEMP_USE_LLVM_6_TO_9
#endif

#undef LOTUS_LLVM_COMPAT_GE_15
#undef LOTUS_LLVM_COMPAT_GE_14
