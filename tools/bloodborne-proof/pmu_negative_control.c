// SPDX-License-Identifier: MIT
// Device PMU negative/positive control. This is not a game benchmark.

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static volatile uint64_t sink;

__attribute__((noinline)) static uint64_t branch_add(uint64_t lhs, uint64_t rhs) {
  return lhs + rhs;
}

__attribute__((noinline)) static uint64_t branch_xor(uint64_t lhs, uint64_t rhs) {
  return lhs ^ rhs;
}

static uint64_t parse_iterations(const char *text) {
  char *end = NULL;
  const unsigned long long value = strtoull(text, &end, 10);
  if (end == text || *end != '\0' || value == 0 || value > UINT64_C(1000000000)) {
    fprintf(stderr, "invalid iterations: %s\n", text);
    exit(64);
  }
  return (uint64_t)value;
}

static void run_control(uint64_t iterations) {
  uint64_t value = UINT64_C(0x9e3779b97f4a7c15);
  for (uint64_t index = 0; index < iterations; ++index) {
    value = value * UINT64_C(6364136223846793005) + UINT64_C(1);
    __asm__ volatile("" : "+r"(value));
  }
  sink = value;
}

static void run_dmb(uint64_t iterations) {
  uint64_t value = 0;
  for (uint64_t index = 0; index < iterations; ++index) {
    __asm__ volatile("dmb ish" ::: "memory");
    value += index ^ (value >> 7);
  }
  sink = value;
}

static void run_branch(uint64_t iterations) {
  typedef uint64_t (*operation)(uint64_t, uint64_t);
  static operation volatile choices[2] = {branch_add, branch_xor};
  uint64_t state = UINT64_C(0x6a09e667f3bcc909);
  uint64_t value = 0;
  for (uint64_t index = 0; index < iterations; ++index) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    operation selected = choices[(state ^ index) & 1];
    value = selected(value, state);
  }
  sink = value;
}

static void run_icache(uint64_t iterations) {
  enum { SLOT_BYTES = 1024, SLOT_COUNT = 2048 };
  const size_t allocation_size = (size_t)SLOT_BYTES * SLOT_COUNT;
  uint32_t *code = mmap(NULL, allocation_size, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (code == MAP_FAILED) {
    perror("mmap");
    exit(70);
  }
  for (size_t slot = 0; slot < SLOT_COUNT; ++slot) {
    uint32_t *entry = code + slot * (SLOT_BYTES / sizeof(uint32_t));
    for (size_t index = 0; index + 1 < SLOT_BYTES / sizeof(uint32_t); ++index) {
      entry[index] = UINT32_C(0xd503201f);  // nop
    }
    entry[SLOT_BYTES / sizeof(uint32_t) - 1] = UINT32_C(0xd65f03c0);  // ret
  }
  __builtin___clear_cache((char *)code, (char *)code + allocation_size);
  if (mprotect(code, allocation_size, PROT_READ | PROT_EXEC) != 0) {
    perror("mprotect");
    exit(71);
  }
  uint64_t state = UINT64_C(0xbb67ae8584caa73b);
  for (uint64_t index = 0; index < iterations; ++index) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    void (*entry)(void) = (void (*)(void))((char *)code +
        (state & (SLOT_COUNT - 1)) * SLOT_BYTES);
    entry();
  }
  if (munmap(code, allocation_size) != 0) {
    perror("munmap");
    exit(72);
  }
  sink = state;
}

int main(int argc, char **argv) {
  if (argc != 3) {
    fprintf(stderr, "usage: %s control|dmb|branch|icache ITERATIONS\n", argv[0]);
    return 64;
  }
  const uint64_t iterations = parse_iterations(argv[2]);
  if (strcmp(argv[1], "control") == 0) {
    run_control(iterations);
  } else if (strcmp(argv[1], "dmb") == 0) {
    run_dmb(iterations);
  } else if (strcmp(argv[1], "branch") == 0) {
    run_branch(iterations);
  } else if (strcmp(argv[1], "icache") == 0) {
    run_icache(iterations);
  } else {
    fprintf(stderr, "invalid mode: %s\n", argv[1]);
    return 64;
  }
  printf("mode=%s iterations=%" PRIu64 " sink=%" PRIu64 "\n",
         argv[1], iterations, sink);
  return 0;
}
