#define _POSIX_C_SOURCE 199309L

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/poll.h>
#include <sys/signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "defs.h"

static uint8_t ram[RAMSIZE];
static uint16_t rs[16];
static uint16_t ip;
static uint16_t stack_base, stack_head;
static uint8_t flags;
static bool interrupt_mask;

static int intfd;
struct pollfd intpollfd;

__attribute__((always_inline)) static inline bool
is_halting(void)
{
  return flags & HALT_FLAG;
}

__attribute__((always_inline)) static inline uint8_t
next_byte_adv(void)
{
  return ram[ip++];
}

__attribute__((always_inline)) static inline uint16_t
next_short_adv(void)
{
  uint8_t high = next_byte_adv();
  uint8_t low = next_byte_adv();
  uint16_t out = (((uint16_t)high) << 8) | (uint16_t)low;
  return out;
}

__attribute__((always_inline)) static inline void
set_loc_short(const uint16_t in, const uint16_t at)
{
  ram[at] = (uint8_t)(in >> 8);
  ram[at + 1] = (uint8_t)in;
}

__attribute__((always_inline)) static inline void
set_loc_byte(const uint8_t in, const uint16_t at)
{
  ram[at] = in;
}

__attribute__((always_inline)) static inline uint16_t
get_loc_short(const uint16_t loc)
{
  uint8_t h, l;
  uint16_t out;

  h = ram[loc];
  l = ram[loc + 1];

  out = ((uint16_t)(h << 8)) | (uint16_t)l;

  return out;
}

__attribute__((always_inline)) static inline uint8_t
get_loc_byte(const uint16_t loc)
{
  uint8_t out;

  out = ram[loc];

  return out;
}

__attribute__((always_inline)) static inline struct timespec
diff_timespec(struct timespec left, struct timespec right)
{
  struct timespec diff;

  diff.tv_nsec = left.tv_nsec - right.tv_nsec;
  diff.tv_sec = left.tv_sec - right.tv_sec;
  if (diff.tv_nsec < 0) {
    diff.tv_sec -= 1;
    diff.tv_nsec += 1000000000;
  }

  return diff;
}

__attribute__((always_inline)) static inline bool
timespec_lessthan(struct timespec left, struct timespec right)
{
  if (left.tv_sec < right.tv_sec)
    return true;
  if (left.tv_nsec < right.tv_nsec)
    return true;
  return false;
}

#define CLOCK_INTERONSET_INTERVAL 2000

static struct timespec
gen_min_tick_time(void)
{
  struct timespec out;

  out.tv_nsec = (vm_config.step_sleep * 1000000) + CLOCK_INTERONSET_INTERVAL;

  out.tv_sec = 0;
  while (out.tv_nsec >= 1000000000) {
    out.tv_nsec -= 1000000000;
    out.tv_sec += 1;
  }

  return out;
}

