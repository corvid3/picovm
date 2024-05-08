#pragma once

/* config.h

        contains some globals passed in from arguments
        avoids passing around arguments around the program
*/

#include <stdbool.h>
#include <stdlib.h>

struct vm_config
{
  const char* input_filename;
  const char* output_filename;
  const char* parallel_loc;
  
  bool dump_registers;
  bool dump_memory;
  bool show_steps;

  // if not 0, sleep n number of millis between vm steps
  int step_sleep;
};

extern struct vm_config vm_config;
