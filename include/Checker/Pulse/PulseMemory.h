#ifndef CHECKER_PULSE_PULSEMEMORY_H
#define CHECKER_PULSE_PULSEMEMORY_H

#include "Checker/Pulse/PulseAbstractValue.h"
#include "Checker/Pulse/PulseValueHistory.h"
#include <llvm/IR/Value.h>
#include <map>
#include <set>

namespace pulse {

/**
 * Address: abstract address with history (for error reporting)
 */
struct Address {
    AbstractValue addr;
    ValueHistory history;  // How we got here
    
    Address() : addr() {}
    Address(AbstractValue a) : addr(a) {}
};

/**
 * Memory attributes: properties attached to addresses
 */
enum class Attribute {
    Allocated,      // Memory was allocated
    Invalid,        // Memory is invalid (freed, out of scope)
    Uninitialized,  // Memory is uninitialized
    Null,           // Pointer is null
    Tainted,        // Value is tainted
    FileHandle,     // File handle resource
    Lock,           // Lock resource
    AsyncResource   // Async/awaitable resource
};

using AttributeSet = std::set<Attribute>;

/**
 * Access path: field access, array index, or dereference
 */
enum class AccessKind {
    Dereference,
    Field,
    ArrayIndex
};

struct Access {
    AccessKind kind;
    union {
        unsigned field_idx;      // For Field
        AbstractValue index;     // For ArrayIndex
    };

    Access() : kind(AccessKind::Dereference), field_idx(0) {}
    explicit Access(AccessKind k) : kind(k), field_idx(0) {}
    Access(unsigned idx) : kind(AccessKind::Field), field_idx(idx) {}

    static Access arrayIndex(AbstractValue idx) {
        Access a;
        a.kind = AccessKind::ArrayIndex;
        a.index = idx;
        return a;
    }

    bool operator<(const Access& other) const {
        if (kind != other.kind) return kind < other.kind;
        if (kind == AccessKind::Field) return field_idx < other.field_idx;
        if (kind == AccessKind::ArrayIndex) return index < other.index;
        return false;
    }
};

/**
 * Stack: maps variables to abstract addresses
 */
class Stack {
private:
    std::map<const llvm::Value*, Address> stack_;
    
public:
    // Allow access for merging
    const std::map<const llvm::Value*, Address>& getMap() const { return stack_; }
    void add(const llvm::Value* var, Address addr);
    Address* find(const llvm::Value* var);
    const Address* find(const llvm::Value* var) const;
    void remove(const llvm::Value* var);
    void clear();
};

/**
 * Heap: graph of abstract addresses connected by access paths
 */
class Heap {
private:
    // addr -> (access -> target_addr)
    std::map<AbstractValue, std::map<Access, Address>> edges_;
    
public:
    // Allow access for merging
    const std::map<AbstractValue, std::map<Access, Address>>& getEdges() const { return edges_; }
    std::map<AbstractValue, std::map<Access, Address>>& getEdges() { return edges_; }
    void addEdge(AbstractValue from, Access access, Address to);
    Address* findEdge(AbstractValue from, Access access);
    const Address* findEdge(AbstractValue from, Access access) const;
    void removeEdges(AbstractValue addr);
};

/**
 * AddressAttributes: properties attached to addresses
 */
class AddressAttributes {
private:
    std::map<AbstractValue, AttributeSet> attrs_;
    
public:
    // Allow access for merging
    const std::map<AbstractValue, AttributeSet>& getAttrs() const { return attrs_; }
    std::map<AbstractValue, AttributeSet>& getAttrs() { return attrs_; }
    void add(AbstractValue addr, Attribute attr);
    void remove(AbstractValue addr, Attribute attr);
    bool has(AbstractValue addr, Attribute attr) const;
    AttributeSet get(AbstractValue addr) const;
    void clear(AbstractValue addr);
};

} // namespace pulse

#endif // CHECKER_PULSE_PULSEMEMORY_H