static void
run(void)
{
  uint16_t op0, op1;
  uint32_t tmp;

  // in nanoseconds
  struct timespec cur_tick, last_tick, clock_io, diff;
  clock_io = gen_min_tick_time();

  clock_gettime(CLOCK_MONOTONIC, &cur_tick);

  /// whether or not we are currently performing an interrupt
  bool perf_int = false;

  while (!is_halting()) {
    if (interrupt_mask && !perf_int) {
      if (poll(&intpollfd, 1, 0) != 0) {
        perf_int = true;

        uint8_t int_val;
        if (read(intfd, &int_val, 1) < 0)
          ERR("failed to read from interrupt pipe\n");

        if (int_val < 0 || int_val > 2)
          ERR("interrupt value must be either 0, 1, or 2. crashing!\n")

        printf("caught incoming signal: %i\n", int_val);

        set_loc_short(ip, stack_head);
        stack_head += 2;
        set_loc_byte(flags, stack_head);
        stack_head += 1;

        ip = get_loc_short(int_val * 2);
        printf("%x\n", ip);
      }
    }

    const uint8_t op_byte = next_byte_adv();
    const enum vm_ops op = op_byte;

    if (vm_config.show_steps)
      printf("stepped | ip = %i; op = %x\n", ip, op_byte);

    switch (op) {
      case NOP:
        break;

      case HALT:
        flags |= HALT_FLAG;
        break;

      case MOVE_REG_REG:
        op0 = next_byte_adv();
        rs[op0 & 0x0F] = rs[(op0 & 0xF0) >> 4];
        break;

      case LOAD_REG_IMM:
        op0 = next_byte_adv();
        op1 = next_short_adv();
        rs[op0] = op1;
        break;

      case LOAD_REG_DEREF:
        op0 = next_byte_adv();
        op1 = next_short_adv();
        rs[op0] = get_loc_short(op1);
        break;

      case LOAD_REG_REGDEREF:
        op0 = next_byte_adv();
        rs[op0 & 0x0F] = get_loc_short(rs[(op0 & 0xF0) >> 4]);
        break;

      case LOAD_REG_REGDEREF_OFF:
        op0 = next_byte_adv();
        op1 = next_short_adv();
        rs[op0 & 0x0F] = get_loc_short(rs[(op0 & 0xF0) >> 4] + op1);
        break;

      case STOR_PTRDEREF_REG:
        op0 = next_short_adv();
        op1 = next_byte_adv();
        set_loc_short(rs[op1], op0);
        break;

      case STOR_REGDEREF_REG:
        op0 = next_byte_adv();
        set_loc_short(rs[(op0 & 0xF0) >> 4], ram[op0 & 0x0F]);
        break;

      case STOR_REGDEREF_OFF_REG:
        op0 = next_byte_adv();
        op1 = next_short_adv();
        set_loc_short(rs[(op0 & 0xF0) >> 4] + op1, ram[op0 & 0x0F]);
        break;

      case STOR_PTRDEREF_IMM:
        op0 = next_short_adv();
        op1 = next_short_adv();
        set_loc_short(op0, op1);
        break;

      case STOR_REGDEREF_IMM:
        op0 = next_byte_adv();
        op1 = next_short_adv();
        set_loc_short(rs[op0 & 0x0f], op1);
        break;
      case STOR_REGDEREF_OFF_IMM:
        op0 = next_byte_adv();
        op1 = next_short_adv();
        tmp = next_short_adv();
        set_loc_short(rs[op0 & 0x0f] + op1, tmp);
        break;

      case ADD_REG_REG:
        op0 = next_byte_adv();
        tmp = (uint32_t)rs[op0 & 0x0F] + (uint32_t)rs[(op0 & 0xF0) >> 4];

        if (tmp > UINT16_MAX)
          flags |= CRRY_FLAG;
        else
          flags &= ~CRRY_FLAG;

        rs[op0 & 0x0F] = (uint16_t)tmp;
        break;

      case ADD_REG_IMM:
        op0 = next_byte_adv();
        op1 = next_short_adv();
        tmp = (uint32_t)rs[op0 & 0x0F] + op1;

        if (tmp > UINT16_MAX)
          flags |= CRRY_FLAG;
        else
          flags &= ~CRRY_FLAG;

        rs[op0 & 0x0F] = tmp;
        break;

      case SUB_REG_REG:
        op0 = next_byte_adv();
        tmp = (uint32_t)rs[op0 & 0x0F] - (uint32_t)rs[(op0 & 0xF0) >> 4];

        // we can abuse some principles of register math here
        // if we underflow the u32, it's going to have a val > UINT16_MAX
        if (tmp > UINT16_MAX)
          flags |= CRRY_FLAG;
        else
          flags &= ~CRRY_FLAG;

        rs[op0 & 0x0F] = tmp;
        break;

      case SUB_REG_IMM:
        op0 = next_byte_adv();
        op1 = next_short_adv();

        tmp = rs[op0 & 0x0F] - op1;

        // we can abuse some principles of register math here
        // if we underflow the u32, it's going to have a val > UINT16_MAX
        if (tmp > UINT16_MAX)
          flags |= CRRY_FLAG;
        else
          flags &= ~CRRY_FLAG;

        rs[op0 & 0x0F] = tmp;
        break;

      case MUL_REG_REG:
        op0 = next_byte_adv();

        tmp = (uint32_t)rs[op0 & 0x0F] * (uint32_t)rs[(op0 & 0xF0) >> 4];

        if (tmp > UINT16_MAX)
          flags |= CRRY_FLAG;
        else
          flags &= ~CRRY_FLAG;

        rs[op0 & 0x0F] = tmp;
        break;

      case MUL_REG_IMM:
        op0 = next_byte_adv();
        op1 = next_short_adv();

        tmp = (uint32_t)rs[op0 & 0x0F] * (uint32_t)op1;

        if (tmp > UINT16_MAX)
          flags |= CRRY_FLAG;
        else
          flags &= ~CRRY_FLAG;

        rs[op0 & 0x0F] = tmp;
        break;

      case DIV_REG_REG:
        op0 = next_byte_adv();

        if (rs[(op0 & 0xF0) >> 4] == 0)
          rs[op0 & 0x0F] = 0;
        else
          rs[op0 & 0x0F] /= rs[(op0 & 0xF0) >> 4];

        break;

      case DIV_REG_IMM:
        op0 = next_byte_adv();
        op1 = next_short_adv();
        if (op1 == 0)
          rs[op0 & 0x0F] = 0;
        else
          rs[op0 & 0x0F] /= op1;

        break;

      case NOT_REG:
        op0 = next_byte_adv();
        rs[op0] = ~rs[op0];
        break;

      case OR_REG_REG:
        op0 = next_byte_adv();
        rs[op0 & 0x0F] |= rs[(op0 & 0xF0) >> 4];
        break;

      case OR_REG_IMM:
        op0 = next_byte_adv();
        op1 = next_short_adv();
        rs[op0] |= op1;
        break;

      case AND_REG_REG:
        op0 = next_byte_adv();
        rs[op0 & 0x0F] &= rs[(op0 & 0xF0) >> 4];
        break;

      case AND_REG_IMM:
        op0 = next_byte_adv();
        op1 = next_short_adv();
        rs[op0] &= op1;
        break;

      case XOR_REG_REG:
        op0 = next_byte_adv();
        rs[op0 & 0x0F] ^= rs[(op0 & 0xF0) >> 4];
        break;

      case XOR_REG_IMM:
        op0 = next_byte_adv();
        op1 = next_short_adv();
        rs[op0] ^= op1;
        break;

      case TEST_REG_REG:
        op0 = next_byte_adv();
        tmp = rs[op0 & 0x0F] - rs[(op0 & 0xF0) >> 4];

        // abusing the underflow principal once more...
        if (tmp > UINT16_MAX)
          flags |= PLUS_FLAG;
        else
          flags &= ~PLUS_FLAG;

        if (tmp == 0)
          flags |= ZERO_FLAG;
        else
          flags &= ~ZERO_FLAG;

        if (tmp % 2)
          flags |= PRTY_FLAG;
        else
          flags &= ~PRTY_FLAG;
        break;

      case TEST_REG_IMM:
        op0 = next_byte_adv();
        op1 = next_short_adv();
        tmp = rs[op0 & 0x0F] - op1;

        // abusing the underflow principal once more...
        if (tmp > UINT16_MAX)
          flags |= PLUS_FLAG;
        else
          flags &= ~PLUS_FLAG;

        if (tmp == 0)
          flags |= ZERO_FLAG;
        else
          flags &= ~ZERO_FLAG;

        if (tmp % 2)
          flags |= PRTY_FLAG;
        else
          flags &= ~PRTY_FLAG;
        break;

      case SWAP:
        op0 = next_byte_adv();
        tmp = rs[op0 & 0x0F];
        rs[op0 & 0x0F] = rs[(op0 & 0xF0) >> 4];
        rs[(op0 & 0xF0) >> 4] = tmp;
        break;

      case CALL:
        set_loc_short(ip, stack_head);
        stack_head += 2;
        ip = next_short_adv();
        break;

      case CALLDYN:
        op0 = next_byte_adv();
        set_loc_short(ip, stack_head);
        stack_head += 2;
        ip = rs[op0 & 0x0F];
        break;

      case RET:
        ip = get_loc_short(stack_head);
        stack_head -= 2;
        break;

      case PUSH:
        op0 = next_byte_adv();
        tmp = rs[op0 & 0x0F];
        set_loc_short(tmp, stack_head);
        stack_head += 2;
        break;

      case POP:
        op0 = next_byte_adv();
        stack_head -= 2;
        rs[op0 & 0x0F] = get_loc_short(stack_head);
        break;

      case SETHEAD:
        op0 = next_short_adv();
        stack_head = op0;
        break;

      case SETBASE:
        op0 = next_short_adv();
        stack_base = op0;
        break;

      case READIN:
        op0 = next_byte_adv();
        if (read(STDIN_FILENO,
                 (void*)&ram[rs[op0 & 0x0F]],
                 rs[(op0 & 0xF0) >> 4]) < 0)
          ERR("failed to read stdin, crashing!\n");
        break;

      case WRITEOUT:
        op0 = next_byte_adv();
        if (write(STDOUT_FILENO,
                  (void*)&ram[rs[op0 & 0x0F]],
                  rs[(op0 & 0xF0) >> 4]) < 0)
          ERR("failed to write to stdout, crashing!\n");
        break;

      case ENINT:
        interrupt_mask = true;
        break;

      case DISINT:
        interrupt_mask = false;
        break;

      case BRANCH:
        ip = next_short_adv();
        break;

      case BRANCH_EQUAL:
        if (flags & ZERO_FLAG)
          ip = next_short_adv();
        break;

      case BRANCH_NOT_EQUAL:
        if (!(flags & ZERO_FLAG))
          ip = next_short_adv();
        break;

      case BRANCH_LESS_THAN:
        if (!(flags & ZERO_FLAG) && !(flags & PLUS_FLAG))
          ip = next_short_adv();
        break;

      case BRANCH_GREATER_THAN:
        if (!(flags & ZERO_FLAG) && !(flags & PLUS_FLAG))
          ip = next_short_adv();
        break;

      case BRANCH_LESS_THAN_EQUAL:
        if ((flags & ZERO_FLAG) || !(flags & PLUS_FLAG))
          ip = next_short_adv();
        break;

      case BRANCH_GREATER_THAN_EQUAL:
        if ((flags & ZERO_FLAG) || !(flags & PLUS_FLAG))
          ip = next_short_adv();
        break;

      case RTI:
        stack_head -= 1;
        flags = get_loc_byte(stack_head);
        stack_head -= 2;
        ip = get_loc_short(stack_head);
        perf_int = false;
        break;
    }

    last_tick = cur_tick;
    clock_gettime(CLOCK_MONOTONIC, &cur_tick);

    diff = diff_timespec(cur_tick, last_tick);
    // printf("diff: %li %li\n", diff.tv_sec, diff.tv_nsec);
    // printf("io: %li %li\n", clock_io.tv_sec, clock_io.tv_nsec);
    if (timespec_lessthan(diff, clock_io)) {
      const struct timespec to_sleep = diff_timespec(clock_io, diff);
      // printf("%li %li\n", to_sleep.tv_sec, to_sleep.tv_nsec);
      struct timespec rem;
      nanosleep(&to_sleep, &rem);
    }

    clock_gettime(CLOCK_MONOTONIC, &cur_tick);
  }
}

