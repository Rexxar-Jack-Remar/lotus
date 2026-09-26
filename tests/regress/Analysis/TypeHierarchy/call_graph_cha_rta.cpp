struct Base {
  virtual ~Base() = default;
  virtual int foo() { return 1; }
};

struct DerivedAllocated : Base {
  int foo() override { return 2; }
};

struct DerivedUnallocated : Base {
  ~DerivedUnallocated() override;
  int foo() override { return 3; }
};
DerivedUnallocated::~DerivedUnallocated() = default;

Base *createObj(bool cond) {
  if (cond) {
    return new DerivedAllocated();
  }
  return new Base();
}

int main() {
  Base *b = createObj(true);
  int res = b->foo();
  return res;
}
