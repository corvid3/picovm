#include <ctype.h>
#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "defs.h"

#define DIRECTIVE_SET "set"
#define DIRECTIVE_OFFSET "offset"
#define DIRECTIVE_WORD "word"
#define DIRECTIVE_BYTE "byte"
#define DIRECTIVE_ASCII "ascii"
#define DIRECTIVE_ASCIZ "asciz"

struct token
{
  enum tokty
  {
    IDENT,
    DIRECTIVE,
    STRING,
    LABELDEF,

    COMMA,
    SEMICOLON,

    REGISTER,
    IMMVAL,      // #A2Bh
    LBLVAL,      // &somewhere
    DEREFVAL,    // *A2Bh
    LBLDEREF,    // @somewhere
    DEREF,       // [r0]
    DEREFOFF,    // [r0 + #2]
    DEREFOFFLBL, // [r0 + &somewhere]

    TOK_NOP,

    TOK_MOV,

    TOK_LOAD,
    TOK_STOR,

    TOK_ADD,
    TOK_SUB,
    TOK_MUL,
    TOK_DIV,

    TOK_CALL,
    TOK_RET,
    TOK_RTI,
    TOK_PUSH,
    TOK_POP,
    TOK_SETHEAD,
    TOK_SETBASE,

    TOK_TEST,
    TOK_JUMP,
    TOK_BEQL,
    TOK_BNEQ,
    TOK_BLES,
    TOK_BGRT,
    TOK_BLTE,
    TOK_BGTE,

    TOK_READ,
    TOK_WRITE,

    TOK_ENINT,
    TOK_DISINT,

    TOK_HALT,

    TOK_EOF,
  } t;

  union tokdat
  {
    char* s;
    int i;

    struct
    {
      int reg, off;
    } deref_off;

    struct
    {
      int reg;
      char* s;
    } deref_off_lbl;
  } d;
};

struct tok_str_pair
{
  const char* dat;
  enum tokty ty;
};

static const struct tok_str_pair tokpairs[] = {
  { .dat = "nop", .ty = TOK_NOP },
  { .dat = "load", .ty = TOK_LOAD },
  { .dat = "stor", .ty = TOK_STOR },
  { .dat = "add", .ty = TOK_ADD },
  { .dat = "sub", .ty = TOK_SUB },
  { .dat = "mul", .ty = TOK_MUL },
  { .dat = "div", .ty = TOK_DIV },
  { .dat = "call", .ty = TOK_CALL },
  { .dat = "ret", .ty = TOK_RET },
  { .dat = "push", .ty = TOK_PUSH },
  { .dat = "pop", .ty = TOK_POP },
  { .dat = "sethead", .ty = TOK_SETHEAD },
  { .dat = "setbase", .ty = TOK_SETBASE },
  { .dat = "test", .ty = TOK_TEST },
  { .dat = "jump", .ty = TOK_JUMP },
  { .dat = "beql", .ty = TOK_BEQL },
  { .dat = "bneq", .ty = TOK_BNEQ },
  { .dat = "bles", .ty = TOK_BLES },
  { .dat = "bgrt", .ty = TOK_BGRT },
  { .dat = "blte", .ty = TOK_BLTE },
  { .dat = "bgte", .ty = TOK_BGTE },
  { .dat = "read", .ty = TOK_READ },
  { .dat = "write", .ty = TOK_WRITE },
  { .dat = "enint", .ty = TOK_ENINT },
  { .dat = "disint", .ty = TOK_DISINT },
  { .dat = "halt", .ty = TOK_HALT },
  { .dat = "rti", .ty = TOK_RTI }
};

struct symbol
{
  char* label;
  uint16_t loc;
};

struct unresolved
{
  uint16_t at;
  char* to;
};

#define MAX(x, y) (x > y ? x : y)
#define CURC src[srcidx]
#define MAX_SYMBOLS 512
#define MAX_UNRESOLVED 1024

static const char* src;
static int srcidx;

static struct symbol symbols[MAX_SYMBOLS];
static struct unresolved unresolved[MAX_UNRESOLVED];
static char outbuf[ROMLEN];

static int line, col;

static int num_syms;
static int num_unresolved;
// the current write index to the outbuf
// can be modified by directives (see .set)
static int outbuf_idx;
static int outbuf_max_len;

