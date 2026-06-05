/* split.c -- split a file into pieces.
   Copyright (C) 1988-2026 Free Software Foundation, Inc.

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <https://www.gnu.org/licenses/>.  */

/* By tege@sics.se, with rms.

   TODO:
   * support -p REGEX as in BSD's split.
   * support --suppress-matched as in csplit.  */
#include <config.h>

#include <stdio.h>
#include <getopt.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <spawn.h>
#include <endian.h>

#ifdef USE_JCCERR_CDC
# include <cpuid.h>
#endif

#include "system.h"
#include "alignalloc.h"
#include "assure.h"
#include "fadvise.h"
#include "fd-reopen.h"
#include "fcntl--.h"
#include "full-write.h"
#include "ioblksize.h"
#include "quote.h"
#include "randperm.h"
#include "sig2str.h"
#include "sys-limits.h"
#include "temp-stream.h"
#include "unistd--.h"
#include "xbinary-io.h"
#include "xdectoint.h"
#include "xstrtol.h"
#include "split_cdc.h"

/* The official name of this program (e.g., no 'g' prefix).  */
#define PROGRAM_NAME "split"

#define AUTHORS \
  proper_name_lite ("Torbjorn Granlund", "Torbj\303\266rn Granlund"), \
  proper_name ("Richard M. Stallman")

#define ARRAY_SIZE(a) (sizeof (a) / sizeof ((a)[0]))

enum { N_CHARS = UCHAR_MAX + 1 };

/* Shell command to filter through, instead of creating files.  */
static char const *filter_command;

/* Process ID of the filter.  */
static pid_t filter_pid;

/* Array of open pipes.  */
static int *open_pipes;
static idx_t open_pipes_alloc;
static int n_open_pipes;

/* Whether SIGPIPE has the default action, when --filter is used.  */
static bool default_SIGPIPE;

/* Base name of output files.  */
static char const *outbase = "x";

/* Name of output files.  */
static char *outfile;

/* Pointer to the end of the prefix in OUTFILE.
   Suffixes are inserted here.  */
static char *outfile_mid;

/* Generate new suffix when suffixes are exhausted.  */
static bool suffix_auto = true;

/* Length of OUTFILE's suffix.  */
static idx_t suffix_length;

/* Alphabet of characters to use in suffix.  */
static char const *suffix_alphabet = "abcdefghijklmnopqrstuvwxyz";

/* Numerical suffix start value.  */
static char const *numeric_suffix_start;

/* Additional suffix to append to output file names.  */
static char const *additional_suffix;

/* Name of input file.  May be "-".  */
static char const *infile = "-";

/* stat buf for input file.  */
static struct stat in_stat_buf;

/* Descriptor on which output file is open.  */
static int output_desc = -1;

/* If true, print a diagnostic on standard error just before each
   output file is opened. */
static bool verbose;

static bool debug;

/* If true, don't generate zero length output files. */
static bool elide_empty_files;

/* If true, in round robin mode, immediately copy
   input to output, which is much slower, so disabled by default.  */
static bool unbuffered;

/* The character marking end of line.  Defaults to \n below.  */
static int eolchar = -1;

/* Lookup table mapping u8 to u32|u64 for Cdc_type functions.  */
void const *cdc_table;

/* Precomputing window-dependent unbuz table speeds BUZHash up measurably.
   buz32: -35% cycles, buz64: -26% cycles @ Intel Core i7-6600U (Skylake) */
void const *unbuz_table;

/* Some assertions are measurably expensive to check.  NDEBUG is nice idea, but
   some downstreams keep NDEBUG unset, so user should not pay for tests.  */
static bool const TEST =
#if defined DEBUG_EXPENSIVE
    true
#else
    false
#endif
    ;

/* The split mode to use.  */
enum Split_type
{
  type_undef, type_bytes, type_byteslines, type_lines, type_digits,
  type_bytes_cdc, type_byteslines_cdc,
  type_chunk_bytes, type_chunk_lines, type_rr
};

enum Cdc_type
{
  cdc_buz32, cdc_buz64, cdc_gear32, cdc_gear64,
  cdc_undef = -1 /* undef is the last one */
};

static char const *cdc_names[] = { "buz32", "buz64", "gear32", "gear64" };
static cdchash_fn const cdc_hash[] = { buz32, buz64, gear32, gear64 };
static cdcfind_fn const cdc_find[]
    = { buz32_find, buz64_find, gear32_rawfind, gear64_rawfind };
#ifdef USE_JCCERR_CDC
static cdcfind_fn const cdc_find_jccerr[]
    = { buz32_find_jccerr, buz64_find_jccerr, gear32_rawfind_jccerr,
        gear64_rawfind_jccerr };
#endif
/* BUZHash default follows default value used by BorgBackup.  */
static int const cdc_window_default[] = { 4095, 4095, 32, 64 };
static int const cdc_window_min[] = { 4, 8, 32, 64 };

static bool
iscdc (enum Split_type type)
{
  return type == type_bytes_cdc || type == type_byteslines_cdc;
}

static bool
cdc_isbuz (enum Cdc_type hash)
{
  return hash == cdc_buz32 || hash == cdc_buz64;
}

static bool
cdc_isgear (enum Cdc_type hash)
{
  return hash == cdc_gear32 || hash == cdc_gear64;
}

static bool
cdc_is64 (enum Cdc_type hash)
{
  return hash == cdc_gear64 || hash == cdc_buz64;
}

static bool
cdc_is32 (enum Cdc_type hash)
{
  return hash == cdc_gear32 || hash == cdc_buz32;
}

#ifdef USE_JCCERR_CDC
static bool
is_genuine_intel (void)
{
  unsigned int eax, ebx, ecx, edx;
  return __get_cpuid (0, &eax, &ebx, &ecx, &edx) && ebx == signature_INTEL_ebx
         && edx == signature_INTEL_edx && ecx == signature_INTEL_ecx;
}

static bool
is_intel_jcc_erratum_affected (void)
{
  if (!is_genuine_intel ())
    return false;

  unsigned int eax, ebx, ecx, edx;
  if (!__get_cpuid (1, &eax, &ebx, &ecx, &edx))
    return false; /* too ancient */

  unsigned int const family = (eax >> 8) & 0x0Fu;
  if (family != 6)
    return false; /* Extended Family ID doesn't matter, only =6 is affected */

  unsigned int const model = (eax >> 4) & 0x0Fu;
  unsigned int const extended_model = (eax >> 16) & 0x0Fu;
  unsigned int const actual_model
      = (assure (family == 6), model + (extended_model << 4));
  unsigned int const stepping = eax & 0x0Fu;
  unsigned int const processor = (actual_model << 4) + stepping;
  /* Affected processors as published by Intel in BCP #660236.
     Affected microarchitectures are Skylake, Kaby Lake, Amber Lake,
     Whiskey Lake, Coffee Lake, Cascade Lake, Comet Lake.  */
  switch (processor)
    {
    case 0x4E3:
    case 0x554:
    case 0x557:
    case 0x5E3:
    case 0x8E9:
    case 0x8EA:
    case 0x8EB:
    case 0x8EC:
    case 0x9E9:
    case 0x9EA:
    case 0x9EB:
    case 0x9ED:
    case 0xA60:
    case 0xAEA:
      return true;
    }
  return false;
}
#endif

static cdcfind_fn const *
get_cdc_find (void)
{
#ifdef USE_JCCERR_CDC
  const bool affected = is_intel_jcc_erratum_affected ();
  if (debug)
    error (0, 0,
           (affected ? _("using Intel Jcc erratum mitigation")
                     : _("not affected by Intel Jcc erratum")));
  return affected ? cdc_find_jccerr : cdc_find;
#else
  return cdc_find;
#endif
}

/* TODO: should getcachelinesize() be moved next to getpagesize() ?  */
static idx_t
getcachelinesize (void)
{
#ifdef LEVEL1_DCACHE_LINESIZE
  idx_t cacheline_size = sysconf (LEVEL1_DCACHE_LINESIZE);
#else
  idx_t cacheline_size = -1;
#endif
  if (cacheline_size < 0)
    cacheline_size = CDC_TABLE_DEFAULT_ALIGNAS;
  return cacheline_size;
}

/* For long options that have no equivalent short option, use a
   non-character as a pseudo short option, starting with CHAR_MAX + 1.  */
enum
{
  VERBOSE_OPTION = CHAR_MAX + 1,
  FILTER_OPTION,
  IO_BLKSIZE_OPTION,
  ADDITIONAL_SUFFIX_OPTION,
  RANDOM_SOURCE_OPTION,
  DEBUG_PROGRAM_OPTION
};

static struct option const longopts[] =
{
  {"bytes", required_argument, NULL, 'b'},
  {"lines", required_argument, NULL, 'l'},
  {"line-bytes", required_argument, NULL, 'C'},
  {"number", required_argument, NULL, 'n'},
  {"elide-empty-files", no_argument, NULL, 'e'},
  {"unbuffered", no_argument, NULL, 'u'},
  {"suffix-length", required_argument, NULL, 'a'},
  {"additional-suffix", required_argument, NULL,
   ADDITIONAL_SUFFIX_OPTION},
  {"numeric-suffixes", optional_argument, NULL, 'd'},
  {"hex-suffixes", optional_argument, NULL, 'x'},
  {"filter", required_argument, NULL, FILTER_OPTION},
  {"random-source", required_argument, NULL,
   RANDOM_SOURCE_OPTION},
  {"verbose", no_argument, NULL, VERBOSE_OPTION},
  {"debug", no_argument, NULL, DEBUG_PROGRAM_OPTION},
  {"separator", required_argument, NULL, 't'},
  {"-io-blksize", required_argument, NULL,
   IO_BLKSIZE_OPTION}, /* do not document */
  {GETOPT_HELP_OPTION_DECL},
  {GETOPT_VERSION_OPTION_DECL},
  {NULL, 0, NULL, 0}
};

/* Return true if the errno value, ERR, is ignorable.  */
static inline bool
ignorable (int err)
{
  return filter_command && err == EPIPE;
}

