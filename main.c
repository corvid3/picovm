#define _POSIX_C_SOURCE 199309L

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <libgen.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/poll.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "defs.h"

struct vm_config vm_config = {
  .input_filename = NULL,
  .output_filename = NULL,
  .dump_registers = false,
  .dump_memory = false,
  .show_steps = false,
  .step_sleep = 0,
};

enum runtype
{
  RUN_HELP,
  RUN_VM,
  RUN_ASM,
};

struct argument_help
{
  const char c;
  const char* def;
};

struct argument_help help_args[] = {
  { .c = 'h', "print this help" },
  { .c = 'a', "run picovm in assembler mode" },
  { .c = 'v', "run picovm in vm mode" },
  { .c = 'f', "specify an input filepath" },
  { .c = 'o', "specify an output filepath" },
  { .c = 's', "when running in vm mode, 'n' number of millis between steps" },
  { .c = 'S', "when running in vm mode, display current IP and op per step" },
  { .c = 'd', "when running in vm mode, dump registers" },
  { .c = 'D', "when running in vm mode, dump memory to outfile/generic" },
};

static void
print_help(void)
{
  printf("usage: picovm [-av] -f input [-o output]\n");
  for (size_t i = 0; i < sizeof(help_args) / sizeof(struct argument_help);
       i++) {
    printf("(-%c) %s\n", help_args[i].c, help_args[i].def);
  }
}

static void
run_assembler(void)
{
  FILE *infile, *outfile;
  size_t infilelen, asmlen;
  char *filedata, *input_copy, *outname, *asmdata;

  if (!vm_config.output_filename) {
    input_copy = malloc(strlen(vm_config.input_filename) + 1);
    strcpy(input_copy, vm_config.input_filename);
    char* buf = basename(input_copy);
    outname = malloc(strlen(buf) + 6);
    strrchr(buf, '.')[0] = 0;
    sprintf(outname, "./%s.rom", buf);
    free(input_copy);
  } else
    outname = (char*)vm_config.output_filename;

  infile = fopen(vm_config.input_filename, "r");

  if (!infile)
    ERR("failed to open infile \n");

  fseek(infile, 0, SEEK_END);
  infilelen = ftell(infile);
  rewind(infile);

  filedata = malloc(infilelen + 1);
  filedata[infilelen] = 0;
  if (fread(filedata, 1, infilelen, infile) != infilelen) {
    ERR("failed to read input file\n");
  }

  asmdata = assemble(filedata, &asmlen);

  outfile = fopen(outname, "w");
  if (!outfile)
    ERR("failed to open outfile \n");
  fwrite(asmdata, 1, asmlen, outfile);

  free(filedata);
  fclose(infile);
  fclose(outfile);
}

static int
try_get_fifo(void)
{
  int fd, counter = 0;

  const struct timespec timer = { .tv_nsec = 10000000, .tv_sec = 0 };

  do {
    fd = open(INTFIFO, O_WRONLY);
    counter += 1;
    if (counter > 5)
      return 0;
    if (fd < 0)
      nanosleep(&timer, NULL);
  } while (fd < 0);

  return fd;
}

// forwards any STDIN data to the main VM thread
// and trips the STDIN interrupt for every character
static void*
vm_forwarder(void* vs_pipein)
{
  int pipein = *(int*)vs_pipein;

  int fifofd, retry_counter;
  struct pollfd stdinpollfd = { .events = POLLIN, .fd = STDIN_FILENO };

  fifofd = try_get_fifo();

  if (fifofd < 0)
    ERR("failed to open fifo file from the talker thread | %i %s\n",
        errno,
        strerror(errno));

  for (;;) {
    if (poll(&stdinpollfd, 1, -1) < 0)
      ERR("polling stdin somehow failed, crashing!\n");

    char c;
    if (read(STDIN_FILENO, &c, 1) < 1)
      ERR("faile to read from STDIN in the forwarding thread\n");

    // first write the data
    if (write(pipein, &c, 1) < 1)
      ERR("failed to write into the fakestdin pipe\n");
    // trip the stdin interrupt
    if (write(fifofd, &(int[]){ 0 }, 1) < 0)
      ERR("failed to write stdin interrupt to FIFO\n");
  }
}

static void
run_vm(void)
{
  FILE* file;
  uint8_t* filedata;
  size_t filelen;

  if (!vm_config.input_filename)
    ERR("trying to run a VM with no .rom input file is a bad idea.\n");

  file = fopen(vm_config.input_filename, "rb");
  if (!file)
    ERR("failed to open VM.rom file \"%s\"\n", vm_config.input_filename);

  fseek(file, 0, SEEK_END);
  filelen = ftell(file);
  rewind(file);

  filedata = malloc(filelen);

  if (fread(filedata, 1, filelen, file) < filelen)
    ERR("failed to read rom file \"%s\"\n", vm_config.input_filename);

  fclose(file);

  int stdinpipe[2];
  if (pipe(stdinpipe) < 0)
    ERR("failed to create fakestdin pipe\n");

  pthread_t talker;
  pthread_create(&talker, NULL, vm_forwarder, &stdinpipe[1]);
  run_with_rom(filedata, filelen, stdinpipe[0]);
  pthread_cancel(talker);

  free(filedata);
}

extern int
main(int argc, char** argv)
{
  enum runtype type = RUN_HELP;
  char b;

  while ((b = getopt(argc, argv, "+avhf:o:s:dDS")) != -1) {
    switch (b) {
      case 'f':
        vm_config.input_filename = optarg;
        break;

      case 'h':
        type = RUN_HELP;
        break;

      case 'a':
        type = RUN_ASM;
        break;

      case 'v':
        type = RUN_VM;
        break;

      case 'o':
        vm_config.output_filename = optarg;
        break;

      case 'd':
        vm_config.dump_registers = true;
        break;

      case 'D':
        vm_config.dump_memory = true;
        break;

      case 's':
        vm_config.step_sleep = atoi(optarg);
        break;

      case 'S':
        vm_config.show_steps = true;
        break;

      case '?':
        printf("unknown argument %c\n", optopt);
        break;

      case ':':
        printf("argument %c requires a parameter\n", optopt);
        break;
    }
  }

  switch (type) {
    case RUN_HELP:
      print_help();
      return 0;

    case RUN_ASM:
      run_assembler();
      return 0;

    case RUN_VM:
      run_vm();
      return 0;
  }
}