// the outbuf offset, does not modify the index of the outbuf write
// makes the current outbuf location "think" that its in a different location
// useful for writing roms
// e.g. ".set $0 .offset $0x400h label hi goto hi"
//       writes a "JMP $0x400h" @ 0x0000 in outbuf
static int outbuf_offset;

static const char* TOKTY_NAMES[] = {
  [IDENT] = "IDENT",
  [DIRECTIVE] = "DIRECTIVE",
  [STRING] = "STRING",

  [COMMA] = "COMMA",
  [SEMICOLON] = "SEMICOLON",

  [REGISTER] = "REGISTER",
  [IMMVAL] = "IMMVAL",           // #A2Bh
  [LBLVAL] = "LBLVAL",           // &somewhere
  [DEREFVAL] = "DEREFVAL",       // *A2Bh
  [LBLDEREF] = "LBLDEREF",       // @somewhere
  [DEREF] = "DEREF",             // [r0]
  [DEREFOFF] = "DEREFOFF",       // [r0 + #2]
  [DEREFOFFLBL] = "DEREFOFFLBL", // [r0 + &somewhere]

  [TOK_NOP] = "TOK_NOP",

  [TOK_MOV] = "TOK_MOV",

  [TOK_LOAD] = "TOK_LOAD",
  [TOK_STOR] = "TOK_STOR",

  [TOK_ADD] = "TOK_ADD",
  [TOK_SUB] = "TOK_SUB",
  [TOK_MUL] = "TOK_MUL",
  [TOK_DIV] = "TOK_DIV",

  [TOK_CALL] = "TOK_CALL",
  [TOK_RET] = "TOK_RET",
  [TOK_RTI] = "TOK_RTI",
  [TOK_PUSH] = "TOK_PUSH",
  [TOK_POP] = "TOK_POP",
  [TOK_SETHEAD] = "TOK_SETHEAD",
  [TOK_SETBASE] = "TOK_SETBASE",

  [TOK_READ] = "TOK_READ",
  [TOK_WRITE] = "TOK_WRITE",

  [TOK_ENINT] = "TOK_ENINT",
  [TOK_DISINT] = "TOK_DISINT",

  [TOK_HALT] = "TOK_HALT",

  [TOK_EOF] = "TOK_EOF",
};

static char
nextc(void)
{
  char c = src[srcidx++];
  if (c == '\n') {
    line += 1;
    col = 0;
  } else {
    col += 1;
  }
  return c;
}

static bool
ishex(char c)
{
  char l = tolower(c);
  if ((l >= 'a' && l <= 'f') || isdigit(c))
    return true;
  return false;
}

static int
parse_int(void)
{
  int start, end, len, out;
  char* c;

  start = end = srcidx;

  for (;;) {
    if (!ishex(CURC))
      break;
    nextc();
    end += 1;
  }

  len = end - start;

  c = malloc(len + 1);
  c[len] = 0;
  for (int i = start; i < end; i++)
    c[i - start] = src[i];

  errno = 0;
  if (tolower(CURC) == 'h') {
    out = strtol(c, NULL, 16);
    nextc();
  } else
    out = strtol(c, NULL, 10);

  if (errno == EINVAL)
    ERR("invalid number %s\n", c);

  free(c);

  if (out > UINT16_MAX || out < 0)
    ERR("numbers in ASM cannot be >0xFFFF or <0\n");

  return out;
}

static char*
parse_ident(void)
{
  int start, end, len;
  char* c;

  start = end = srcidx;

  for (;;) {
    if (!isalnum(CURC) && CURC != '_')
      break;
    end += 1;
    nextc();
  }

  len = end - start;

  c = malloc(len + 1);
  c[len] = 0;
  for (int i = start; i < end; i++)
    c[i - start] = src[i];

  return c;
}

static char*
parse_string(void)
{
  int start, end, len, out;
  char* c;

  start = end = srcidx;

  while (nextc() != '\"')
    end += 1;
  len = end - start;

  c = malloc(len + 1);
  c[len] = 0;
  for (int i = start; i < end; i++)
    c[i - start] = src[i];

  return c;
}

__attribute__((always_inline)) static inline bool
case_agnostic_strcmp(const char* left, const char* right)
{
  const size_t ll = strlen(left);
  const size_t rl = strlen(right);

  if (ll != rl)
    return false;

  for (size_t i = 0; i < ll; i++)
    if (tolower(left[i]) != tolower(right[i]))
      return false;

  return true;
}

