#ifndef LOG_H
#define LOG_H

#include <stdio.h>

// Simple logging macros for testing
#define LOG_I(component, ...) printf("[INFO] " __VA_ARGS__)
#define LOG_W(component, ...) printf("[WARN] " __VA_ARGS__)
#define LOG_E(component, ...) printf("[ERROR] " __VA_ARGS__)
#define LOG_D(component, ...) printf("[DEBUG] " __VA_ARGS__)

#endif