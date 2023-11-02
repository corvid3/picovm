#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ERR(...)                                                               \
  {                                                                            \
    fprintf(stderr, "\x1b[31;49mERR: \x1b[39;49m" __VA_ARGS__);                \
    exit(1);                                                                   \
  }

#define INTFIFO "/tmp/picovmint.in"

/// the maximum addressable space of 16bit is 0xFFFF + 1 (includes 0)
#define RAMSIZE (0xFFFF + 1)
#define ROMLOC 0xC000
#define ROMLEN ((long)((0xFFFF - 0xC000) + 0x01))
#define STARTUP_VECTOR 0xFFFE

enum flags {
  CRRY_FLAG = 0x01,
  ZERO_FLAG = 0x02,
  PLUS_FLAG = 0x04,
  PRTY_FLAG = 0x08,
  HALT_FLAG = 0x80
};

enum vm_ops {
  NOP = 0x00,
  SWAP = 0x01,

  LOAD_REG_REG = 0x10,
  LOAD_REG_IMM = 0x11,
  LOAD_REG_DEREF = 0x12,
  LOAD_REG_REGDEREF = 0x13,
  LOAD_REG_REGDEREF_OFF = 0x14,

  STOR_PTRDEREF_REG = 0x15, // stor *22cc, %2
  STOR_REGDEREF_REG = 0x16,
  STOR_REGDEREF_OFF_REG = 0x17,
  STOR_PTRDEREF_IMM = 0x18, // stor *22cc, 00
  STOR_REGDEREF_IMM = 0x19, // stor [%0], 00
  STOR_REGDEREF_OFF_IMM = 0x1a, // stor [%0 + 24], 0x02

  // arithmetic/bin functions store the result in the second register
  // division rounds down

  ADD_REG_REG = 0x30,
  ADD_REG_IMM = 0x31,
  SUB_REG_REG = 0x32,
  SUB_REG_IMM = 0x33,
  MUL_REG_REG = 0x34,
  MUL_REG_IMM = 0x35,
  DIV_REG_REG = 0x36,
  DIV_REG_IMM = 0x37,

  NOT_REG = 0x40,
  OR_REG_REG = 0x41,
  OR_REG_IMM = 0x42,
  AND_REG_REG = 0x43,
  AND_REG_IMM = 0x44,
  XOR_REG_REG = 0x45,
  XOR_REG_IMM = 0x46,

  // subtract src from dest, set flags, restore dest
  TEST_REG_REG = 0x50,
  TEST_REG_IMM = 0x51,

  // swap two registers

  CALL = 0xA0,
  CALLDYN = 0xA1,
  RET = 0xA2,
  // return from interrupt
  RTI = 0xA3,
  PUSH = 0xA5,
  POP = 0xA6,
  SETHEAD = 0xAA,
  SETBASE = 0xAB,

  BRANCH = 0xB0,
  BRANCH_EQUAL = 0xB1,
  BRANCH_NOT_EQUAL = 0xB2,
  BRANCH_LESS_THAN = 0xB3,
  BRANCH_GREATER_THAN = 0xB4,
  BRANCH_LESS_THAN_EQUAL = 0xB5,
  BRANCH_GREATER_THAN_EQUAL = 0xB6,

  // read uint16_t num of bytes to uint16_t loc in mem
  READIN_REG_REG = 0xD0,
  READIN_REG_IMM = 0xD1,
  READIN_IMM_REG = 0xD2,
  READIN_IMM_IMM = 0xD3,

  // write uint16_t num of bytes from uint16_t loc in mem
  WRITEOUT_REG_REG = 0xD4,
  WRITEOUT_REG_IMM = 0xD5,
  WRITEOUT_IMM_REG = 0xD6,
  WRITEOUT_IMM_IMM = 0xD7,

  /// enable interrupts
  ENINT = 0xFA,

  /// disable interrupts
  DISINT = 0xFB,

  HALT = 0xFF,
};

extern void run_with_rom(const uint8_t *rom, size_t len, int pipeout_stdin);

extern char *assemble(const char *in, size_t *outlen);