struct token
next(void)
{
retry:
  while (CURC != 0 && isspace(CURC))
    nextc();

  switch (CURC) {
    case 0:
      return (struct token){ .t = TOK_EOF };

    case '|':
      while (CURC != '\n')
        nextc();
      goto retry;

    case ';':
      nextc();
      return (struct token){ .t = SEMICOLON };

    case '"':
      nextc();
      return (struct token){ .t = STRING, .d.s = parse_string() };

    case '.':
      nextc();
      char* str = parse_ident();
      return (struct token){ .t = DIRECTIVE, .d.s = str };

    case ',':
      nextc();
      return (struct token){ .t = COMMA };

    case '%':
      nextc();
      int idx = parse_int();
      if (idx > 16)
        ERR("register indexes must be between inclusive 0 and 15\n");
      return (struct token){ .t = REGISTER, .d.i = idx };

    case '#':
      nextc();
      return (struct token){
        .t = IMMVAL,
        .d.i = parse_int(),
      };

    case '*':
      nextc();
      return (struct token){
        .t = DEREFVAL,
        .d.i = parse_int(),
      };

    case '@':
      nextc();
      return (struct token){
        .t = LBLDEREF,
        .d.s = parse_ident(),
      };

    default:
      if (isalpha(CURC) || CURC == '_') {
        char* str = parse_ident();

        for (size_t i = 0; i < sizeof(tokpairs) / sizeof(struct tok_str_pair);
             i++)
          if (case_agnostic_strcmp(str, tokpairs[i].dat)) {
            free(str);
            return (struct token){ .t = tokpairs[i].ty };
          }

        if (src[srcidx] == ':') {
          nextc();
          return (struct token){
            .t = LABELDEF,
            .d.s = str,
          };
        } else {
          return (struct token){
            .t = LBLVAL,
            .d.s = str,
          };
        }
      }
  }

  ERR("unknown token in lexer <%c>\n", CURC);
}

__attribute__((always_inline)) static inline struct token
peek(void)
{
  int tmp = srcidx;
  struct token out = next();
  srcidx = tmp;
  return out;
}

static void
init(const char* indat)
{
  srcidx = 0;
  src = indat;

  line = col = 0;

  num_syms = 0;
  outbuf_idx = 0;
  outbuf_offset = 0;
}

#define PUSH_BYTE(i) outbuf[outbuf_idx++] = (uint8_t)i
#define PUSH_SHORT(i)                                                          \
  {                                                                            \
    outbuf[outbuf_idx++] = (i >> 8);                                           \
    outbuf[outbuf_idx++] = (i);                                                \
  }
#define RELOCATE_THIS 0x10000

static inline void
mark_last_unresolved(char* to)
{
  unresolved[num_unresolved++] = (struct unresolved){
    .at = outbuf_idx - 2,
    .to = to,
  };
}

static struct symbol*
get_sym(const char* str)
{
  for (int i = 0; i < num_syms; i++) {
    if (strcmp(symbols[i].label, str) == 0)
      return &symbols[i];
  }

  return NULL;
}

// resolve all symbols
static void
link(void)
{
  for (int i = 0; i < num_unresolved; i++) {
    struct unresolved cur = unresolved[i];
    struct symbol* sym = get_sym(cur.to);

    printf(
      "linking... @%04Xh -> <%s> = @%04Xh\n", cur.at, sym->label, sym->loc);

    if (!sym)
      ERR("failed to find symbol\n");

    outbuf[cur.at] = (uint8_t)(sym->loc >> 8);
    outbuf[cur.at + 1] = (uint8_t)(sym->loc);
  }
}

static inline void
push_symbol(char* name)
{
  symbols[num_syms++] =
    (struct symbol){ .label = name, .loc = outbuf_idx + outbuf_offset };
  printf("new label: <%s> = %04Xh\n", name, outbuf_idx + outbuf_offset);
}

static void
directive_set(void)
{
  struct token tok = next();
  if (tok.t != IMMVAL)
    ERR("expected immediate value after .set directive\n");
  outbuf_idx = tok.d.i;
}

static void
directive_offset(void)
{
  struct token tok = next();
  if (tok.t != IMMVAL)
    ERR("expected immediate value after .offset directive\n");
  outbuf_offset = tok.d.i;
}

static void
directive_word(void)
{
  struct token tok = next();
  switch (tok.t) {
    case IMMVAL:
      PUSH_BYTE(tok.d.i >> 8);
      PUSH_BYTE(tok.d.i);
      break;

    case LBLVAL:
      PUSH_BYTE(0x00);
      PUSH_BYTE(0x00);
      mark_last_unresolved(tok.d.s);
      break;

    default:
      ERR("expected immediate value or labelptr after .word directive\n");
  }
}

