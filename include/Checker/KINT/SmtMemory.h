#pragma once

#include <z3++.h>

#include <vector>

namespace kint {

// A tiny, byte-addressed SMT memory model:
// - address sort: bit-vector (addrBits)
// - value sort:   8-bit bit-vector (byte)
// - load/store of wider integers is implemented via byte concat/split
class SmtMemory {
public:
    SmtMemory(z3::context& ctx, unsigned addrBits);

    unsigned addrBits() const { return m_addrBits; }

    const z3::expr& mem() const { return m_mem; }
    z3::expr& mem() { return m_mem; }

    void push();
    void pop();
    void reset();

    // Loads/stores raw byte vectors of width (numBytes * 8).
    z3::expr loadBytes(const z3::expr& addr, unsigned numBytes, bool littleEndian = true) const;
    void storeBytes(const z3::expr& addr, const z3::expr& value, unsigned numBytes, bool littleEndian = true);

    // Convenience wrappers for integer-typed loads/stores, where `storeBytes` is the LLVM store size in bytes.
    z3::expr loadInt(const z3::expr& addr, unsigned bitWidth, unsigned storeBytes, bool littleEndian = true) const;
    void storeInt(const z3::expr& addr, const z3::expr& value, unsigned bitWidth, unsigned storeSizeBytes,
                  bool littleEndian = true);

private:
    z3::expr addrAdd(const z3::expr& addr, uint64_t byteOffset) const;

    z3::context& m_ctx;
    unsigned m_addrBits;
    z3::expr m_mem;
    std::vector<z3::expr> m_stack;
};

} // namespace kint
