#include "Checker/Pulse/PulseDiagnostic.h"
#include <llvm/IR/Function.h>
#include <llvm/Support/raw_ostream.h>

namespace pulse {

// --- AccessToInvalidAddress ---

size_t AccessToInvalidAddress::getHash() const {
    // Hash based on location, issue type, and invalidation kind
    // We don't hash the full trace to allow merging similar issues with different paths
    size_t h = std::hash<const llvm::Instruction*>{}(location_);
    h = h ^ (std::hash<std::string>{}(issue_type_) << 1);
    h = h ^ (std::hash<int>{}(static_cast<int>(invalidation_kind_)) << 2);
    return h;
}

bool AccessToInvalidAddress::equals(const Diagnostic& other) const {
    const auto* o = dynamic_cast<const AccessToInvalidAddress*>(&other);
    if (!o) return false;
    return location_ == o->location_ &&
           issue_type_ == o->issue_type_ &&
           invalidation_kind_ == o->invalidation_kind_;
}

// --- MemoryLeak ---

std::string MemoryLeak::getMessage() const {
    return "Memory leak detected";
}

std::string MemoryLeak::getDescription() const {
    std::string desc = "Memory allocated by " + allocator_name_;
    if (allocation_site_) {
        // Add line number if available
    }
    desc += " is not reachable after this point";
    return desc;
}

size_t MemoryLeak::getHash() const {
    size_t h = std::hash<const llvm::Instruction*>{}(allocation_site_); // Group by allocation site
    return h;
}

bool MemoryLeak::equals(const Diagnostic& other) const {
    const auto* o = dynamic_cast<const MemoryLeak*>(&other);
    if (!o) return false;
    return allocation_site_ == o->allocation_site_;
}

// --- ResourceLeak ---

std::string ResourceLeak::getMessage() const {
    return "Resource leak detected";
}

std::string ResourceLeak::getDescription() const {
    return resource_name_ + " acquired here is never released";
}

size_t ResourceLeak::getHash() const {
    size_t h = std::hash<const llvm::Instruction*>{}(allocation_site_);
    h = h ^ (std::hash<std::string>{}(resource_name_) << 1);
    return h;
}

bool ResourceLeak::equals(const Diagnostic& other) const {
    const auto* o = dynamic_cast<const ResourceLeak*>(&other);
    if (!o) return false;
    return allocation_site_ == o->allocation_site_ &&
           resource_name_ == o->resource_name_;
}

// --- TaintFlow ---

std::string TaintFlow::getMessage() const {
    return "Tainted data flows to sensitive sink";
}

std::string TaintFlow::getDescription() const {
    return "Data from " + source_kind_ + " flows to " + sink_kind_;
}

size_t TaintFlow::getHash() const {
    size_t h = std::hash<const llvm::Instruction*>{}(location_);
    h = h ^ (std::hash<std::string>{}(source_kind_) << 1);
    h = h ^ (std::hash<std::string>{}(sink_kind_) << 2);
    return h;
}

bool TaintFlow::equals(const Diagnostic& other) const {
    const auto* o = dynamic_cast<const TaintFlow*>(&other);
    if (!o) return false;
    return location_ == o->location_ &&
           source_kind_ == o->source_kind_ &&
           sink_kind_ == o->sink_kind_;
}

// --- UnnecessaryCopy ---

std::string UnnecessaryCopy::getMessage() const {
    return "Unnecessary copy of " + variable_name_;
}

std::string UnnecessaryCopy::getDescription() const {
    return "Variable " + variable_name_ + " of type " + type_name_ + " is copied but the copy is not modified";
}

std::string UnnecessaryCopy::getSuggestion() const {
    return "Use a const reference (const &) to avoid the copy.";
}

size_t UnnecessaryCopy::getHash() const {
    size_t h = std::hash<const llvm::Instruction*>{}(location_);
    h = h ^ (std::hash<std::string>{}(variable_name_) << 1);
    return h;
}

bool UnnecessaryCopy::equals(const Diagnostic& other) const {
    const auto* o = dynamic_cast<const UnnecessaryCopy*>(&other);
    if (!o) return false;
    return location_ == o->location_ &&
           variable_name_ == o->variable_name_;
}

} // namespace pulse