static void
directive_byte(void)
{
  struct token tok = next();
  switch (tok.t) {
    case IMMVAL:
      if (tok.d.i < 0 || tok.d.i > UINT8_MAX)
        ERR(".byte directive has value outside of uint8_t range\n")
      PUSH_BYTE(tok.d.i);
      break;

    default:
      ERR("expected immediate value after .byte directive\n")
  }
}

static void
directive_ascii(void)
{
  struct token tok = next();
  if (tok.t != STRING)
    ERR(".ascii directive must have a string immediately after\n")
  for (size_t i = 0; i < strlen(tok.d.s); i++)
    PUSH_BYTE(tok.d.s[i]);
}

static void
directive_asciz(void)
{
  struct token tok = next();
  if (tok.t != STRING)
    ERR(".asciz directive must have a string immediately after\n")
  for (size_t i = 0; i < strlen(tok.d.s); i++)
    PUSH_BYTE(tok.d.s[i]);
  PUSH_BYTE(0x00);
}

struct
{
  const char* name;
  void (*ptr)(void);
} directive_pairs[] = {
  { .name = DIRECTIVE_SET, .ptr = directive_set },
  { .name = DIRECTIVE_OFFSET, .ptr = directive_offset },
  { .name = DIRECTIVE_WORD, .ptr = directive_word },
  { .name = DIRECTIVE_BYTE, .ptr = directive_byte },
  { .name = DIRECTIVE_ASCII, .ptr = directive_ascii },
  { .name = DIRECTIVE_ASCIZ, .ptr = directive_asciz },
};

const int directive_pair_len =
  sizeof(directive_pairs) / sizeof(*directive_pairs);

static bool
match_directive(const char* in)
{
  for (size_t i = 0; i < directive_pair_len; i++) {
    if (strcmp(directive_pairs[i].name, in) == 0) {
      directive_pairs[i].ptr();
      return true;
    }
  }

  return false;
}

struct matrix_variant
{
  const enum vm_ops out;
  const int num_ops;
  const enum tokty* operands;
};

struct matrix_instruction
{
  const enum tokty tok;
  const int num_vars;
  const struct matrix_variant* variants;
};

#define DEFNINSTR(whensees, ...)                                               \
  {                                                                            \
    whensees,                                                                  \
      sizeof((const struct matrix_variant[])__VA_ARGS__) /                     \
        sizeof(const struct matrix_variant),                                   \
      (const struct matrix_variant[])__VA_ARGS__,                              \
  }
#define DEFNZINSTR(whensees, out)                                              \
  {                                                                            \
    whensees, 1,                                                               \
      (const struct matrix_variant[]){                                         \
        out,                                                                   \
        0,                                                                     \
        NULL,                                                                  \
      },                                                                       \
  }
#define DEFNVARI(out, ...)                                                     \
  {                                                                            \
    out, sizeof((const enum tokty[])__VA_ARGS__) / sizeof(enum tokty),         \
      (const enum tokty[])__VA_ARGS__,                                         \
  }