static void
set_suffix_length (intmax_t n_units, enum Split_type split_type)
{
#define DEFAULT_SUFFIX_LENGTH 2

  int suffix_length_needed = 0;

  /* The suffix auto length feature is incompatible with
     a user specified start value as the generated suffixes
     are not all consecutive.  */
  if (numeric_suffix_start)
    suffix_auto = false;

  /* Auto-calculate the suffix length if the number of files is given.  */
  if (split_type == type_chunk_bytes || split_type == type_chunk_lines
      || split_type == type_rr)
    {
      intmax_t n_units_end = n_units - 1;
      if (numeric_suffix_start)
        {
          intmax_t n_start;
          strtol_error e = xstrtoimax (numeric_suffix_start, NULL, 10,
                                       &n_start, "");
          if (e == LONGINT_OK && n_start < n_units)
            {
              /* Restrict auto adjustment so we don't keep
                 incrementing a suffix size arbitrarily,
                 as that would break sort order for files
                 generated from multiple split runs.  */
              if (ckd_add (&n_units_end, n_units_end, n_start))
                n_units_end = INTMAX_MAX;
            }

        }
      idx_t alphabet_len = strlen (suffix_alphabet);
      do
        suffix_length_needed++;
      while (n_units_end /= alphabet_len);

      suffix_auto = false;
    }

  if (suffix_length)            /* set by user */
    {
      if (suffix_length < suffix_length_needed)
        error (EXIT_FAILURE, 0,
               _("the suffix length needs to be at least %d"),
               suffix_length_needed);
      suffix_auto = false;
      return;
    }
  else
    suffix_length = MAX (DEFAULT_SUFFIX_LENGTH, suffix_length_needed);
}

void
usage (int status)
{
  if (status != EXIT_SUCCESS)
    emit_try_help ();
  else
    {
      printf (_("\
Usage: %s [OPTION]... [FILE [PREFIX]]\n\
"),
              program_name);
      fputs (_("\
Output pieces of FILE to PREFIXaa, PREFIXab, ...;\n\
default size is 1000 lines, and default PREFIX is 'x'.\n\
"), stdout);

      emit_stdin_note ();
      emit_mandatory_arg_note ();

      oprintf (_("\
  -a, --suffix-length=N\n\
         generate suffixes of length N (default %d)\n\
"), DEFAULT_SUFFIX_LENGTH);
      oputs (_("\
      --additional-suffix=SUFFIX\n\
         append an additional SUFFIX to file names\n\
"));
      oputs (_("\
  -b, --bytes=SIZE\n\
         put SIZE bytes per output file; see explanation below\n\
"));
      oputs (_("\
  -C, --line-bytes=SIZE\n\
         put at most SIZE bytes of records per output file\n\
"));
      oputs (_("\
  -d\n\
         use numeric suffixes starting at 0, not alphabetic\n\
"));
      oputs (_("\
      --numeric-suffixes[=FROM]\n\
         same as -d, but allow setting the start value\n\
"));
      oputs (_("\
  -x\n\
         use hex suffixes starting at 0, not alphabetic\n\
"));
      oputs (_("\
      --hex-suffixes[=FROM]\n\
         same as -x, but allow setting the start value\n\
"));
      oputs (_("\
  -e, --elide-empty-files\n\
         do not generate empty output files with '-n'\n\
"));
      oputs (_("\
      --filter=COMMAND\n\
         write to shell COMMAND; file name is $FILE\n\
"));
      oputs (_("\
  -l, --lines=NUMBER\n\
         put NUMBER lines/records per output file\n\
"));
      oputs (_("\
  -n, --number=CHUNKS\n\
         generate CHUNKS output files; see explanation below\n\
"));
      oputs (_("\
  -t, --separator=SEP\n\
         use SEP instead of newline as the record separator;\n\
         '\\0' (zero) specifies the NUL character\n\
"));
      oputs (_("\
  -u, --unbuffered\n\
         immediately copy input to output with '-n r/...'\n\
"));
      oputs (_("\
      --random-source=FILE\n\
         get random seed for content-defined chunking from FILE\n\
"));
      oputs (_("\
      --verbose\n\
         print a diagnostic just before each output file is opened\n\
"));
      oputs (_("\
      --debug\n\
         indicate what CDC implementation is used\n\
"));
      oputs (HELP_OPTION_DESCRIPTION);
      oputs (VERSION_OPTION_DESCRIPTION);
      emit_size_note ();
      fputs (_("\n\
SIZE may also be:\n\
  N            N bytes with optional unit\n\
  HASH/N       N bytes on average using HASH for content-defined chunking,\n\
               HASH may be gear32, gear64, buz32 and buz64\n\
  HASH[W]/N    N bytes on average while hashing sliding window of W bytes\n\
  HASH/N/K     N bytes on average but K bytes at most\n\
  HASH[W]/N/K  like HASH[W]/N and HASH/N/K combined\n\
"), stdout);
      fputs (_("\n\
CHUNKS may be:\n\
  N       split into N files based on size of input\n\
  K/N     output Kth of N to standard output\n\
  l/N     split into N files without splitting lines/records\n\
  l/K/N   output Kth of N to standard output without splitting lines/records\n\
  r/N     like 'l' but use round robin distribution\n\
  r/K/N   likewise but only output Kth of N to standard output\n\
"), stdout);
      fputs (_("\n\
-n (except -nr) will buffer to $TMPDIR, defaulting to /tmp,\n\
if the input size cannot easily be determined.\n\
"), stdout);
      emit_ancillary_info (PROGRAM_NAME);
    }
  exit (status);
}

/* Copy the data in FD to a temporary file, then make that file FD.
   Use BUF, of size BUFSIZE, to copy.  Return the number of
   bytes copied, or -1 (setting errno) on error.  */
static off_t
copy_to_tmpfile (int fd, char *buf, idx_t bufsize)
{
  FILE *tmp;
  if (!temp_stream (&tmp, NULL))
    return -1;
  off_t copied = 0;
  off_t r;

  while (0 < (r = read (fd, buf, bufsize)))
    {
      if (fwrite (buf, 1, r, tmp) != r)
        return -1;
      if (ckd_add (&copied, copied, r))
        {
          errno = EOVERFLOW;
          return -1;
        }
    }

  if (r < 0)
    return r;
  r = dup2 (fileno (tmp), fd);
  if (r < 0)
    return r;
  if (fclose (tmp) < 0)
    return -1;
  return copied;
}

/* Return the number of bytes that can be read from FD with status ST.
   Store up to the first BUFSIZE bytes of the file's data into BUF,
   and advance the file position by the number of bytes read.  On
   input error, set errno and return -1.  */

static off_t
input_file_size (int fd, struct stat const *st, char *buf, idx_t bufsize)
{
  off_t size = 0;
  do
    {
      ssize_t n_read = read (fd, buf + size, bufsize - size);
      if (n_read <= 0)
        return n_read < 0 ? n_read : size;
      size += n_read;
    }
  while (size < bufsize);

  off_t cur, end;
  if ((usable_st_size (st) && st->st_size < size)
      || (cur = lseek (fd, 0, SEEK_CUR)) < 0
      || cur < size /* E.g., /dev/zero on GNU/Linux.  */
      || (end = lseek (fd, 0, SEEK_END)) < 0)
    {
      char *tmpbuf = xmalloc (bufsize);
      end = copy_to_tmpfile (fd, tmpbuf, bufsize);
      free (tmpbuf);
      if (end < 0)
        return end;
      cur = 0;
    }

  if (end == OFF_T_MAX /* E.g., /dev/zero on GNU/Hurd.  */
      || (cur < end && ckd_add (&size, size, end - cur)))
    {
      errno = EOVERFLOW;
      return -1;
    }

  if (cur < end)
    {
      off_t r = lseek (fd, cur, SEEK_SET);
      if (r < 0)
        return r;
    }

  return size;
}

/* Compute the next sequential output file name and store it into the
   string 'outfile'.  */

static void
next_file_name (void)
{
  /* Index in suffix_alphabet of each character in the suffix.  */
  static idx_t *sufindex;
  static idx_t outbase_length;
  static idx_t outfile_length;
  static idx_t addsuf_length;

  if (! outfile)
    {
      bool overflow, widen;

new_name:
      widen = !! outfile_length;

      if (! widen)
        {
          /* Allocate and initialize the first file name.  */

          outbase_length = strlen (outbase);
          addsuf_length = additional_suffix ? strlen (additional_suffix) : 0;
          overflow = ckd_add (&outfile_length, outbase_length + addsuf_length,
                              suffix_length);
        }
      else
        {
          /* Reallocate and initialize a new wider file name.
             We do this by subsuming the unchanging part of
             the generated suffix into the prefix (base), and
             reinitializing the now one longer suffix.  */

          overflow = ckd_add (&outfile_length, outfile_length, 2);
          suffix_length++;
        }

      idx_t outfile_size;
      overflow |= ckd_add (&outfile_size, outfile_length, 1);
      if (overflow)
        xalloc_die ();
      outfile = xirealloc (outfile, outfile_size);

      if (! widen)
        memcpy (outfile, outbase, outbase_length);
      else
        {
          /* Append the last alphabet character to the file name prefix.  */
          outfile[outbase_length] = suffix_alphabet[sufindex[0]];
          outbase_length++;
        }

      outfile_mid = outfile + outbase_length;
      memset (outfile_mid, suffix_alphabet[0], suffix_length);
      if (additional_suffix)
        memcpy (outfile_mid + suffix_length, additional_suffix, addsuf_length);
      outfile[outfile_length] = 0;

      free (sufindex);
      sufindex = xicalloc (suffix_length, sizeof *sufindex);

      if (numeric_suffix_start)
        {
          affirm (! widen);

          /* Update the output file name.  */
          idx_t i = strlen (numeric_suffix_start);
          memcpy (outfile_mid + suffix_length - i, numeric_suffix_start, i);

          /* Update the suffix index.  */
          idx_t *sufindex_end = sufindex + suffix_length;
          while (i-- != 0)
            *--sufindex_end = numeric_suffix_start[i] - '0';
        }

#if ! _POSIX_NO_TRUNC && HAVE_PATHCONF && defined _PC_NAME_MAX
      /* POSIX requires that if the output file name is too long for
         its directory, 'split' must fail without creating any files.
         This must be checked for explicitly on operating systems that
         silently truncate file names.  */
      {
        char *dir = dir_name (outfile);
        long name_max = pathconf (dir, _PC_NAME_MAX);
        if (0 <= name_max && name_max < base_len (last_component (outfile)))
          error (EXIT_FAILURE, ENAMETOOLONG, "%s", quotef (outfile));
        free (dir);
      }
#endif
    }
  else
    {
      /* Increment the suffix in place, if possible.  */

      idx_t i = suffix_length;
      while (i-- != 0)
        {
          sufindex[i]++;
          if (suffix_auto && i == 0 && ! suffix_alphabet[sufindex[0] + 1])
            goto new_name;
          outfile_mid[i] = suffix_alphabet[sufindex[i]];
          if (outfile_mid[i])
            return;
          sufindex[i] = 0;
          outfile_mid[i] = suffix_alphabet[sufindex[i]];
        }
      error (EXIT_FAILURE, 0, _("output file suffixes exhausted"));
    }
}

