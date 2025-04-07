#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "util/freq-utils.h"
#include "util/rapl-utils.h"
#include "util/util.h"

// attacker core; 0 is package level
volatile static int attacker_core_ID;

// constant for power-based instruction throttling
volatile static double power_limit = 0.1;

// energy_value defines the memory location of the energy value, pointer is a
// static pointer to that memory location that we can reference
volatile double energy_value = 0;
volatile static double *current_energy = &energy_value;

// again, we define the memory location and a pointer to that location that we
// can access
volatile uint32_t instruction_counter;
volatile static uint32_t *instruction_counter_ptr = &instruction_counter;

#define TIME_BETWEEN_MEASUREMENTS 1000000L // 1 millisecond

#define STACK_SIZE 8192

struct args_t {
  uint64_t iters;
  int selector;
};

static __attribute__((noinline)) int victim(void *varg) {
  struct args_t *arg = varg;
  uint64_t my_uint64 = 0x0000FFFFFFFF0000;
  uint64_t count = (uint64_t)arg->selector;

  while (1) {
    if (*current_energy < power_limit) {
      asm volatile(".align 64\t\n"

                   "shlx %[count], %[value], %%rbx\n\t"
                   "shlx %[count], %[value], %%rcx\n\t"
                   "shrx %[count], %[value], %%rsi\n\t"
                   "shrx %[count], %[value], %%rdi\n\t"
                   "shlx %[count], %[value], %%r8\n\t"
                   "shlx %[count], %[value], %%r9\n\t"
                   "shrx %[count], %[value], %%r10\n\t"
                   "shrx %[count], %[value], %%r11\n\t"
                   "shlx %[count], %[value], %%r12\n\t"
                   "shlx %[count], %[value], %%r13\n\t"

                   "shrx %[count], %[value], %%rbx\n\t"
                   "shrx %[count], %[value], %%rcx\n\t"
                   "shlx %[count], %[value], %%rsi\n\t"
                   "shlx %[count], %[value], %%rdi\n\t"
                   "shrx %[count], %[value], %%r8\n\t"
                   "shrx %[count], %[value], %%r9\n\t"
                   "shlx %[count], %[value], %%r10\n\t"
                   "shlx %[count], %[value], %%r11\n\t"
                   "shrx %[count], %[value], %%r12\n\t"
                   "shrx %[count], %[value], %%r13\n\t"
                   "lock addl $11, %[instruction_count]"

                   : [instruction_count] "+m"(*instruction_counter_ptr)
                   : [count] "r"(count), [value] "r"(my_uint64)
                   : "rbx", "rcx", "rsi", "rdi", "r8", "r9", "r10", "r11",
                     "r12", "r13");
    } else {
      asm volatile("nop\n\t"
                   "nop\n\t"
                   "nop\n\t"
                   "nop\n\t"
                   "nop\n\t"
                   "nop\n\t"
                   "nop\n\t"
                   "nop\n\t"
                   "nop\n\t"
                   "nop\n\t"
                   "nop\n\t"
                   "nop\n\t"
                   "nop\n\t"
                   "nop\n\t"
                   "nop\n\t"
                   "nop\n\t"
                   "nop\n\t"
                   "nop\n\t"
                   "nop\n\t"
                   "nop\n\t"
                   "nop\n\t"
                   "nop\n\t"
                   "nop\n\t"
                   "nop\n\t"
                   "nop\n\t"
                   "nop\n\t"
                   "nop\n\t"
                   "nop\n\t"
                   "nop\n\t"
                   "nop\n\t"
                   "nop\n\t"
                   "nop\n\t"
                   :
                   :
                   :);
    }
  }

  return 0;
}

// Collects traces
static __attribute__((noinline)) int monitor(void *in) {
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
    energy_value = energy - prev_energy;

    // Store measurement
    fprintf(output_file, "%.15f\n", *current_energy);

    // Save current
    prev_energy = energy;
  }

  // Clean up
  fclose(output_file);
  return 0;
}

int main(int argc, char *argv[]) {
  // Check arguments
  if (argc != 4) {
    fprintf(stderr, "Wrong Input! Enter: %s <ntasks> <samples> <outer>\n",
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

    // Set alternating selector
    arg.selector = selectors[i % num_selectors];

    // Start victim threads
    int tids[ntasks];
    for (int tnum = 0; tnum < ntasks; tnum++) {
      tids[tnum] = clone(&victim, tstacks + (ntasks - tnum) * STACK_SIZE,
                         CLONE_VM | SIGCHLD, &arg);
    }

    // Start the monitor thread
    clone(&monitor, tstacks + (ntasks + 1) * STACK_SIZE, CLONE_VM | SIGCHLD,
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
  }

  // Get final average instruction count
  int average_instruction_count_without_nop = *instruction_counter_ptr / ntasks;
  // TODO: Write report function (average power from polling, total non-nop
  // instructions executed, etc)
  // report(average_instruction_count_without_nop);

  // Clean up
  munmap(tstacks, (ntasks + 1) * STACK_SIZE);
}