struct matrix_instruction instruction_matrix[] = {
  DEFNZINSTR(TOK_NOP, NOP),
  DEFNZINSTR(TOK_RET, RET),
  DEFNZINSTR(TOK_ENINT, ENINT),
  DEFNZINSTR(TOK_DISINT, DISINT),
  DEFNZINSTR(TOK_RTI, RTI),
  DEFNZINSTR(TOK_HALT, HALT),
  DEFNINSTR(TOK_SETHEAD,
            {
              DEFNVARI(SETHEAD, { IMMVAL }),
              DEFNVARI(SETHEAD, { LBLVAL }),
            }),
  DEFNINSTR(TOK_SETBASE,
            {
              DEFNVARI(SETBASE, { IMMVAL }),
              DEFNVARI(SETBASE, { LBLVAL }),
            }),
  DEFNINSTR(TOK_CALL,
            {
              DEFNVARI(CALL, { IMMVAL }),
              DEFNVARI(CALL, { LBLVAL }),
            }),
  DEFNINSTR(TOK_LOAD,
            {
              DEFNVARI(LOAD_REG_REG, { REGISTER, REGISTER }),
              DEFNVARI(LOAD_REG_IMM, { REGISTER, IMMVAL }),
              DEFNVARI(LOAD_REG_IMM, { REGISTER, LBLVAL }),
              DEFNVARI(LOAD_REG_DEREF, { REGISTER, DEREFVAL }),
              DEFNVARI(LOAD_REG_DEREF, { REGISTER, LBLDEREF }),
            }),
  DEFNINSTR(TOK_STOR,
            {
              DEFNVARI(STOR_PTRDEREF_IMM, { DEREFVAL, IMMVAL }),
              DEFNVARI(STOR_PTRDEREF_IMM, { DEREFVAL, LBLVAL }),
              DEFNVARI(STOR_PTRDEREF_IMM, { LBLDEREF, IMMVAL }),
              DEFNVARI(STOR_PTRDEREF_IMM, { LBLDEREF, LBLVAL }),

              DEFNVARI(STOR_PTRDEREF_REG, { DEREFVAL, REGISTER }),
              DEFNVARI(STOR_PTRDEREF_REG, { LBLDEREF, REGISTER }),
            }),
  DEFNINSTR(TOK_ADD,
            {
              DEFNVARI(ADD_REG_REG, { REGISTER, REGISTER }),
              DEFNVARI(ADD_REG_IMM, { REGISTER, IMMVAL }),
            }),
  DEFNINSTR(TOK_SUB,
            {
              DEFNVARI(SUB_REG_REG, { REGISTER, REGISTER }),
              DEFNVARI(SUB_REG_IMM, { REGISTER, IMMVAL }),
            }),
  DEFNINSTR(TOK_MUL,
            {
              DEFNVARI(MUL_REG_REG, { REGISTER, REGISTER }),
              DEFNVARI(MUL_REG_IMM, { REGISTER, IMMVAL }),
            }),
  DEFNINSTR(TOK_DIV,
            {
              DEFNVARI(DIV_REG_REG, { REGISTER, REGISTER }),
              DEFNVARI(DIV_REG_IMM, { REGISTER, IMMVAL }),
            }),
  DEFNINSTR(TOK_TEST,
            {
              DEFNVARI(TEST_REG_REG, { REGISTER, REGISTER }),
              DEFNVARI(TEST_REG_IMM, { REGISTER, IMMVAL }),
            }),
  DEFNINSTR(TOK_JUMP,
            {
              DEFNVARI(BRANCH, { IMMVAL }),
              DEFNVARI(BRANCH, { LBLVAL }),
            }),
  DEFNINSTR(TOK_BEQL,
            {
              DEFNVARI(BRANCH_EQUAL, { IMMVAL }),
              DEFNVARI(BRANCH_EQUAL, { LBLVAL }),
            }),
  DEFNINSTR(TOK_BNEQ,
            {
              DEFNVARI(BRANCH_NOT_EQUAL, { IMMVAL }),
              DEFNVARI(BRANCH_NOT_EQUAL, { LBLVAL }),
            }),
  DEFNINSTR(TOK_BLES,
            {
              DEFNVARI(BRANCH_LESS_THAN, { IMMVAL }),
              DEFNVARI(BRANCH_LESS_THAN, { LBLVAL }),
            }),
  DEFNINSTR(TOK_BGRT,
            {
              DEFNVARI(BRANCH_GREATER_THAN, { IMMVAL }),
              DEFNVARI(BRANCH_GREATER_THAN, { LBLVAL }),
            }),
  DEFNINSTR(TOK_BLTE,
            {
              DEFNVARI(BRANCH_LESS_THAN_EQUAL, { IMMVAL }),
              DEFNVARI(BRANCH_LESS_THAN_EQUAL, { LBLVAL }),
            }),
  DEFNINSTR(TOK_BGTE,
            {
              DEFNVARI(BRANCH_GREATER_THAN_EQUAL, { IMMVAL }),
              DEFNVARI(BRANCH_GREATER_THAN_EQUAL, { LBLVAL }),
            }),
  DEFNINSTR(TOK_WRITE,
            {
              DEFNVARI(WRITEOUT_IMM_IMM, { IMMVAL, DEREFVAL }),
              DEFNVARI(WRITEOUT_IMM_IMM, { IMMVAL, LBLDEREF }),
              DEFNVARI(WRITEOUT_REG_IMM, { REGISTER, DEREFVAL }),
              DEFNVARI(WRITEOUT_REG_IMM, { REGISTER, LBLDEREF }),
              DEFNVARI(WRITEOUT_IMM_REG, { IMMVAL, REGISTER }),
              DEFNVARI(WRITEOUT_REG_REG, { REGISTER, REGISTER }),
            }),
  DEFNINSTR(TOK_READ,
            {
              DEFNVARI(READIN_IMM_IMM, { IMMVAL, DEREFVAL }),
              DEFNVARI(READIN_IMM_IMM, { IMMVAL, LBLDEREF }),
              DEFNVARI(READIN_REG_IMM, { REGISTER, DEREFVAL }),
              DEFNVARI(READIN_REG_IMM, { REGISTER, LBLDEREF }),
              DEFNVARI(READIN_IMM_REG, { IMMVAL, REGISTER }),
              DEFNVARI(READIN_REG_REG, { REGISTER, REGISTER }),
            }),
};