extern void
run_with_rom(const uint8_t* in, size_t len)
{
  ip = 0;
  interrupt_mask = false;

  if (mkfifo(INTFIFO, 0666) < 0 && errno != EEXIST)
    ERR("failed to make fifo for sysints");

  intfd = open(INTFIFO, O_RDONLY | O_NONBLOCK);

  if (intfd < 0)
    ERR("failed to open fifo for sysints");

  intpollfd.events = POLLIN;
  intpollfd.fd = intfd;

  if (len > ROMLEN)
    ERR("rom len is too large: %lu | must be <= %lu\n", len, ROMLEN);

  memcpy(&ram[ROMLOC], in, len);

  // setup the vector
  uint16_t startup_vector = get_loc_short(STARTUP_VECTOR);
  ip = startup_vector;

  run();

  if (vm_config.dump_registers) {
    for (size_t i = 0; i < 16; i++)
      printf("%%%li = 0x%x; ", i, rs[i]);
    printf("\n");
  }

  if (vm_config.dump_memory) {
    const char* outfile = vm_config.output_filename;
    if (!outfile)
      outfile = "./vm.dump";
    printf("memory contents dumped to: %s\n", outfile);
    FILE* dumpfile = fopen(outfile, "w");
    if (!dumpfile)
      ERR("failed to open dumpfile for writing\n");
    fwrite(ram, 1, RAMSIZE, dumpfile);
    fclose(dumpfile);
  }

  close(intfd);
}