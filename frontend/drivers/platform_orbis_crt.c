/*  RetroArch - A frontend for libretro.
 *
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

/* Process entry for the OpenOrbis (v0.5) build, replacing the _start of
 * the toolchain's crt1.o (whose process parameters are still linked in,
 * see Makefile.ps4).
 *
 * The toolchain's _start reads argc/argv from the stack, but the PS4
 * kernel hands the argument block over in rdi (the FreeBSD amd64 entry
 * convention). A process started from the shell happens to find zeros on
 * its stack; one started with sceSystemServiceLoadExec, as a core is,
 * finds neither its arguments nor a usable environment there.
 *
 * musl's __libc_start_main expects argv, a NULL, the environment, a NULL
 * and the auxiliary vector; the kernel's block is not guaranteed to carry
 * the latter two, so the arguments are copied into a block that has an
 * empty environment and an empty auxiliary vector. */

#include <stddef.h>

#define ORBIS_CRT_MAX_ARGS 64

extern int main(int argc, char **argv, char **envp);
extern int __libc_start_main(int (*main)(int, char **, char **), int argc,
      char **argv, void (*init)(void), void (*fini)(void),
      void (*ldso)(void));
extern void _init(void);
extern void _fini(void);

/* argv[ORBIS_CRT_MAX_ARGS], NULL, envp NULL, auxv AT_NULL pair */
static char *orbis_crt_block[ORBIS_CRT_MAX_ARGS + 4];

void orbis_crt_start(const long *args, const long *stack);

__asm__(
   ".text\n"
   ".global _start\n"
   ".type _start, @function\n"
   "_start:\n"
   "   xorq  %rbp, %rbp\n"
   "   movq  %rsp, %rsi\n"
   "   andq  $-16, %rsp\n"
   "   call  orbis_crt_start\n"
   "   hlt\n");

static int orbis_crt_argc(const long *block)
{
   long argc;
   int i;

   if (!block || ((unsigned long)block & 7))
      return -1;
   argc = block[0] & 0xffffffff;
   if (argc < 0 || argc > ORBIS_CRT_MAX_ARGS)
      return -1;
   for (i = 0; i < argc; i++)
      if (!block[1 + i])
         return -1;
   return (int)argc;
}

void orbis_crt_start(const long *args, const long *stack)
{
   const long *block = args;
   int argc          = orbis_crt_argc(block);
   int i;

   /* Fall back to the stack, as the toolchain's _start does */
   if (argc < 0)
   {
      block = stack;
      argc  = orbis_crt_argc(block);
      if (argc < 0)
         argc = 0;
   }

   for (i = 0; i < argc; i++)
      orbis_crt_block[i] = (char*)block[1 + i];
   /* The rest of the block stays zero: argv terminator, empty
    * environment, AT_NULL. */

   __libc_start_main(main, argc, orbis_crt_block, _init, _fini, 0);
}
