#define _POSIX_C_SOURCE 199309L

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/ip.h>
#include <pthread.h>
#include <signal.h>
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
#include "interrupt.h"
#include "parallel.h"

// define interrupt values here
extern pthread_mutex_t interrupt_mutex;
extern enum interrupt_type interrupt_queue[64];
extern int interrupt_queue_len;

// 16 registers, as we can fit two 4bit reg selectors into one byte
#define NUM_REGS 16

static uint8_t ram[RAMSIZE];
static uint16_t rs[NUM_REGS];
static uint16_t ip;
static uint8_t flags;
static bool interrupt_mask;

static void
dump_registers(void);

void
signal_handler(int sig)
{
  (void)sig;
  flags |= HALT_FLAG;
}

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

__attribute__((always_inline)) static inline void
stack_push_byte(const uint8_t val)
{
  ram[rs[STACK_HEAD_REGISTER]] = val;
  rs[STACK_HEAD_REGISTER] += 1;
}

__attribute__((always_inline)) static inline void
stack_push_short(const uint16_t val)
{
  stack_push_byte(val >> 8);
  stack_push_byte(val & 0xFF);
}

__attribute__((always_inline)) static inline uint8_t
stack_pop_byte(void)
{
  rs[STACK_HEAD_REGISTER] -= 1;
  return ram[rs[STACK_HEAD_REGISTER]];
}