/* Create or truncate a file.  */

static int
create (char const *name)
{
  if (!filter_command)
    {
      if (verbose)
        fprintf (stdout, _("creating file %s\n"), quoteaf (name));

      int oflags = O_WRONLY | O_CREAT | O_BINARY;
      int fd = open (name, oflags | O_EXCL, MODE_RW_UGO);
      if (0 <= fd || errno != EEXIST)
        return fd;
      fd = open (name, oflags, MODE_RW_UGO);
      if (fd < 0)
        return fd;
      struct stat out_stat_buf;
      if (fstat (fd, &out_stat_buf) != 0)
        error (EXIT_FAILURE, errno, _("failed to stat %s"), quoteaf (name));
      if (psame_inode (&in_stat_buf, &out_stat_buf))
        error (EXIT_FAILURE, 0, _("%s would overwrite input; aborting"),
               quoteaf (name));
      if (ftruncate (fd, 0) < 0
          && (S_ISREG (out_stat_buf.st_mode) || S_TYPEISSHM (&out_stat_buf)))
        error (EXIT_FAILURE, errno, _("%s: error truncating"), quotef (name));

      return fd;
    }
  else
    {
      if (setenv ("FILE", name, 1) != 0)
        error (EXIT_FAILURE, errno,
               _("failed to set FILE environment variable"));
      if (verbose)
        fprintf (stdout, _("executing with FILE=%s\n"), quotef (name));

      int result;
      int fd_pair[2];
      pid_t child_pid;

      posix_spawnattr_t attr;
      posix_spawn_file_actions_t actions;

      sigset_t set;
      sigemptyset (&set);
      if (default_SIGPIPE)
        sigaddset (&set, SIGPIPE);

      if (   (result = posix_spawnattr_init (&attr))
          || (result = posix_spawnattr_setflags (&attr,
                                                 (POSIX_SPAWN_USEVFORK
                                                  | POSIX_SPAWN_SETSIGDEF)))
          || (result = posix_spawnattr_setsigdefault (&attr, &set))
          || (result = posix_spawn_file_actions_init (&actions))
         )
        error (EXIT_FAILURE, result, _("posix_spawn initialization failed"));

      if (pipe (fd_pair) != 0)
        error (EXIT_FAILURE, errno, _("failed to create pipe"));

      /* We have to close any pipes that were opened during an
         earlier call, otherwise this process will be holding a
         write-pipe that will prevent the earlier process from
         reading an EOF on the corresponding read-pipe.  */
      for (int i = 0; i < n_open_pipes; ++i)
        if ((result = posix_spawn_file_actions_addclose (&actions,
                                                         open_pipes[i])))
          break;

      if (   result
          || (result = posix_spawn_file_actions_addclose (&actions, fd_pair[1]))
          || (fd_pair[0] != STDIN_FILENO
              && (   (result = posix_spawn_file_actions_adddup2 (&actions,
                                                                 fd_pair[0],
                                                                 STDIN_FILENO))
                  || (result = posix_spawn_file_actions_addclose (&actions,
                                                                  fd_pair[0]))))
         )
        error (EXIT_FAILURE, result, _("posix_spawn setup failed"));


      char const *shell_prog = getenv ("SHELL");
      if (shell_prog == NULL)
        shell_prog = "/bin/sh";
      char const *const argv[] = { last_component (shell_prog), "-c",
                                   filter_command, NULL };

      result = posix_spawn (&child_pid, shell_prog, &actions, &attr,
                            (char * const *) argv, environ);
      if (result != 0)
        error (EXIT_FAILURE, errno, _("failed to run command: \"%s -c %s\""),
               shell_prog, filter_command);

      posix_spawnattr_destroy (&attr);
      posix_spawn_file_actions_destroy (&actions);

      if (close (fd_pair[0]) != 0)
        error (EXIT_FAILURE, errno, _("failed to close input pipe"));
      filter_pid = child_pid;
      if (n_open_pipes == open_pipes_alloc)
        open_pipes = xpalloc (open_pipes, &open_pipes_alloc, 1,
                              MIN (INT_MAX, IDX_MAX), sizeof *open_pipes);
      open_pipes[n_open_pipes++] = fd_pair[1];
      return fd_pair[1];
    }
}

/* Close the output file, and do any associated cleanup.
   If FP and FD are both specified, they refer to the same open file;
   in this case FP is closed, but FD is still used in cleanup.  */
static void
closeout (FILE *fp, int fd, pid_t pid, char const *name)
{
  if (fp != NULL && fclose (fp) != 0 && ! ignorable (errno))
    error (EXIT_FAILURE, errno, "%s", quotef (name));
  if (fd >= 0)
    {
      if (fp == NULL && close (fd) < 0)
        error (EXIT_FAILURE, errno, "%s", quotef (name));
      for (int j = 0; j < n_open_pipes; ++j)
        {
          if (open_pipes[j] == fd)
            {
              open_pipes[j] = open_pipes[--n_open_pipes];
              break;
            }
        }
    }
  if (pid > 0)
    {
      int wstatus;
      if (waitpid (pid, &wstatus, 0) < 0)
        error (EXIT_FAILURE, errno, _("waiting for child process"));
      else if (WIFSIGNALED (wstatus))
        {
          int sig = WTERMSIG (wstatus);
          if (sig != SIGPIPE)
            {
              char signame[MAX (SIG2STR_MAX, INT_BUFSIZE_BOUND (int))];
              if (sig2str (sig, signame) != 0)
                sprintf (signame, "%d", sig);
              error (sig + 128, 0,
                     _("with FILE=%s, signal %s from command: %s"),
                     quotef (name), signame, filter_command);
            }
        }
      else if (WIFEXITED (wstatus))
        {
          int ex = WEXITSTATUS (wstatus);
          if (ex != 0)
            error (ex, 0, _("with FILE=%s, exit %d from command: %s"),
                   quotef (name), ex, filter_command);
        }
      else
        {
          /* shouldn't happen.  */
          error (EXIT_FAILURE, 0,
                 _("unknown status from command (0x%X)"), wstatus + 0u);
        }
    }
}

/* Write BYTES bytes at BP to an output file.
   If NEW_FILE_FLAG is true, open the next output file.
   Otherwise add to the same output file already in use.
   Return true if successful.  */

static bool
cwrite (bool new_file_flag, char const *bp, idx_t bytes)
{
  if (new_file_flag)
    {
      if (!bp && bytes == 0 && elide_empty_files)
        return true;
      closeout (NULL, output_desc, filter_pid, outfile);
      next_file_name ();
      output_desc = create (outfile);
      if (output_desc < 0)
        error (EXIT_FAILURE, errno, "%s", quotef (outfile));
    }

  if (full_write (output_desc, bp, bytes) == bytes)
    return true;
  else
    {
      if (! ignorable (errno))
        error (EXIT_FAILURE, errno, "%s", quotef (outfile));
      return false;
    }
}

/* Split into pieces of exactly N_BYTES bytes.
   However, the first REM_BYTES pieces should be 1 byte longer.
   Use buffer BUF, whose size is BUFSIZE.
   If INITIAL_READ is nonnegative,
   BUF contains the first INITIAL_READ input bytes.  */

static void
bytes_split (intmax_t n_bytes, intmax_t rem_bytes,
             char *buf, idx_t bufsize, ssize_t initial_read,
             intmax_t max_files)
{
  bool new_file_flag = true;
  bool filter_ok = true;
  intmax_t opened = 0;
  intmax_t to_write = n_bytes + (0 < rem_bytes);
  bool eof = ! to_write;

  while (! eof)
    {
      ssize_t n_read;
      if (0 <= initial_read)
        {
          n_read = initial_read;
          initial_read = -1;
          eof = n_read < bufsize;
        }
      else
        {
          if (! filter_ok
              && 0 <= lseek (STDIN_FILENO, to_write, SEEK_CUR))
            {
              to_write = n_bytes + (opened + 1 < rem_bytes);
              new_file_flag = true;
            }

          n_read = read (STDIN_FILENO, buf, bufsize);
          if (n_read < 0)
            error (EXIT_FAILURE, errno, "%s", quotef (infile));
          eof = n_read == 0;
        }
      char *bp_out = buf;
      while (0 < to_write && to_write <= n_read)
        {
          if (filter_ok || new_file_flag)
            filter_ok = cwrite (new_file_flag, bp_out, to_write);
          opened += new_file_flag;
          new_file_flag = !max_files || (opened < max_files);
          if (! filter_ok && ! new_file_flag)
            {
              /* If filters no longer accepting input, stop reading.  */
              n_read = 0;
              eof = true;
              break;
            }
          bp_out += to_write;
          n_read -= to_write;
          to_write = n_bytes + (opened < rem_bytes);
        }
      if (0 < n_read)
        {
          if (filter_ok || new_file_flag)
            filter_ok = cwrite (new_file_flag, bp_out, n_read);
          opened += new_file_flag;
          new_file_flag = false;
          if (! filter_ok && opened == max_files)
            {
              /* If filters no longer accepting input, stop reading.  */
              break;
            }
          to_write -= n_read;
        }
    }

  /* Ensure NUMBER files are created, which truncates
     any existing files or notifies any consumers on fifos.
     FIXME: Should we do this before EXIT_FAILURE?  */
  while (opened++ < max_files)
    cwrite (true, NULL, 0);
}

/* gcc 13.4.0 @cfarm136 warns with -Werror=suggest-attribute=cold about these
   hash functions.  The warning is just wrong, gcc 14.3.0 and 15.2.0 are happy.
   Moreover, __attribute__((hot)) does not prevent the warning!  */
