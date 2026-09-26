struct Animal {
  virtual ~Animal() = default;
  virtual int speak() = 0;
};

struct Dog : Animal {
  int speak() override { return 10; }
};

struct Cat : Animal {
  int speak() override { return 20; }
};

int main() {
  Dog d;
  Cat c;
  Animal *a1 = &d;
  Animal *a2 = &c;
  int r1 = a1->speak();
  int r2 = a2->speak();
  return r1 + r2;
}
