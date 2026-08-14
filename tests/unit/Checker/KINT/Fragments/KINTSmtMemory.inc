TEST_F(KINTCheckerTest, SmtMemoryLittleEndianRoundTrip) {
  z3::context ctx;
  kint::SmtMemory memory(ctx, 64);
  const z3::expr addr = ctx.bv_val(0, 64);

  memory.storeBytes(addr, ctx.bv_val(0x1234, 16), 2, true);

  EXPECT_EQ(getNumeralU64(memory.loadBytes(addr, 2, true)), 0x1234u);
}

TEST_F(KINTCheckerTest, SmtMemoryBigEndianRoundTrip) {
  z3::context ctx;
  kint::SmtMemory memory(ctx, 64);
  const z3::expr addr = ctx.bv_val(0, 64);

  memory.storeBytes(addr, ctx.bv_val(0x1234, 16), 2, false);

  EXPECT_EQ(getNumeralU64(memory.loadBytes(addr, 2, false)), 0x1234u);
}

TEST_F(KINTCheckerTest, SmtMemoryPointerWidthRoundTrip) {
  z3::context ctx;
  kint::SmtMemory memory(ctx, 64);
  const z3::expr addr = ctx.bv_val(8, 64);

  memory.storeBytes(
      addr, ctx.bv_val(static_cast<uint64_t>(0x1122334455667788ULL), 64), 8,
      true);

  EXPECT_EQ(getNumeralU64(memory.loadBytes(addr, 8, true)),
            0x1122334455667788ULL);
}

TEST_F(KINTCheckerTest, SmtMemoryMemmoveHandlesOverlap) {
  z3::context ctx;
  kint::SmtMemory memory(ctx, 64);
  const z3::expr addr = ctx.bv_val(0, 64);

  memory.storeBytes(addr, ctx.bv_val(0x11223344, 32), 4, true);
  memory.memmoveBytes(ctx.bv_val(1, 64), addr, 3);

  EXPECT_EQ(getNumeralU64(memory.loadBytes(addr, 4, true)), 0x22334444u);
}
