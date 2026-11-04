#pragma once

#include <stdint.h>

// Apply the function overrides in a kernel or HAL image (Windows 11 24H2 and later), see funcoverride.cpp
void apply_function_overrides(uint8_t* image, uint32_t size);

// Pages (in bytes) the kernel wants reserved for its KSCP section; set while the kernel and HAL are loaded
extern uint32_t kernel_scp_reserve_size;
