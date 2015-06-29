#pragma once

#include <stdint.h>

void smartos_init(char *kernel_path, char *initrd_path, char *cmdline);
uint64_t smartos_load(void);
