#include <stdatomic.h>
#include <stdint.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "util/rapl-utils.h"
#include "util/util.h"

// attacker core; 0 is package level
volatile static int attacker_core_ID;

// constant for power-based instruction throttling
volatile static double power_limit;

// energy_value defines the memory location of the energy value, pointer is a
// static pointer to that memory location that we can reference
volatile double energy_value = 0;

// again, we define the memory location and a pointer to that location that we
// can access
volatile uint32_t instruction_counter;
volatile static uint32_t *instruction_counter_ptr = &instruction_counter;

#define TIME_BETWEEN_MEASUREMENTS 1000000L // 1 millisecond

#define STACK_SIZE 8192

typedef void (*func_ptr_t)();

struct args_t {
  uint64_t iters;
  int selector;
};

// TODO: Workload?
static __attribute__((noinline)) void workload(void) {
  asm volatile(".align 64\t\n"

               "shlx $0, $x0000FFFFFFFF0000, %%rbx\n\t"
               "shlx $1, $x0000FFFFFFFF0000, %%rcx\n\t"
               "shrx $2, $x0000FFFFFFFF0000, %%rsi\n\t"
               "shrx $3, $x0000FFFFFFFF0000, %%rdi\n\t"
               "shlx $4, $x0000FFFFFFFF0000, %%r8\n\t"
               "shlx $5, $x0000FFFFFFFF0000, %%r9\n\t"
               "shrx $6, $x0000FFFFFFFF0000, %%r10\n\t"
               "shrx $7, $x0000FFFFFFFF0000, %%r11\n\t"
               "shlx $8, $x0000FFFFFFFF0000, %%r12\n\t"
               "shlx $9, $x0000FFFFFFFF0000, %%r13\n\t"

               "shrx $0, $x0000FFFFFFFF0000, %%rbx\n\t"
               "shrx $1, $x0000FFFFFFFF0000, %%rcx\n\t"
               "shlx $2, $x0000FFFFFFFF0000, %%rsi\n\t"
               "shlx $3, $x0000FFFFFFFF0000, %%rdi\n\t"
               "shrx $4, $x0000FFFFFFFF0000, %%r8\n\t"
               "shrx $5, $x0000FFFFFFFF0000, %%r9\n\t"
               "shlx $6, $x0000FFFFFFFF0000, %%r10\n\t"
               "shlx $7, $x0000FFFFFFFF0000, %%r11\n\t"
               "shrx $8, $x0000FFFFFFFF0000, %%r12\n\t"
               "shrx $9, $x0000FFFFFFFF0000, %%r13\n\t"
               "lock addl $11, %[instruction_count]"

               : [instruction_count] "+m"(*instruction_counter_ptr)
               :
               : "rbx", "rcx", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12",
                 "r13");
}

static __attribute__((noinline)) void throttle(void) {
  nanosleep((const struct timespec[]){{0, TIME_BETWEEN_MEASUREMENTS}}, NULL);
}

_Atomic func_ptr_t work = workload;

static __attribute__((noinline)) int victim(void *varg) {
  // struct args_t *arg = varg;
  // uint64_t my_uint64 = 0x0000FFFFFFFF0000;
  // uint64_t count = (uint64_t)arg->selector;

  while (1) {
    work();
  }

  return 0;
}

// Collects traces
static __attribute__((noinline)) int governor(void *in) {
  static int rept_index = 0;

  struct args_t *arg = (struct args_t *)in;

  // Pin monitor to a single CPU
  pin_cpu(attacker_core_ID);

  // Set filename
  // The format is, e.g., ./out/all_02_2330.out
  // where 02 is the selector and 2330 is an index to prevent overwriting files
  char output_filename[64];
  sprintf(output_filename, "./out/all_%02d_%06d.out", arg->selector,
          rept_index);
  rept_index += 1;

  // Prepare output file
  FILE *output_file = fopen((char *)output_filename, "w");
  if (output_file == NULL) {
    perror("output file");
  }

  // Prepare
  volatile double energy, prev_energy = rapl_msr(attacker_core_ID, PP0_ENERGY);

  // Collect measurements
  for (uint64_t i = 0; i < arg->iters; i++) {

    // Wait before next measurement
    nanosleep((const struct timespec[]){{0, TIME_BETWEEN_MEASUREMENTS}}, NULL);

    // Collect measurement
    energy = rapl_msr(attacker_core_ID, PP0_ENERGY);
    // time must be divided by 1_000_000_000, since 1 second under nanosleep is
    // 1 billion
    energy_value = ((energy - prev_energy) * 1000000000) /
                   (double)(TIME_BETWEEN_MEASUREMENTS);

    // Evaluate whether workload should execute, or throttle
    if (energy_value >= power_limit) {
      // Switch to throttling
      atomic_store(&work, throttle);
    } else {
      // Allow threads to resume execution
      atomic_store(&work, workload);
    }

    // Store measurement
    fprintf(output_file, "%.15f\n", energy_value);

    // Save current
    prev_energy = energy;
  }

  // Clean up
  fclose(output_file);
  return 0;
}

