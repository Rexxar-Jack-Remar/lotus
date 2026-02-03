#include "Checker/KINT/SmtMemory.h"

namespace kint {

static z3::expr mk_bv(z3::context& ctx, uint64_t v, unsigned bits) {
    return ctx.bv_val(static_cast<uint64_t>(v), bits);
}

SmtMemory::SmtMemory(z3::context& ctx, unsigned addrBits)
    : m_ctx(ctx)
    , m_addrBits(addrBits)
    , m_mem(z3::const_array(ctx.bv_sort(addrBits), ctx.bv_val(0, 8)))
{
}

void SmtMemory::push() {
    m_stack.push_back(m_mem);
}

void SmtMemory::pop() {
    if (!m_stack.empty()) {
        m_mem = m_stack.back();
        m_stack.pop_back();
    }
}

void SmtMemory::reset() {
    m_stack.clear();
    m_mem = z3::const_array(m_ctx.bv_sort(m_addrBits), m_ctx.bv_val(0, 8));
}

z3::expr SmtMemory::addrAdd(const z3::expr& addr, uint64_t byteOffset) const {
    if (byteOffset == 0) return addr;
    return addr + mk_bv(m_ctx, byteOffset, m_addrBits);
}

z3::expr SmtMemory::loadBytes(const z3::expr& addr, unsigned numBytes, bool littleEndian) const {
    if (numBytes == 0) {
        return m_ctx.bv_val(0, 0);
    }

    // Build a bit-vector of width numBytes*8 from bytes in memory.
    // Little endian: addr+0 is least significant byte.
    // Concatenation order for result: MSB ... LSB.
    z3::expr result = z3::select(m_mem, addrAdd(addr, littleEndian ? (numBytes - 1) : 0));
    for (unsigned i = 1; i < numBytes; ++i) {
        const unsigned byteIndex = littleEndian ? (numBytes - 1 - i) : i;
        z3::expr b = z3::select(m_mem, addrAdd(addr, byteIndex));
        result = z3::concat(b, result);
    }
    return result;
}

void SmtMemory::storeBytes(const z3::expr& addr, const z3::expr& value, unsigned numBytes, bool littleEndian) {
    if (numBytes == 0) return;

    // Ensure value is exactly numBytes*8.
    const unsigned targetBits = numBytes * 8;
    z3::expr v = value;
    const unsigned vbw = v.get_sort().bv_size();
    if (vbw < targetBits) {
        v = z3::zext(v, targetBits - vbw);
    } else if (vbw > targetBits) {
        v = v.extract(targetBits - 1, 0);
    }

    // Store low-order byte at addr+0 for little endian.
    for (unsigned i = 0; i < numBytes; ++i) {
        const unsigned valueByteIndex = littleEndian ? i : (numBytes - 1 - i);
        const unsigned lo = valueByteIndex * 8;
        const unsigned hi = lo + 7;
        z3::expr byteVal = v.extract(hi, lo);
        m_mem = z3::store(m_mem, addrAdd(addr, i), byteVal);
    }
}

z3::expr SmtMemory::loadInt(const z3::expr& addr, unsigned bitWidth, unsigned storeBytes, bool littleEndian) const {
    // Load storeBytes bytes, then truncate to bitWidth (LLVM i1 uses 1 byte store size).
    z3::expr bytes = loadBytes(addr, storeBytes, littleEndian);
    const unsigned loadedBits = storeBytes * 8;
    if (bitWidth == loadedBits) return bytes;
    if (bitWidth < loadedBits) return bytes.extract(bitWidth - 1, 0);
    return z3::zext(bytes, bitWidth - loadedBits);
}

void SmtMemory::storeInt(const z3::expr& addr, const z3::expr& value, unsigned bitWidth, unsigned storeSizeBytes,
                         bool littleEndian) {
    // Extend/truncate to store size in bytes, then write bytes.
    const unsigned storeBits = storeSizeBytes * 8;
    z3::expr v = value;
    const unsigned vbw = v.get_sort().bv_size();
    if (vbw < storeBits) {
        v = z3::zext(v, storeBits - vbw);
    } else if (vbw > storeBits) {
        v = v.extract(storeBits - 1, 0);
    }
    (void)bitWidth; // bitWidth affects semantics, but store size determines bytes written.
    storeBytes(addr, v, storeSizeBytes, littleEndian);
}

} // namespace kint
