/* disasm.c

  debug dissasembler
*/

#include <stdint.h>
#include <stdlib.h>

/*
  @param in    : input .rom
  @param len   : length of input rom
  @param start : where to start disassembling
  @param end   : where to stop dissassembling
            */
__attribute__((unused)) extern char*
dissasemble(const uint8_t* in,
            const uint16_t len,
            const uint16_t start,
            const uint16_t end)
{
  (void)in;

  (void)len;
  (void)start;
  (void)end;

  exit(1);
}