int main(int argc, char *argv[]) {
  // Check arguments
  if (argc != 5) {
    fprintf(stderr,
            "Wrong Input! Enter: %s <ntasks> <samples> <outer> <power_limit>\n",
            argv[0]);
    exit(EXIT_FAILURE);
  }

  // Read in args
  int ntasks;
  struct args_t arg;
  int outer;
  sscanf(argv[1], "%d", &ntasks);
  if (ntasks < 0) {
    fprintf(stderr, "ntasks cannot be negative!\n");
    exit(1);
  }
  sscanf(argv[2], "%" PRIu64, &(arg.iters));
  sscanf(argv[3], "%d", &outer);
  if (outer < 0) {
    fprintf(stderr, "outer cannot be negative!\n");
    exit(1);
  }

  sscanf(argv[4], "%lf", &power_limit);
  if (power_limit < 0) {
    fprintf(stderr, "power_limit cannot be negative!\n");
    exit(1);
  }

  // Open the selector file
  FILE *selectors_file = fopen("input.txt", "r");
  if (selectors_file == NULL)
    perror("fopen error");

  // Read the selectors file line by line
  int num_selectors = 0;
  int selectors[100];
  size_t len = 0;
  ssize_t read = 0;
  char *line = NULL;
  while ((read = getline(&line, &len, selectors_file)) != -1) {
    if (line[read - 1] == '\n')
      line[--read] = '\0';

    // Read selector
    sscanf(line, "%d", &(selectors[num_selectors]));
    num_selectors += 1;
  }

  // Set the scheduling priority to high to avoid interruptions
  // (lower priorities cause more favorable scheduling, and -20 is the max)
  setpriority(PRIO_PROCESS, 0, -20);

  // Prepare up monitor/attacker
  attacker_core_ID = 0;
  set_rapl_units(attacker_core_ID);
  rapl_msr(attacker_core_ID, PP0_ENERGY);

  // Allocate memory for the threads
  char *tstacks = mmap(NULL, (ntasks + 1) * STACK_SIZE, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

  // Run experiment once for each selector
  for (int i = 0; i < outer * num_selectors; i++) {
    static int instr_file_index = 0;

    // Set alternating selector
    arg.selector = selectors[i % num_selectors];

    // Start victim threads
    int tids[ntasks];
    for (int tnum = 0; tnum < ntasks; tnum++) {
      tids[tnum] = clone(&victim, tstacks + (ntasks - tnum) * STACK_SIZE,
                         CLONE_VM | SIGCHLD, &arg);
    }

    // Start the monitor thread
    clone(&governor, tstacks + (ntasks + 1) * STACK_SIZE, CLONE_VM | SIGCHLD,
          (void *)&arg);

    // Join monitor thread
    wait(NULL);

    // Kill victim threads
    for (int tnum = 0; tnum < ntasks; tnum++) {
      syscall(SYS_tgkill, tids[tnum], tids[tnum], SIGTERM);

      // Need to join o/w the threads remain as zombies
      // https://askubuntu.com/a/427222/1552488
      wait(NULL);
    }

    // Get final average instruction count
    int average_instruction_count_without_nop =
        *instruction_counter_ptr / ntasks;
    // TODO: Write report function (average power from polling, total non-nop
    // instructions executed, etc)
    // report(average_instruction_count_without_nop);
    char output_instruction_filename[64];
    sprintf(output_instruction_filename,
            "./out/all_%02d_%06d_instruction_count_avg.out", arg.selector,
            instr_file_index);
    instr_file_index += 1;

    // Prepare output file
    FILE *output_file_instructions =
        fopen((char *)output_instruction_filename, "w");
    if (output_file_instructions == NULL) {
      perror("output file (instructions)");
    }

    fprintf(output_file_instructions, "%d\n",
            average_instruction_count_without_nop);
  }

  // Clean up
  munmap(tstacks, (ntasks + 1) * STACK_SIZE);
}