const int instr_matrix_len =
  sizeof(instruction_matrix) / sizeof(struct matrix_instruction);

static void
matrix_instr_perform(struct matrix_instruction instr)
{
  struct token toks[4];
  int num_ops = 0;

  for (;;) {
    const struct token t = peek();
    if (t.t == SEMICOLON)
      break;
    toks[num_ops++] = next();
  }

  // skip the semicolon...
  next();

  for (int vari = 0; vari < instr.num_vars; vari++) {
    struct matrix_variant cv = instr.variants[vari];

    if (cv.num_ops != num_ops)
      continue;

    for (int opi = 0; opi < cv.num_ops; opi++) {
      if (cv.operands[opi] != toks[opi].t)
        goto fail;
    }

    PUSH_BYTE(cv.out);

    for (int opi = 0; opi < cv.num_ops; opi++) {
      // special case for "dual registers"
      if (opi < num_ops - 1 && toks[opi].t == REGISTER &&
          toks[opi + 1].t == REGISTER) {
        PUSH_BYTE(toks[opi].d.i << 4 | toks[opi + 1].d.i);
        opi++;
        continue;
      }

      if (toks[opi].t == REGISTER) {
        PUSH_BYTE(toks[opi].d.i);
      } else if (toks[opi].t == IMMVAL || toks[opi].t == DEREFVAL) {
        PUSH_SHORT(toks[opi].d.i);
      } else if (toks[opi].t == LBLVAL || toks[opi].t == LBLDEREF) {
        PUSH_SHORT(0);
        mark_last_unresolved(toks[opi].d.s);
      }
    }
    return;

  fail:;
  }

  // fail conditions...
  for (int vari = 0; vari < instr.num_vars; vari++) {
    struct matrix_variant cv = instr.variants[vari];
    if (cv.num_ops != num_ops)
      continue;
    for (int opi = 0; opi < cv.num_ops; opi++) {
      if (toks[opi].t != cv.operands[opi]) {
        ERR("%i:%i invalid value in opcode <%s>\n",
            line + 1,
            col + 1,
            TOKTY_NAMES[instr.tok]);
      }
    }
  }

  // we didn't find any variant with the same number of ops
  // which means we
  ERR("invalid number of operands within an opcode %s\n",
      TOKTY_NAMES[instr.tok])
}

static struct matrix_instruction
matrix_lookup(enum tokty ty)
{
  fflush(stdout);
  for (size_t opidx = 0; opidx < instr_matrix_len; opidx++) {
    struct matrix_instruction cur_matr = instruction_matrix[opidx];
    if (cur_matr.tok == ty)
      return cur_matr;
  }

  ERR("%i:%i unknown instruction in perform_matrix: <%s>\n",
      line + 1,
      col + 1,
      TOKTY_NAMES[ty]);
}

static void
begin_assemble(void)
{
  struct token t;
  for (;;) {
    t = next();

    if (t.t == TOK_EOF)
      break;
    else if (t.t == LABELDEF) {
      push_symbol(t.d.s);
    } else if (t.t == DIRECTIVE) {
      if (!match_directive(t.d.s))
        ERR("unknown directive %s\n", t.d.s);
    } else {
      matrix_instr_perform(matrix_lookup(t.t));
    }

    outbuf_max_len = MAX(outbuf_max_len, outbuf_idx);
  }
}

extern char*
assemble(const char* in, size_t* outlen)
{
  init(in);

  begin_assemble();
  link();

  char* outbufcpy = malloc(outbuf_max_len);
  memcpy(outbufcpy, outbuf, outbuf_max_len);
  *outlen = outbuf_max_len;

  return outbufcpy;
}