__attribute__((always_inline)) static inline uint16_t
stack_pop_short(void)
{
  return ((uint16_t)stack_pop_byte()) | ((uint16_t)stack_pop_byte()) << 8;
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
    if (interrupt_mask) {
      if (!perf_int) {
        if (current_interrupt != INT_NONE) {
          // begin interrupt procedure
          perf_int = true;
          current_interrupt = INT_NONE;
          stack_push_short(ip);

          switch (current_interrupt) {
            case INT_P0:
              ip = get_loc_short(0x0000);
              break;
            case INT_P1:
              ip = get_loc_short(0x0002);
              break;

            case INT_P2:
              ip = get_loc_short(0x0004);
              break;

            default:
              ERR("invalid value in interrupt switch\n");
          }
        } else
          pthread_cond_signal(&interrupt_cond);
      }
    }

    const uint8_t op_byte = next_byte_adv();
    const enum vm_ops op = op_byte;

    if (vm_config.show_steps)
      printf("stepped | ip = %Xh; op = %Xh\n", ip - 1, op_byte);

    switch (op) {
      case NOP:
        break;

      case HALT:
        flags |= HALT_FLAG;
        break;

      case LOAD_REG_REG:
        op0 = next_byte_adv();
        rs[(op0 & 0xF0) >> 4] = rs[op0 & 0x0F];
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
        rs[(op0 & 0xF0) >> 4] = get_loc_short(rs[(op0 & 0x0F)]);
        break;

      case LOAD_REG_REGDEREF_OFF:
        op0 = next_byte_adv();
        op1 = next_short_adv();
        rs[(op0 & 0xF0) >> 4] = get_loc_short(rs[(op0 & 0x0F)] + op1);
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
        set_loc_short(op1, op0);
        break;

      case STOR_REGDEREF_IMM:
        op0 = next_byte_adv();
        op1 = next_short_adv();
        set_loc_short(op1, rs[op0 & 0x0f]);
        break;
      case STOR_REGDEREF_OFF_IMM:
        op0 = next_byte_adv();
        op1 = next_short_adv();
        tmp = next_short_adv();
        set_loc_short(tmp, rs[op0 & 0x0f] + op1);
        break;

      case ADD_REG_REG:
        op0 = next_byte_adv();
        tmp = (uint32_t)rs[op0 & 0x0F] + (uint32_t)rs[(op0 & 0xF0) >> 4];

        if (tmp > UINT16_MAX)
          flags |= CRRY_FLAG;
        else
          flags &= ~CRRY_FLAG;

        rs[(op0 & 0xF0) >> 4] = (uint16_t)tmp;
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
        tmp = (uint32_t)rs[(op0 & 0xF0) >> 4] - (uint32_t)rs[op0 & 0x0F];

        // we can abuse some principles of register math here
        // if we underflow the u32, it's going to have a val > UINT16_MAX
        if (tmp > UINT16_MAX)
          flags |= CRRY_FLAG;
        else
          flags &= ~CRRY_FLAG;

        rs[(op0 & 0xF0) >> 4] = tmp;
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

        rs[(op0 & 0xF0) >> 4] = tmp;
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
          rs[(op0 & 0xF0) >> 4] = 0;
        else
          rs[(op0 & 0xF0) >> 4] /= rs[op0 & 0x0F];

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
        rs[(op0 & 0xF0) >> 4] |= rs[op0 & 0x0F];
        break;

      case OR_REG_IMM:
        op0 = next_byte_adv();
        op1 = next_short_adv();
        rs[op0] |= op1;
        break;

      case AND_REG_REG:
        op0 = next_byte_adv();
        rs[(op0 & 0xF0) >> 4] &= rs[op0 & 0x0F];
        break;

      case AND_REG_IMM:
        op0 = next_byte_adv();
        op1 = next_short_adv();
        rs[op0] &= op1;
        break;

      case XOR_REG_REG:
        op0 = next_byte_adv();
        rs[(op0 & 0xF0) >> 4] ^= rs[op0 & 0x0F];
        break;

      case XOR_REG_IMM:
        op0 = next_byte_adv();
        op1 = next_short_adv();
        rs[op0] ^= op1;
        break;

      case TEST_REG_REG:
        op0 = next_byte_adv();
        tmp = rs[(op0 & 0xF0) >> 4] - rs[op0 & 0x0F];

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
        op0 = next_short_adv();
        set_loc_short(ip, rs[STACK_HEAD_REGISTER]);
        rs[STACK_HEAD_REGISTER] += 2;
        ip = op0;
        break;

      case CALLDYN:
        op0 = next_byte_adv();
        set_loc_short(ip, rs[STACK_HEAD_REGISTER]);
        rs[STACK_HEAD_REGISTER] += 2;
        ip = rs[op0 & 0x0F];
        break;

      case RET:
        rs[STACK_HEAD_REGISTER] -= 2;
        ip = get_loc_short(rs[STACK_HEAD_REGISTER]);
        break;

      case PUSH:
        op0 = next_byte_adv();
        tmp = rs[op0 & 0x0F];
        set_loc_short(tmp, rs[STACK_HEAD_REGISTER]);
        rs[STACK_HEAD_REGISTER] += 2;
        break;

      case POP:
        op0 = next_byte_adv();
        rs[STACK_HEAD_REGISTER] -= 2;
        rs[op0 & 0x0F] = get_loc_short(rs[STACK_HEAD_REGISTER]);
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
        op0 = next_short_adv();
        if (flags & ZERO_FLAG)
          ip = op0;
        break;

      case BRANCH_NOT_EQUAL:
        op0 = next_short_adv();
        if (!(flags & ZERO_FLAG))
          ip = op0;
        break;

      case BRANCH_LESS_THAN:
        op0 = next_short_adv();
        if (!(flags & ZERO_FLAG) && !(flags & PLUS_FLAG))
          ip = op0;
        break;

      case BRANCH_GREATER_THAN:
        op0 = next_short_adv();
        if (!(flags & ZERO_FLAG) && !(flags & PLUS_FLAG))
          ip = op0;
        break;

      case BRANCH_LESS_THAN_EQUAL:
        op0 = next_short_adv();
        if ((flags & ZERO_FLAG) || !(flags & PLUS_FLAG))
          ip = op0;
        break;

      case BRANCH_GREATER_THAN_EQUAL:
        op0 = next_short_adv();
        if ((flags & ZERO_FLAG) || !(flags & PLUS_FLAG))
          ip = op0;
        break;

      case RTI:
        rs[STACK_HEAD_REGISTER] -= 1;
        flags = get_loc_byte(rs[STACK_HEAD_REGISTER]);
        rs[STACK_HEAD_REGISTER] -= 2;
        ip = get_loc_short(rs[STACK_HEAD_REGISTER]);
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

static const char*
get_register_name_by_idx(size_t idx)
{
  switch (idx) {
    case 0:
      return "%r0";
    case 1:
      return "%r1";
    case 2:
      return "%r2";
    case 3:
      return "%r3";
    case 4:
      return "%r4";
    case 5:
      return "%r5";
    case 6:
      return "%r6";
    case 7:
      return "%r7";
    case 8:
      return "%r8";
    case 9:
      return "%r9";
    case 10:
      return "%x0";
    case 11:
      return "%x1";
    case 12:
      return "%x2";
    case 13:
      return "%x3";
    case 14:
      return "%sh";
    case 15:
      return "%sb";
    default:
      ERR("invalid reg index when trying to get register name\n");
  };
}

static void
dump_registers(void)
{
  for (size_t i = 0; i < NUM_REGS; i++) {
    printf("\x1b[32;49m%s\x1b[39;49m = \x1b[33;49m%04Xh\x1b[39;49m ",
           get_register_name_by_idx(i),
           rs[i]);
    if (i % 4 == 3)
      printf("\n");
  }
}

extern void
run_with_rom(const uint8_t* in, size_t len, int pipeout_stdin)
{
  fake_stdin = pipeout_stdin;

  signal(SIGINT, signal_handler);
  ip = 0;
  interrupt_mask = false;

  if (mkfifo(INTFIFO, 0666) < 0 && errno != EEXIST)
    ERR("failed to make fifo for sysints");

  intfd = open(INTFIFO, O_RDONLY | O_NONBLOCK);

  if (intfd < 0)
    ERR("failed to open fifo for sysints");

  intpollfd.events = POLLIN;
  intpollfd.fd = intfd;

  int stdin_fl = fcntl(STDIN_FILENO, F_GETFL);
  fcntl(STDIN_FILENO, F_SETFL, stdin_fl | O_NONBLOCK);

  if (len > ROMLEN)
    ERR("rom len is too large: %lu | must be <= %lu\n", len, ROMLEN);

  memcpy(&ram[ROMLOC], in, len);

  // setup the vector
  uint16_t startup_vector = get_loc_short(STARTUP_VECTOR);
  ip = startup_vector;

  run();
  printf("\nvm halted\n");

  if (vm_config.dump_registers)
    dump_registers();

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
