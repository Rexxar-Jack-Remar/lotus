int add(int a, int b) { return a + b; }
int sub(int a, int b) { return a - b; }

int compute(int (*fn)(int, int), int x, int y) {
  return fn(x, y);
}

int main() {
  int res1 = compute(&add, 5, 3);
  int res2 = compute(&sub, 5, 3);
  return res1 + res2;
}
