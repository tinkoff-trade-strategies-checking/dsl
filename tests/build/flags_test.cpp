#include <gtest/gtest.h>

// INV-4: сборка с -ffp-contract=off. Без него компилятор вправе слить a * b + c
// в одну операцию FMA и получить другой результат на другом процессоре.
TEST(BuildFlags, NoFusedMultiplyAdd) {
  // a * b = 1 - 2^-54 не представимо в double и округляется до 1, поэтому
  // раздельные умножение и сложение дают ровно 0; с FMA результат был бы
  // -2^-54.
  volatile double A = 1.0 + 0x1p-27;
  volatile double B = 1.0 - 0x1p-27;
  volatile double C = -1.0;
  EXPECT_EQ(A * B + C, 0.0);
}