extern void
buz32 (void *phash_, unsigned char const *p, idx_t count)
{
  uint32_t *const phash = phash_;
  uint32_t const *const buz = cdc_table;
  uint32_t hash = *phash;
  affirm (count >= 1);
  for (unsigned char const *end = p + count; p != end; p++)
    hash = rotl32 (hash, 1) ^ buz[*p];
  *phash = hash;
}

extern void
buz64 (void *phash_, unsigned char const *p, idx_t count)
{
  affirm (count >= 1);
  uint64_t *const phash = phash_;
  uint64_t const *const buz = cdc_table;
  uint64_t hash = *phash;
  for (unsigned char const *end = p + count; p != end; p++)
    hash = rotl64 (hash, 1) ^ buz[*p];
  *phash = hash;
}

extern void
gear32 (void *phash_, unsigned char const *p, idx_t count)
{
  affirm (count >= 1);
  uint32_t *const phash = phash_;
  uint32_t const *const cdc = cdc_table;
  uint32_t hash = *phash;
  for (unsigned char const *end = p + count; p != end; p++)
    hash = (hash << 1) + cdc[*p];
  *phash = hash;
}

extern void
gear64 (void *phash_, unsigned char const *p, idx_t count)
{
  affirm (count >= 1);
  uint64_t *const phash = phash_;
  uint64_t const *const cdc = cdc_table;
  uint64_t hash = *phash;
  for (unsigned char const *end = p + count; p != end; p++)
    hash = (hash << 1) + cdc[*p];
  *phash = hash;
}

static char *
gear32_terminator_alloc (void)
{
  uint32_t const *const cdc = cdc_table;

  /* Find a character in CDC table with the LSB being 0-bit and 1-bit.  */
  unsigned char zero, one;
  idx_t i;
  for (i = 0; i < N_CHARS && (cdc[i] & 1) != 0; i++)
    ;
  zero = i;
  for (i = 0; i < N_CHARS && (cdc[i] & 1) != 1; i++)
    ;
  one = i;
  /* Chance of the LSB being equal across 256 random values is 2^-255.  */
  if ((cdc[zero] & 1) != 0 || (cdc[one] & 1) != 1)
    error (EXIT_FAILURE, 0, _("low-entropy --random-source"));

  uint32_t hash = 0; /* Zero is the target value */
  idx_t const window = sizeof (hash) * CHAR_WIDTH;
  char *t = xmalloc (window);
  for (i = window - 1; i >= 0; i--)
    {
      unsigned char c = (hash & 1) ? one : zero;
      t[i] = (char)c;
      hash = (hash - cdc[c]) >> 1;
    }
  if (TEST)
    assure ((hash = 0, gear32 (&hash, (void *)t, window), hash == 0));
  return t;
}

static char *
gear64_terminator_alloc (void)
{
  uint64_t const *const cdc = cdc_table;

  /* Find a character in CDC table with the LSB being 0-bit and 1-bit.  */
  unsigned char zero, one;
  idx_t i;
  for (i = 0; i < N_CHARS && (cdc[i] & 1) != 0; i++)
    ;
  zero = i;
  for (i = 0; i < N_CHARS && (cdc[i] & 1) != 1; i++)
    ;
  one = i;
  /* Chance of the LSB being equal across 256 random values is 2^-255.  */
  if ((cdc[zero] & 1) != 0 || (cdc[one] & 1) != 1)
    error (EXIT_FAILURE, 0, _("low-entropy --random-source"));

  uint64_t hash = 0; /* Zero is the target value */
  idx_t const window = sizeof (hash) * CHAR_WIDTH;
  char *t = xmalloc (window);
  for (i = window - 1; i >= 0; i--)
    {
      unsigned char c = (hash & 1) ? one : zero;
      t[i] = (char)c;
      hash = (hash - cdc[c]) >> 1;
    }
  if (TEST)
    assure ((hash = 0, gear64 (&hash, (void *)t, window), hash == 0));
  return t;
}

/* Split into pieces of approximately AVGSZ bytes, not larger than MAXSZ bytes,
   using content-defined chunking with rolling HASH running over sliding window
   of WIN bytes.  Use buffer BUF of IOSZ bytes for I/O.  The buffer has WINDOW
   bytes prepended and a trailer of WINDOW bytes for GearHash terminator.  */
static void
bytes_cdc_split (enum Cdc_type const hash, intmax_t const avgsz,
                 intmax_t const maxsz, idx_t const window, char *const buf,
                 idx_t const iosz, bool const do_lines)
{
  static_assert (UINT64_MAX <= UINTMAX_MAX);
  affirm (1 <= window && window <= avgsz && avgsz < maxsz && window <= iosz);
  /* The adjustment matters when AVGSZ is close to WINDOW as the code does not
     run Bernoulli trials against the hashes of the first WINDOW-1 bytes.  */
  intmax_t const avgadj = avgsz - (window - 1);
  /* Following idea of Daniel Lemire's paper "Fast Random Integer Generation
     in an Interval" paper we approximate chunk size with Bernoulli probability
     of 1/AVGSZ moved from floating point [0.0…1.0) domain to u32|u64.  */
  uint32_t const le32 = cdc_is32 (hash) ? UINT32_MAX / avgadj : 0;
  uint64_t const le64 = cdc_is64 (hash) ? UINT64_MAX / avgadj : 0;
  void const *const ple = cdc_is32 (hash)   ? &le32
                          : cdc_is64 (hash) ? &le64
                                            : (affirm (false), NULL);
  /* BUZHash and GearHash handle initial state differently.  GearHash
     completely forgets initial state as the WINDOW bytes pass by.  BUZHash
     continues to rotate bits of the initial state forever as that state is
     never "shifted out".  Initial BUZHash value should be symmetric against
     all barrel shifts: it should be either 0 or ~0, BUZHash becomes dependent
     on offset of the WINDOW modulo UINT_WIDTH otherwise.  */
  uint32_t hash32 = 0;
  uint64_t hash64 = 0;
  void *const phash = cdc_is32 (hash)   ? &hash32
                      : cdc_is64 (hash) ? &hash64
                                        : (affirm (false), NULL);
  /* We can't rely on terminator to be intact in the trailer area right after
     I/O buffer as short read() might return just a few bytes less than IOSZ.
     That's why possible positions of multi-byte terminators might overlap
     during different loop iterations and may overwrite each other.  Also,
     reading file from disk usually returns full IOSZ buffer, but pipe and TCP
     socket behave differently.  E.g. pipe has 64 KiB capacity limit on Linux
     and socket buffer is scaled dynamically.  */
  ssize_t terminator_at = 0;
  char *const terminator
      = (hash == cdc_gear32)   ? gear32_terminator_alloc ()
        : (hash == cdc_gear64) ? gear64_terminator_alloc ()
                               : NULL;
  cdchash_fn const hashcall = cdc_hash[hash];
  cdcfind_fn const findcall = get_cdc_find ()[hash];

  bool new_file_flag = true;
  bool filter_ok = true;
  intmax_t write_at_most = maxsz;
  idx_t to_gulp = window;
  bool search_hash = true;
  ssize_t n_read;

  while ((n_read = read (STDIN_FILENO, buf, iosz)) > 0)
    {
      char *const eob = buf + n_read;
      if (n_read != terminator_at && terminator)
        {
          memcpy (eob, terminator, window);
          terminator_at = n_read;
        }

      /* So, we have some buffer with at least WINDOW bytes of data prepended
         to it.  We need to find few points in the buffer.  The points, where:
         1) HASH of initial WINDOW is ready, all bytes are gulped,
         2) MAXSZ is reached, 3) HASH <= LE, 4) end of buffer resides.  */
      for (char const *start = buf; start != eob;)
        {
          typedef unsigned char const cuchar_t;
          idx_t const startsz = eob - start;
          char const *const max_end
              = write_at_most <= startsz ? start + write_at_most : NULL;
          char const *hash_end = NULL;
          char const *unread = start;

          if (search_hash)
            {
              if (to_gulp)
                {
                  idx_t const gulpable = MIN (startsz, to_gulp);
                  hashcall (phash, (cuchar_t*)start, gulpable);
                  to_gulp -= gulpable;
                  unread += gulpable;
                  if (!to_gulp && (hash32 <= le32 && hash64 <= le64))
                    hash_end = unread;
                }

              if (!hash_end && unread != eob)
                {
                  char const *const le_at = (char const *)findcall (
                      phash, ple, (cuchar_t *)unread, (cuchar_t *)eob, window);
                  if (le_at < eob)
                    {
                      unread = le_at;  /* unread var is used iff (do_lines) */
                      hash_end = le_at + 1;
                    }
                }

              if (TEST && hash_end)
                {
                  void const *const last = hash_end - window;
                  uint64_t h64 = 0;
                  uint32_t h32 = 0;
                  if (phash == &hash64)
                    assure ((hashcall (&h64, last, window), h64 == hash64));
                  else if (phash == &hash32)
                    assure ((hashcall (&h32, last, window), h32 == hash32));
                  else
                    affirm (false);
                }

              if (do_lines && hash_end)
                search_hash = false;
            }

          if (!search_hash)
            {
              char const *const eol = memchr (unread, eolchar, eob - unread);
              hash_end = eol ? eol + 1 : NULL;
            }

          char const *const wrend = (hash_end && max_end)
                                        ? MIN (hash_end, max_end)
                                    : hash_end ? hash_end
                                    : max_end  ? max_end
                                               : eob;
          ssize_t const to_write = wrend - start;
          if (filter_ok || new_file_flag)
            filter_ok = cwrite (new_file_flag, start, to_write);
          start = wrend;
          new_file_flag = (hash_end || max_end);
          if (new_file_flag)
            {
              write_at_most = maxsz;
              hash32 = 0;
              hash64 = 0;
              to_gulp = window;
              search_hash = true;
            }
          else
            {
              write_at_most -= to_write;
            }
        }
      /* BUZHash depends on this window to shift old values out, GearHash
         needs it to feed last WINDOW bytes on overrun in *_rawfind versions
         combined with either short read or cut point close to the beginning
         of the buffer.  Short read might also lead to overlap happening when
         N_READ is less than WINDOW.  */
      memmove (buf - window, eob - window, window);
    }
  if (n_read < 0)
    error (EXIT_FAILURE, errno, "%s", quotef (infile));
  IF_LINT (free (terminator));
}

