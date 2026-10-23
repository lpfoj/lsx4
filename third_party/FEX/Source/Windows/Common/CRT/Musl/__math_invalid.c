// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright © 2005-2020 Rich Felker, et al.

#include "libm.h"

double __math_invalid(double x) {
  return (x - x) / (x - x);
}
