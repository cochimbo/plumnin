/* Forced into shine's sources with MSVC (/FI): two of its variables carry GCC's
   __attribute__((unused)), which MSVC does not know, and a function-like macro cannot be
   defined on its command line. */
#define __attribute__(x)
