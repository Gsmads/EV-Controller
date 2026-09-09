#include "Arduino.h"

void printString(const char *s);
void printPgmString(const char *s);
void print_uint8_base10(uint8_t n);
void print_uint8_base2_ndigit(uint8_t n, uint8_t digits);
void print_uint32_base10(uint32_t n);
void printInteger(long n);
void printFloat(float n, uint8_t decimal_places);