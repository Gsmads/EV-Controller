
#include "Inc.h"
#include "hal_uart.h"
#include "util_rom.h"

void printString(const char *s)
{
  while (*s)
    hal_uart_write(*s++);
}


// Print a string stored in PGM-memory
/* Строка лежит в постоянной памяти, поэтому читается через util_rom,
   а не разыменованием: на гарвардской архитектуре это разные адресные
   пространства (ADR-0021). */
void printRomString(const char *s)
{
  char c;
  while ((c = (char)util_rom_read_u8(s++)))
    hal_uart_write(c);
}


// void printIntegerInBase(unsigned long n, unsigned long base)
// {
// 	unsigned char buf[8 * sizeof(long)]; // Assumes 8-bit chars.
// 	unsigned long i = 0;
//
// 	if (n == 0) {
// 		hal_uart_write('0');
// 		return;
// 	}
//
// 	while (n > 0) {
// 		buf[i++] = n % base;
// 		n /= base;
// 	}
//
// 	for (; i > 0; i--)
// 		hal_uart_write(buf[i - 1] < 10 ?
// 			'0' + buf[i - 1] :
// 			'A' + buf[i - 1] - 10);
// }


// Prints an uint8 variable in base 10.
void print_uint8_base10(uint8_t n)
{
  uint8_t digit_a = 0;
  uint8_t digit_b = 0;
  if (n >= 100) { // 100-255
    digit_a = '0' + n % 10;
    n /= 10;
  }
  if (n >= 10) { // 10-99
    digit_b = '0' + n % 10;
    n /= 10;
  }
  hal_uart_write('0' + n);
  if (digit_b) { hal_uart_write(digit_b); }
  if (digit_a) { hal_uart_write(digit_a); }
}


// Prints an uint8 variable in base 2 with desired number of desired digits.
void print_uint8_base2_ndigit(uint8_t n, uint8_t digits) {
  unsigned char buf[digits];
  uint8_t i = 0;

  for (; i < digits; i++) {
      buf[i] = n % 2 ;
      n /= 2;
  }

  for (; i > 0; i--)
      hal_uart_write('0' + buf[i - 1]);
}


void print_uint32_base10(uint32_t n)
{
  if (n == 0) {
    hal_uart_write('0');
    return;
  }

  unsigned char buf[10];
  uint8_t i = 0;

  while (n > 0) {
    buf[i++] = n % 10;
    n /= 10;
  }

  for (; i > 0; i--)
    hal_uart_write('0' + buf[i-1]);
}


void printInteger(long n)
{
  if (n < 0) {
    hal_uart_write('-');
    print_uint32_base10(-n);
  } else {
    print_uint32_base10(n);
  }
}


// Convert float to string by immediately converting to a long integer, which contains
// more digits than a float. Number of decimal places, which are tracked by a counter,
// may be set by the user. The integer is then efficiently converted to a string.
// NOTE: AVR '%' and '/' integer operations are very efficient. Bitshifting speed-up
// techniques are actually just slightly slower. Found this out the hard way.
void printFloat(float n, uint8_t decimal_places)
{
  if (n < 0) {
    hal_uart_write('-');
    n = -n;
  }

  uint8_t decimals = decimal_places;
  while (decimals >= 2) { // Quickly convert values expected to be E0 to E-4.
    n *= 100;
    decimals -= 2;
  }
  if (decimals) { n *= 10; }
  n += 0.5; // Add rounding factor. Ensures carryover through entire value.

  // Generate digits backwards and store in string.
  unsigned char buf[13];
  uint8_t i = 0;
  uint32_t a = (long)n;
  while(a > 0) {
    buf[i++] = (a % 10) + '0'; // Get digit
    a /= 10;
  }
  while (i < decimal_places) {
     buf[i++] = '0'; // Fill in zeros to decimal point for (n < 1)
  }
  if (i == decimal_places) { // Fill in leading zero, if needed.
    buf[i++] = '0';
  }

  // Print the generated string.
  for (; i > 0; i--) {
    if (i == decimal_places) { hal_uart_write('.'); } // Insert decimal point in right place.
    hal_uart_write(buf[i-1]);
  }
}


// Floating value printing handlers for special variables types used in Grbl and are defined
// in the config.h.
//  - CoordValue: Handles all position or coordinate values in inches or mm reporting.
//  - RateValue: Handles feed rate and current velocity in inches or mm reporting.
/*void printFloat_CoordValue(float n) {
  if (bit_istrue(settings.flags,BITFLAG_REPORT_INCHES)) {
    printFloat(n*INCH_PER_MM,N_DECIMAL_COORDVALUE_INCH);
  } else {
    printFloat(n,N_DECIMAL_COORDVALUE_MM);
  }
}

void printFloat_RateValue(float n) {
  if (bit_istrue(settings.flags,BITFLAG_REPORT_INCHES)) {
    printFloat(n*INCH_PER_MM,N_DECIMAL_RATEVALUE_INCH);
  } else {
    printFloat(n,N_DECIMAL_RATEVALUE_MM);
  }
}

// Debug tool to print free memory in bytes at the called point.
// NOTE: Keep commented unless using. Part of this function always gets compiled in.
// void printFreeMemory()
// {
//   extern int __heap_start, *__brkval;
//   uint16_t free;  // Up to 64k values.
//   free = (int) &free - (__brkval == 0 ? (int) &__heap_start : (int) __brkval);
//   printInteger((int32_t)free);
//   printString(" ");
// }

*/