/* Split into pieces of exactly N_LINES lines.
   Use buffer BUF, whose size is BUFSIZE.  */

static void
lines_split (intmax_t n_lines, char *buf, idx_t bufsize)
{
  ssize_t n_read;
  char *bp, *bp_out, *eob;
  bool new_file_flag = true;
  intmax_t n = 0;

  do
    {
      n_read = read (STDIN_FILENO, buf, bufsize);
      if (n_read < 0)
        error (EXIT_FAILURE, errno, "%s", quotef (infile));
      bp = bp_out = buf;
      eob = bp + n_read;
      *eob = eolchar;
      while (true)
        {
          bp = rawmemchr (bp, eolchar);
          if (bp == eob)
            {
              if (eob != bp_out) /* do not write 0 bytes! */
                {
                  idx_t len = eob - bp_out;
                  cwrite (new_file_flag, bp_out, len);
                  new_file_flag = false;
                }
              break;
            }

          ++bp;
          if (++n >= n_lines)
            {
              cwrite (new_file_flag, bp_out, bp - bp_out);
              bp_out = bp;
              new_file_flag = true;
              n = 0;
            }
        }
    }
  while (n_read);
}

/* Split into pieces that are as large as possible while still not more
   than N_BYTES bytes, and are split on line boundaries except
   where lines longer than N_BYTES bytes occur. */

static void
line_bytes_split (intmax_t n_bytes, char *buf, idx_t bufsize)
{
  ssize_t n_read;
  intmax_t n_out = 0;      /* for each split.  */
  idx_t n_hold = 0;
  char *hold = NULL;        /* for lines > bufsize.  */
  idx_t hold_size = 0;
  bool split_line = false;  /* Whether a \n was output in a split.  */

  do
    {
      n_read = read (STDIN_FILENO, buf, bufsize);
      if (n_read < 0)
        error (EXIT_FAILURE, errno, "%s", quotef (infile));
      idx_t n_left = n_read;
      char *sob = buf;
      while (n_left)
        {
          idx_t split_rest = 0;
          char *eoc = NULL;
          char *eol;

          /* Determine End Of Chunk and/or End of Line,
             which are used below to select what to write or buffer.  */
          if (n_bytes - n_out - n_hold <= n_left)
            {
              /* Have enough for split.  */
              split_rest = n_bytes - n_out - n_hold;
              eoc = sob + split_rest - 1;
              eol = memrchr (sob, eolchar, split_rest);
            }
          else
            eol = memrchr (sob, eolchar, n_left);

          /* Output hold space if possible.  */
          if (n_hold && !(!eol && n_out))
            {
              cwrite (n_out == 0, hold, n_hold);
              n_out += n_hold;
              n_hold = 0;
            }

          /* Output to eol if present.  */
          if (eol)
            {
              split_line = true;
              idx_t n_write = eol - sob + 1;
              cwrite (n_out == 0, sob, n_write);
              n_out += n_write;
              n_left -= n_write;
              sob += n_write;
              if (eoc)
                split_rest -= n_write;
            }

          /* Output to eoc or eob if possible.  */
          if (n_left && !split_line)
            {
              idx_t n_write = eoc ? split_rest : n_left;
              cwrite (n_out == 0, sob, n_write);
              n_out += n_write;
              n_left -= n_write;
              sob += n_write;
              if (eoc)
                split_rest -= n_write;
            }

          /* Update hold if needed.  */
          if ((eoc && split_rest) || (!eoc && n_left))
            {
              idx_t n_buf = eoc ? split_rest : n_left;
              if (hold_size - n_hold < n_buf)
                hold = xpalloc (hold, &hold_size, n_buf - (hold_size - n_hold),
                                -1, sizeof *hold);
              memcpy (hold + n_hold, sob, n_buf);
              n_hold += n_buf;
              n_left -= n_buf;
              sob += n_buf;
            }

          /* Reset for new split.  */
          if (eoc)
            {
              n_out = 0;
              split_line = false;
            }
        }
    }
  while (n_read);

  /* Handle no eol at end of file.  */
  if (n_hold)
    cwrite (n_out == 0, hold, n_hold);

  free (hold);
}

/* -n l/[K/]N: Write lines to files of approximately file size / N.
   The file is partitioned into file size / N sized portions, with the
   last assigned any excess.  If a line _starts_ within a partition
   it is written completely to the corresponding file.  Since lines
   are not split even if they overlap a partition, the files written
   can be larger or smaller than the partition size, and even empty
   if a line is so long as to completely overlap the partition.  */

static void
lines_chunk_split (intmax_t k, intmax_t n, char *buf, idx_t bufsize,
                   ssize_t initial_read, off_t file_size)
{
  affirm (n && k <= n);

  intmax_t rem_bytes = file_size % n;
  off_t chunk_size = file_size / n;
  intmax_t chunk_no = 1;
  off_t chunk_end = chunk_size + (0 < rem_bytes);
  off_t n_written = 0;
  bool new_file_flag = true;
  bool chunk_truncated = false;

  if (k > 1 && 0 < file_size)
    {
      /* Start reading 1 byte before kth chunk of file.  */
      off_t start = (k - 1) * chunk_size + MIN (k - 1, rem_bytes) - 1;
      if (start < initial_read)
        {
          memmove (buf, buf + start, initial_read - start);
          initial_read -= start;
        }
      else
        {
          if (initial_read < start
              && lseek (STDIN_FILENO, start - initial_read, SEEK_CUR) < 0)
            error (EXIT_FAILURE, errno, "%s", quotef (infile));
          initial_read = -1;
        }
      n_written = start;
      chunk_no = k - 1;
      chunk_end = start + 1;
    }

  while (n_written < file_size)
    {
      char *bp = buf, *eob;
      ssize_t n_read;
      if (0 <= initial_read)
        {
          n_read = initial_read;
          initial_read = -1;
        }
      else
        {
          n_read = read (STDIN_FILENO, buf,
                         MIN (bufsize, file_size - n_written));
          if (n_read < 0)
            error (EXIT_FAILURE, errno, "%s", quotef (infile));
        }
      if (n_read == 0)
        break; /* eof.  */
      chunk_truncated = false;
      eob = buf + n_read;

      while (bp != eob)
        {
          idx_t to_write;
          bool next = false;

          /* Begin looking for '\n' at last byte of chunk.  */
          off_t skip = MIN (n_read, MAX (0, chunk_end - 1 - n_written));
          char *bp_out = memchr (bp + skip, eolchar, n_read - skip);
          if (bp_out)
            {
              bp_out++;
              next = true;
            }
          else
            bp_out = eob;
          to_write = bp_out - bp;

          if (k == chunk_no)
            {
              /* We don't use the stdout buffer here since we're writing
                 large chunks from an existing file, so it's more efficient
                 to write out directly.  */
              if (full_write (STDOUT_FILENO, bp, to_write) != to_write)
                write_error ();
            }
          else if (! k)
            cwrite (new_file_flag, bp, to_write);
          n_written += to_write;
          bp += to_write;
          n_read -= to_write;
          new_file_flag = next;

          /* A line could have been so long that it skipped
             entire chunks. So create empty files in that case.  */
          while (next || chunk_end <= n_written)
            {
              if (!next && bp == eob)
                {
                  /* replenish buf, before going to next chunk.  */
                  chunk_truncated = true;
                  break;
                }
              if (k == chunk_no)
                return;
              chunk_end += chunk_size + (chunk_no < rem_bytes);
              chunk_no++;
              if (chunk_end <= n_written)
                {
                  if (! k)
                    cwrite (true, NULL, 0);
                }
              else
                next = false;
            }
        }
    }

  if (chunk_truncated)
    chunk_no++;

  /* Ensure NUMBER files are created, which truncates
     any existing files or notifies any consumers on fifos.
     FIXME: Should we do this before EXIT_FAILURE?  */
  if (!k)
    while (chunk_no++ <= n)
      cwrite (true, NULL, 0);
}

/* -n K/N: Extract Kth of N chunks.  */

static void
bytes_chunk_extract (intmax_t k, intmax_t n, char *buf, idx_t bufsize,
                     ssize_t initial_read, off_t file_size)
{
  off_t start;
  off_t end;

  affirm (0 < k && k <= n);

  start = (k - 1) * (file_size / n) + MIN (k - 1, file_size % n);
  end = k == n ? file_size : k * (file_size / n) + MIN (k, file_size % n);

  if (start < initial_read)
    {
      memmove (buf, buf + start, initial_read - start);
      initial_read -= start;
    }
  else
    {
      if (initial_read < start
          && lseek (STDIN_FILENO, start - initial_read, SEEK_CUR) < 0)
        error (EXIT_FAILURE, errno, "%s", quotef (infile));
      initial_read = -1;
    }

  while (start < end)
    {
      ssize_t n_read;
      if (0 <= initial_read)
        {
          n_read = initial_read;
          initial_read = -1;
        }
      else
        {
          n_read = read (STDIN_FILENO, buf, bufsize);
          if (n_read < 0)
            error (EXIT_FAILURE, errno, "%s", quotef (infile));
        }
      if (n_read == 0)
        break; /* eof.  */
      n_read = MIN (n_read, end - start);
      if (full_write (STDOUT_FILENO, buf, n_read) != n_read
          && ! ignorable (errno))
        error (EXIT_FAILURE, errno, "%s", quotef ("-"));
      start += n_read;
    }
}

typedef struct of_info
{
  char *of_name;
  int ofd;
  FILE *ofile;
  pid_t opid;
} of_t;

enum
{
  OFD_NEW = -1,
  OFD_APPEND = -2
};

/* Rotate file descriptors when we're writing to more output files than we
   have available file descriptors.
   Return whether we came under file resource pressure.
   If so, it's probably best to close each file when finished with it.  */

static bool
ofile_open (of_t *files, idx_t i_check, idx_t nfiles)
{
  bool file_limit = false;

  if (files[i_check].ofd <= OFD_NEW)
    {
      int fd;
      idx_t i_reopen = i_check ? i_check - 1 : nfiles - 1;

      /* Another process could have opened a file in between the calls to
         close and open, so we should keep trying until open succeeds or
         we've closed all of our files.  */
      while (true)
        {
          if (files[i_check].ofd == OFD_NEW)
            fd = create (files[i_check].of_name);
          else /* OFD_APPEND  */
            {
              /* Attempt to append to previously opened file.
                 We use O_NONBLOCK to support writing to fifos,
                 where the other end has closed because of our
                 previous close.  In that case we'll immediately
                 get an error, rather than waiting indefinitely.
                 In specialized cases the consumer can keep reading
                 from the fifo, terminating on conditions in the data
                 itself, or perhaps never in the case of 'tail -f'.
                 I.e., for fifos it is valid to attempt this reopen.

                 We don't handle the filter_command case here, as create()
                 will exit if there are not enough files in that case.
                 I.e., we don't support restarting filters, as that would
                 put too much burden on users specifying --filter commands.  */
              fd = open (files[i_check].of_name,
                         O_WRONLY | O_BINARY | O_APPEND | O_NONBLOCK);
            }

          if (0 <= fd)
            break;

          if (!(errno == EMFILE || errno == ENFILE))
            error (EXIT_FAILURE, errno, "%s", quotef (files[i_check].of_name));

          file_limit = true;

          /* Search backwards for an open file to close.  */
          while (files[i_reopen].ofd < 0)
            {
              i_reopen = i_reopen ? i_reopen - 1 : nfiles - 1;
              /* No more open files to close, exit with E[NM]FILE.  */
              if (i_reopen == i_check)
                error (EXIT_FAILURE, errno, "%s",
                       quotef (files[i_check].of_name));
            }

          if (fclose (files[i_reopen].ofile) != 0)
            error (EXIT_FAILURE, errno, "%s", quotef (files[i_reopen].of_name));
          files[i_reopen].ofile = NULL;
          files[i_reopen].ofd = OFD_APPEND;
        }

      files[i_check].ofd = fd;
      FILE *ofile = fdopen (fd, "a");
      if (!ofile)
        error (EXIT_FAILURE, errno, "%s", quotef (files[i_check].of_name));
      files[i_check].ofile = ofile;
      files[i_check].opid = filter_pid;
      filter_pid = 0;
    }

  return file_limit;
}

/* -n r/[K/]N: Divide file into N chunks in round robin fashion.
   Use BUF of size BUFSIZE for the buffer, and if allocating storage
   put its address into *FILESP to pacify -fsanitize=leak.
   When K == 0, we try to keep the files open in parallel.
   If we run out of file resources, then we revert
   to opening and closing each file for each line.  */

static void
lines_rr (intmax_t k, intmax_t n, char *buf, idx_t bufsize, of_t **filesp)
{
  bool wrapped = false;
  bool wrote = false;
  bool file_limit;
  idx_t i_file;
  of_t *files IF_LINT (= NULL);
  intmax_t line_no;

  if (k)
    line_no = 1;
  else
    {
      if (IDX_MAX < n)
        xalloc_die ();
      files = *filesp = xinmalloc (n, sizeof *files);

      /* Generate output file names. */
      for (i_file = 0; i_file < n; i_file++)
        {
          next_file_name ();
          files[i_file].of_name = xstrdup (outfile);
          files[i_file].ofd = OFD_NEW;
          files[i_file].ofile = NULL;
          files[i_file].opid = 0;
        }
      i_file = 0;
      file_limit = false;
    }

  while (true)
    {
      char *bp = buf, *eob;
      ssize_t n_read = read (STDIN_FILENO, buf, bufsize);
      if (n_read < 0)
        error (EXIT_FAILURE, errno, "%s", quotef (infile));
      else if (n_read == 0)
        break; /* eof.  */
      eob = buf + n_read;

      while (bp != eob)
        {
          idx_t to_write;
          bool next = false;

          /* Find end of line. */
          char *bp_out = memchr (bp, eolchar, eob - bp);
          if (bp_out)
            {
              bp_out++;
              next = true;
            }
          else
            bp_out = eob;
          to_write = bp_out - bp;

          if (k)
            {
              if (line_no == k && unbuffered)
                {
                  if (full_write (STDOUT_FILENO, bp, to_write) != to_write)
                    write_error ();
                }
              else if (line_no == k && fwrite (bp, to_write, 1, stdout) != 1)
                {
                  write_error ();
                }
              if (next)
                line_no = (line_no == n) ? 1 : line_no + 1;
            }
          else
            {
              /* Secure file descriptor. */
              file_limit |= ofile_open (files, i_file, n);
              if (unbuffered)
                {
                  /* Note writing to fd, rather than flushing the FILE gives
                     an 8% performance benefit, due to reduced data copying.  */
                  if (full_write (files[i_file].ofd, bp, to_write) != to_write
                      && ! ignorable (errno))
                    error (EXIT_FAILURE, errno, "%s",
                           quotef (files[i_file].of_name));
                }
              else if (fwrite (bp, to_write, 1, files[i_file].ofile) != 1
                       && ! ignorable (errno))
                error (EXIT_FAILURE, errno, "%s",
                       quotef (files[i_file].of_name));

              if (! ignorable (errno))
                wrote = true;

              if (file_limit)
                {
                  if (fclose (files[i_file].ofile) != 0)
                    error (EXIT_FAILURE, errno, "%s",
                           quotef (files[i_file].of_name));
                  files[i_file].ofile = NULL;
                  files[i_file].ofd = OFD_APPEND;
                }
              if (next && ++i_file == n)
                {
                  wrapped = true;
                  /* If no filters are accepting input, stop reading.  */
                  if (! wrote)
                    goto no_filters;
                  wrote = false;
                  i_file = 0;
                }
            }

          bp = bp_out;
        }
    }

no_filters:
  /* Ensure all files created, so that any existing files are truncated,
     and to signal any waiting fifo consumers.
     Also, close any open file descriptors.
     FIXME: Should we do this before EXIT_FAILURE?  */
  if (!k)
    {
      idx_t ceiling = wrapped ? n : i_file;
      for (i_file = 0; i_file < n; i_file++)
        {
          if (i_file >= ceiling && !elide_empty_files)
            file_limit |= ofile_open (files, i_file, n);
          if (files[i_file].ofd >= 0)
            closeout (files[i_file].ofile, files[i_file].ofd,
                      files[i_file].opid, files[i_file].of_name);
          files[i_file].ofd = OFD_APPEND;
        }
    }
}

#define FAIL_ONLY_ONE_WAY()					\
  do								\
    {								\
      error (0, 0, _("cannot split in more than one way"));	\
      usage (EXIT_FAILURE);					\
    }								\
  while (0)

/* Report a string-to-integer conversion failure MSGID with ARG.  */

static _Noreturn void
strtoint_die (char const *msgid, char const *arg)
{
  error (EXIT_FAILURE, errno == EINVAL ? 0 : errno, "%s: %s",
         gettext (msgid), quote (arg));
}

/* Use OVERFLOW_OK when it is OK to ignore LONGINT_OVERFLOW errors, since the
   extreme value will do the right thing anyway on any practical platform.  */
#define OVERFLOW_OK LONGINT_OVERFLOW

static char const byte_multipliers[] = "bEGKkMmPQRTYZ0";

/* Parse ARG for number of bytes or lines.  The number can be followed
   by MULTIPLIERS, and the resulting value must be positive.
   If the number cannot be parsed, diagnose with MSG.
   Return the number parsed, or an INTMAX_MAX on overflow.  */

static intmax_t
parse_n_units (char const *arg, char const *multipliers, char const *msgid)
{
  intmax_t n;
  if (OVERFLOW_OK < xstrtoimax (arg, NULL, 10, &n, multipliers) || n < 1)
    strtoint_die (msgid, arg);
  return n;
}

/* Parse K/N syntax of chunk options.  */

static void
parse_chunk (intmax_t *k_units, intmax_t *n_units, char const *arg)
{
  char *argend;
  strtol_error e = xstrtoimax (arg, &argend, 10, n_units, "");
  if (e == LONGINT_INVALID_SUFFIX_CHAR && *argend == '/')
    {
      *k_units = *n_units;
      *n_units = parse_n_units (argend + 1, "",
                                N_("invalid number of chunks"));
      if (! (0 < *k_units && *k_units <= *n_units))
        error (EXIT_FAILURE, 0, "%s: %s", _("invalid chunk number"),
               quote_mem (arg, argend - arg));
    }
  else if (! (e <= OVERFLOW_OK && 0 < *n_units))
    strtoint_die (N_("invalid number of chunks"), arg);
}

/* Parse HASH[WINDOW]/AVG/MAX syntax of content-defined chunking SIZE.  */

static enum Cdc_type
parse_cdc (intmax_t *window, intmax_t *avgsz, intmax_t *maxsz, char const *arg)
{
  enum Cdc_type hash = cdc_undef;
  for (int i = 0; i < ARRAY_SIZE (cdc_names); ++i)
    if (STRPREFIX (arg, cdc_names[i]))
      {
        arg += strlen (cdc_names[i]);
        hash = (enum Cdc_type)i;
        break;
      }
  if (hash == cdc_undef)
    error (EXIT_FAILURE, 0, _("unknown rolling hash: %s"), quote (arg));

  if (*arg == '[')
    {
      char *next = NULL;
      arg++; /* skip '[' */
      strtol_error e = xstrtoimax (arg, &next, 10, window, byte_multipliers);
      if (e != LONGINT_INVALID_SUFFIX_CHAR || *next != ']')
        strtoint_die (N_ ("cannot parse hash window"), arg);

      /* Window below hash width makes bad PRF out of BUZHash for sure.  Longer
         window does not guarantee good PRF though.  It's possible to implement
         GearHash over shortened window, but it makes terminator calculation
         trickier and overall utility of reduced-window GearHash is unclear. */
      if (cdc_isgear (hash) && *window != cdc_window_min[hash])
        error (EXIT_FAILURE, 0, _("%s hash window must be %d"),
               cdc_names[hash], cdc_window_min[hash]);
      else if (cdc_isbuz (hash) && *window < cdc_window_min[hash])
        error (EXIT_FAILURE, 0, _("%s hash window must be at least %d"),
               cdc_names[hash], cdc_window_min[hash]);

      arg = next + 1;
    }
  else if (*arg == '/')
    *window = cdc_window_default[hash];
  else
    error (EXIT_FAILURE, 0,
           _("cannot parse %s: neither hash window nor chunk size"),
           quote (arg));

  if (*arg != '/')
    error (EXIT_FAILURE, 0, _("cannot parse %s as chunk size"), quote (arg));
  arg++; /* skip '/' */

  char *next = NULL;
  strtol_error e = xstrtoimax (arg, &next, 10, avgsz, byte_multipliers);
  if (!(e == LONGINT_OK || (e == LONGINT_INVALID_SUFFIX_CHAR && *next == '/')))
    strtoint_die (N_ ("cannot parse average chunk size"), arg);

  /* AVGSZ > WINDOW is not a hard requirement for rolling hash, but it's way
     easier to reason about chunks having at least WINDOW bytes each.  */
  if (*avgsz <= *window)
    error (EXIT_FAILURE, 0,
           _("average chunk (%jd) must be larger than hash window (%jd)"),
           *avgsz, *window);

  /* Let's set ~40 MiB as the largest chunk size that is supported by decision
     function over 32-bit hash value with 1% error tolerance.  The smallest
     value to exceed 1% err is 43821726 bytes.

     Other options are to make exception for power-of-two values or to compute
     error margin for specific AVGSZ value.  However, several discontinuous
     ranges of accepted values are kinda confusing from UX standpoint.

     High power-of-two values like 2G or 4G bring another issue to the table.
     It's not _proven_ that BUZHash can actually produce every possible N-bit
     hash value for every possible WINDOW given specific S-box.  So it's not
     proven that 1 or 0 will ever be emitted as a hash value to support 2G/4G.
     It's trivially false for small windows: e.g. 3-byte window has no way
     to produce more than 2^24 hashes.  It's also possible (but very unlikely)
     to generate a bit-balanced BUZHash S-box with every popcount(cdc_table[c])
     being even.  Such S-box makes popcount(buzNN(STR)) even for _any_ STR
     as both ROTL and XOR operations preserve parity of the popcount.  */
  intmax_t const hash32_chunk_max = INTMAX_C (42000000);
  if (cdc_is32 (hash) && *avgsz > hash32_chunk_max)
    error (EXIT_FAILURE, 0,
           _("average chunk over 40MiB/42MB needs 64-bit hash"));

  /* There is no explicit "signaling" value to skip MAXSZ code altogether.
     First, 2^63 is large enough.  Second, CDC is probabilistic anyway :-P  */
  static_assert (INT64_MAX <= INTMAX_MAX);
  *maxsz = INTMAX_MAX;
  if (*next == '/'
      && xstrtoimax (next + 1, NULL, 10, maxsz, byte_multipliers)
             != LONGINT_OK)
    strtoint_die (N_ ("cannot parse maximum chunk size"), next + 1);

  if (*maxsz <= *avgsz)
    error (EXIT_FAILURE, 0,
           _("maximum chunk (%jd) must be larger than average chunk (%jd)"),
           *maxsz, *avgsz);

  return hash;
}

static void
cdc_table_init (enum Cdc_type hash, char const *random_source, idx_t window)
{
  /* Alignment is not vital for CDC lookup tables, but it saves one cache-line
     and it might save us from confusing fall from the D-cache cliff.  */
  idx_t const cacheline_size = getcachelinesize ();

  /* The code does not support vectorised rolling hash _implementations_.
     Naive vectorisation hits memory wall as each input byte is processed
     through lookup table at least once.  Replacing lookup table with
     pseudo-random function from u8 to u32|u64 is possible but it makes
     its interface different as --random-source would use entropy differently.
     So, potential SIMD implementation is effectively a _different_ rolling
     hash function with different name.  And it still has to be quite fast
     to beat SISD implementation running at 1.33 cpb :-)  */
  if (!random_source && cdc_is64 (hash))
    cdc_table = buz_seed;
  else if (!random_source && cdc_is32 (hash))
    {
      uint32_t *t = xalignalloc (cacheline_size, N_CHARS * sizeof (*t));
      for (idx_t i = 0; i < N_CHARS; i++)
        t[i] = buz_seed[i];
      cdc_table = t;
    }
  else if (random_source && cdc_isgear (hash))
    {
      /* random-source for GearHash is N_CHARS little-endian integers */
      size_t const sizeof_hash
          = cdc_is64 (hash) ? sizeof (uint64_t) : sizeof (uint32_t);
      size_t const sizeof_table = N_CHARS * sizeof_hash;
      void *t = xalignalloc (cacheline_size, sizeof_table);
      FILE *fd = fopen (random_source, "rb");
      if (!fd)
        error (EXIT_FAILURE, errno, "%s", quotef (random_source));
      size_t const n_read = fread (t, 1, sizeof_table, fd);
      if (n_read != sizeof_table)
        error (EXIT_FAILURE, 0, _("%s: got only %zu of %zu bytes"),
               quotef (random_source), n_read, sizeof_table);
      fclose (fd);
      if (hash == cdc_gear64)
        for (uint64_t *p = t, *const end = p + N_CHARS; p != end; p++)
          *p = le64toh (*p);
      else
        for (uint32_t *p = t, *const end = p + N_CHARS; p != end; p++)
          *p = le32toh (*p);
      cdc_table = t;
    }
  else if (random_source && cdc_isbuz (hash))
    {
      /* random-source for BUZHash is more complex, make-buz-table.c describes
         the reasons.  GearHash random-source reader above is constant-time and
         it's 10 times faster than BUZHash S-box generation, but that's 0.1M
         CPU cycles vs. 1M.  Effect of those 1M cycles of initialization is
         quite low: it's BUZHash runtime over ~400 KiB of data.  */
      size_t const sizeof_hash
          = cdc_is64 (hash) ? sizeof (uint64_t) : sizeof (uint32_t);
      size_t const hash_width = sizeof_hash * UCHAR_WIDTH;
      /* randperm_bound() is not really a strict bound, it's just a hint.
         Infinite stream of 0xFF bytes makes the sampling RNG loop!  */
      size_t const seed_size
          = randperm_bound (N_CHARS / 2, N_CHARS) * hash_width;
      struct randint_source *r = randint_all_new (random_source, seed_size);
      if (!r)
        error (EXIT_FAILURE, errno, "%s", quotef (random_source));
      size_t const sizeof_table = sizeof_hash * N_CHARS;
      void *const t = xalignalloc (cacheline_size, sizeof_table);
      memset (t, 0, sizeof_table);
      for (unsigned bit = 0; bit < hash_width; bit++)
        {
          uint32_t *const t32 = t;
          uint64_t *const t64 = t;
          size_t *const perm = randperm_new (r, N_CHARS / 2, N_CHARS);
          if (hash == cdc_buz64)
            for (unsigned c = 0; c < N_CHARS / 2; c++)
              t64[perm[c]] |= UINT64_C (1) << bit;
          else
            for (unsigned c = 0; c < N_CHARS / 2; c++)
              t32[perm[c]] |= UINT32_C (1) << bit;
          free (perm);
        }
      if (randint_all_free (r))
        error (EXIT_FAILURE, errno, "%s", quotef (random_source));
      cdc_table = t;
    }
  else
    affirm (false);

  affirm (window >= 1);
  if ((hash == cdc_buz32 && window % 32 == 0)
      || (hash == cdc_buz64 && window % 64 == 0))
    unbuz_table = cdc_table;
  else if (hash == cdc_buz32)
    {
      uint32_t const *const buz = cdc_table;
      uint32_t *t = xalignalloc (cacheline_size, N_CHARS * sizeof *t);
      for (int i = 0; i < N_CHARS; i++)
        t[i] = rotl32 (buz[i], window % 32);
      unbuz_table = t;
    }
  else if (hash == cdc_buz64)
    {
      uint64_t const *const buz = cdc_table;
      uint64_t *t = xalignalloc (cacheline_size, N_CHARS * sizeof *t);
      for (int i = 0; i < N_CHARS; i++)
        t[i] = rotl64 (buz[i], window % 64);
      unbuz_table = t;
    }
  else
    affirm ((hash == cdc_gear32 && window == 32)
            || (hash == cdc_gear64 && window == 64));
}

int
main (int argc, char **argv)
{
  enum Split_type split_type = type_undef;
  enum Cdc_type cdc_type = cdc_undef;
  idx_t in_blk_size = 0;	/* optimal block size of input file device */
  idx_t page_size = getpagesize ();
  intmax_t k_units = 0;
  intmax_t w_units = 0;
  intmax_t n_units = 0;
  char const *random_source = NULL;

  int c;
  int digits_optind = 0;
  off_t file_size = OFF_T_MAX;

  initialize_main (&argc, &argv);
  set_program_name (argv[0]);
  setlocale (LC_ALL, "");
  bindtextdomain (PACKAGE, LOCALEDIR);
  textdomain (PACKAGE);

  atexit (close_stdout);

  /* Parse command line options.  */

  while (true)
    {
      /* This is the argv-index of the option we will read next.  */
      int this_optind = optind ? optind : 1;

      c = getopt_long (argc, argv, "0123456789C:a:b:del:n:t:ux",
                       longopts, NULL);
      if (c == -1)
        break;

      switch (c)
        {
        case 'a':
          suffix_length = xdectoimax (optarg, 0, IDX_MAX,
                                      "", _("invalid suffix length"), 0);
          break;

        case ADDITIONAL_SUFFIX_OPTION:
          {
            int suffix_len = strlen (optarg);
            if (last_component (optarg) != optarg
                || (suffix_len && ISSLASH (optarg[suffix_len - 1])))
              {
                error (0, 0,
                       _("invalid suffix %s, contains directory separator"),
                       quote (optarg));
                usage (EXIT_FAILURE);
              }
          }
          additional_suffix = optarg;
          break;

        case 'b':
          if (split_type != type_undef)
            FAIL_ONLY_ONE_WAY ();
          /* skip any whitespace */
          while (isspace (to_uchar (*optarg)))
            optarg++;
          if (isdigit (*optarg))
            {
              split_type = type_bytes;
              n_units = parse_n_units (optarg, byte_multipliers,
                                       N_("invalid number of bytes"));
            }
          else
            {
              split_type = type_bytes_cdc;
              cdc_type = parse_cdc (&w_units, &n_units, &k_units, optarg);
            }
          break;

        case 'l':
          if (split_type != type_undef)
            FAIL_ONLY_ONE_WAY ();
          split_type = type_lines;
          n_units = parse_n_units (optarg, "", N_("invalid number of lines"));
          break;

        case 'C':
          if (split_type != type_undef)
            FAIL_ONLY_ONE_WAY ();
          /* skip any whitespace */
          while (isspace (to_uchar (*optarg)))
            optarg++;
          if (isdigit (*optarg))
            {
              split_type = type_byteslines;
              n_units = parse_n_units (optarg, byte_multipliers,
                                       N_("invalid number of lines"));
            }
          else
            {
              split_type = type_byteslines_cdc;
              cdc_type = parse_cdc (&w_units, &n_units, &k_units, optarg);
            }
          break;

        case 'n':
          if (split_type != type_undef)
            FAIL_ONLY_ONE_WAY ();
          /* skip any whitespace */
          while (isspace (to_uchar (*optarg)))
            optarg++;
          if (STRNCMP_LIT (optarg, "r/") == 0)
            {
              split_type = type_rr;
              optarg += 2;
            }
          else if (STRNCMP_LIT (optarg, "l/") == 0)
            {
              split_type = type_chunk_lines;
              optarg += 2;
            }
          else
            split_type = type_chunk_bytes;
          parse_chunk (&k_units, &n_units, optarg);
          break;

        case 'u':
          unbuffered = true;
          break;

        case 't':
          {
            char neweol = optarg[0];
            if (! neweol)
              error (EXIT_FAILURE, 0, _("empty record separator"));
            if (optarg[1])
              {
                if (streq (optarg, "\\0"))
                  neweol = '\0';
                else
                  {
                    /* Provoke with 'split -txx'.  Complain about
                       "multi-character tab" instead of "multibyte tab", so
                       that the diagnostic's wording does not need to be
                       changed once multibyte characters are supported.  */
                    error (EXIT_FAILURE, 0, _("multi-character separator %s"),
                           quote (optarg));
                  }
              }
            /* Make it explicit we don't support multiple separators.  */
            if (0 <= eolchar && neweol != eolchar)
              {
                error (EXIT_FAILURE, 0,
                       _("multiple separator characters specified"));
              }

            eolchar = neweol;
          }
          break;

        case '0':
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '7':
        case '8':
        case '9':
          if (split_type == type_undef)
            {
              split_type = type_digits;
              n_units = 0;
            }
          if (split_type != type_undef && split_type != type_digits)
            FAIL_ONLY_ONE_WAY ();
          if (digits_optind != 0 && digits_optind != this_optind)
            n_units = 0;	/* More than one number given; ignore other. */
          digits_optind = this_optind;
          if (ckd_mul (&n_units, n_units, 10)
              || ckd_add (&n_units, n_units, c - '0'))
            n_units = INTMAX_MAX;
          break;

        case 'd':
        case 'x':
          if (c == 'd')
            suffix_alphabet = "0123456789";
          else
            suffix_alphabet = "0123456789abcdef";
          if (optarg)
            {
              if (strlen (optarg) != strspn (optarg, suffix_alphabet))
                {
                  error (0, 0,
                         (c == 'd') ?
                           _("%s: invalid start value for numerical suffix") :
                           _("%s: invalid start value for hexadecimal suffix"),
                         quote (optarg));
                  usage (EXIT_FAILURE);
                }
              else
                {
                  /* Skip any leading zero.  */
                  while (*optarg == '0' && *(optarg + 1) != '\0')
                    optarg++;
                  numeric_suffix_start = optarg;
                }
            }
          break;

        case 'e':
          elide_empty_files = true;
          break;

        case FILTER_OPTION:
          filter_command = optarg;
          break;

        case RANDOM_SOURCE_OPTION:
          if (random_source && !streq (random_source, optarg))
            error (EXIT_FAILURE, 0, _("multiple random sources specified"));
          random_source = optarg;
          break;

        case IO_BLKSIZE_OPTION:
          in_blk_size
              = xnumtoumax (optarg, 10, 1,
                            MIN (SYS_BUFSIZE_MAX, MIN (IDX_MAX, SIZE_MAX) - 1),
                            byte_multipliers, _("invalid IO block size"), 0,
                            XTOINT_MIN_RANGE);
          break;

        case VERBOSE_OPTION:
          verbose = true;
          break;

        case DEBUG_PROGRAM_OPTION:
          debug = true;
          break;

        case_GETOPT_HELP_CHAR;

        case_GETOPT_VERSION_CHAR (PROGRAM_NAME, AUTHORS);

        default:
          usage (EXIT_FAILURE);
        }
    }

  if (!iscdc (split_type) && k_units != 0 && filter_command)
    {
      error (0, 0, _("--filter does not process a chunk extracted to "
                     "standard output"));
      usage (EXIT_FAILURE);
    }

  /* Handle default case.  */
  if (split_type == type_undef)
    {
      split_type = type_lines;
      n_units = 1000;
    }

  if (n_units == 0)
    {
      error (0, 0, _("invalid number of lines: %s"), quote ("0"));
      usage (EXIT_FAILURE);
    }

  if (eolchar < 0)
    eolchar = '\n';

  set_suffix_length (n_units, split_type);

  /* Get out the filename arguments.  */

  if (optind < argc)
    infile = argv[optind++];

  if (optind < argc)
    outbase = argv[optind++];

  if (optind < argc)
    {
      error (0, 0, _("extra operand %s"), quote (argv[optind]));
      usage (EXIT_FAILURE);
    }

  /* Check that the suffix length is large enough for the numerical
     suffix start value.  */
  if (numeric_suffix_start && strlen (numeric_suffix_start) > suffix_length)
    {
      error (0, 0, _("numerical suffix start value is too large "
                     "for the suffix length"));
      usage (EXIT_FAILURE);
    }

  /* Open the input file.  */
  if (! streq (infile, "-")
      && fd_reopen (STDIN_FILENO, infile, O_RDONLY, 0) < 0)
    error (EXIT_FAILURE, errno, _("cannot open %s for reading"),
           quoteaf (infile));

  /* Binary I/O is safer when byte counts are used.  */
  xset_binary_mode (STDIN_FILENO, O_BINARY);

  /* Advise the kernel of our access pattern.  */
  fdadvise (STDIN_FILENO, 0, 0, FADVISE_SEQUENTIAL);

  /* Get the optimal block size of input device and make a buffer.  */

  if (fstat (STDIN_FILENO, &in_stat_buf) != 0)
    error (EXIT_FAILURE, errno, "%s", quotef (infile));

  if (in_blk_size == 0)
    in_blk_size = io_blksize (&in_stat_buf);
  /* It's not hard to support larger BUZHash window: few more memmove calls.
     It's tricky to do that without performance degradation within the current
     code spinning around I/O buffer of IN_BLK_SIZE bytes.  */
  int const buz_window_max = MIN (in_blk_size, IO_BUFSIZE);
  if (iscdc (split_type) && cdc_isbuz (cdc_type) && buz_window_max < w_units)
    error (EXIT_FAILURE, 0,
           _("%s[%jd] exceeds the largest supported BUZHash window (%d)"),
           cdc_names[cdc_type], w_units, buz_window_max);

  /* The I/O buffer is IN_BLK_SIZE bytes and is aligned to the PAGE_SIZE.
     lines_split() uses one more byte so avoid boundary checks with rawmemchr.
     The same idea needs W_UNITS bytes to terminate GearHash computation.  */
  idx_t buf_size = in_blk_size;
  if (split_type == type_digits || split_type == type_lines)
    buf_size += 1;
  else if (iscdc (split_type) && cdc_isgear (cdc_type))
    buf_size += w_units;

  /* BUZHash needs prepended WINDOW for computation, GearHash needs it
     for backtracking on short read.  */
  idx_t prepend = 0;
  if (iscdc (split_type))
    if (ckd_add (&prepend, 1, (w_units - 1) | (page_size - 1)))
      xalloc_die ();
  if (ckd_add (&buf_size, buf_size, prepend))
    xalloc_die ();

  char *buf = xalignalloc (page_size, buf_size);
  /* memset() is here to suppress warning about reading uninitialized memory
     with memmove() in case of short read.  The uninitialized value is not used
     in computation as it's still hash gulping stage of bytes_cdc_split.  */
  memset (buf, 0, prepend);
  buf += prepend;

  ssize_t initial_read = -1;
  if (split_type == type_chunk_bytes || split_type == type_chunk_lines)
    {
      file_size = input_file_size (STDIN_FILENO, &in_stat_buf,
                                   buf, in_blk_size);
      if (file_size < 0)
        error (EXIT_FAILURE, errno, _("%s: cannot determine file size"),
               quotef (infile));
      initial_read = MIN (file_size, in_blk_size);
    }
  else if (iscdc (split_type))
    cdc_table_init (cdc_type, random_source, w_units);

  /* When filtering, closure of one pipe must not terminate the process,
     as there may still be other streams expecting input from us.  */
  if (filter_command)
    default_SIGPIPE = signal (SIGPIPE, SIG_IGN) == SIG_DFL;

  switch (split_type)
    {
    case type_digits:
    case type_lines:
      lines_split (n_units, buf, in_blk_size);
      break;

    case type_bytes:
      bytes_split (n_units, 0, buf, in_blk_size, -1, 0);
      break;

    case type_bytes_cdc:
    case type_byteslines_cdc:
      bytes_cdc_split (cdc_type, n_units, k_units, w_units, buf, in_blk_size,
                       split_type == type_byteslines_cdc);
      break;

    case type_byteslines:
      line_bytes_split (n_units, buf, in_blk_size);
      break;

    case type_chunk_bytes:
      if (k_units == 0)
        bytes_split (file_size / n_units, file_size % n_units,
                     buf, in_blk_size, initial_read, n_units);
      else
        bytes_chunk_extract (k_units, n_units, buf, in_blk_size, initial_read,
                             file_size);
      break;

    case type_chunk_lines:
      lines_chunk_split (k_units, n_units, buf, in_blk_size, initial_read,
                         file_size);
      break;

    case type_rr:
      /* Note, this is like 'sed -n ${k}~${n}p' when k > 0,
         but the functionality is provided for symmetry.  */
      {
        of_t *files;
        lines_rr (k_units, n_units, buf, in_blk_size, &files);
      }
      break;

    case type_undef: default:
      affirm (false);
    }

  if (close (STDIN_FILENO) != 0)
    error (EXIT_FAILURE, errno, "%s", quotef (infile));
  closeout (NULL, output_desc, filter_pid, outfile);

  main_exit (EXIT_SUCCESS);
}
