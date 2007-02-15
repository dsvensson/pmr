/* pmr by Heikki Orsila <heikki.orsila@iki.fi>
   This source is in public domain. Do what you want with it.

   Get latest version of pmr from: http://www.iki.fi/shd/foss/pmr/

   Pmr displays the bandwidth of the pipe going through the process.
*/

#define _LARGEFILE64_SOURCE
#define _FILE_OFFSET_BITS 64

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <assert.h>

#include "md5.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif


#define darray_append(n, nallocated, array, item) do { \
    assert((n) >= 0); \
    assert((nallocated) >= 0); \
    assert((n) <= (nallocated)); \
                                 \
    if ((n) == (nallocated)) { \
      if ((nallocated) == 0) \
        (nallocated) = 1; \
      (nallocated) *= 2; \
      assert((nallocated) > 0); \
      (array) = realloc((array), (nallocated) * sizeof((array)[0])); \
      if ((array) == NULL) { \
        fprintf(stderr, "no memory for darray elements\n"); \
        exit(-1); \
      } \
    } \
      \
    (array)[(n)] = (item); \
    (n)++; \
  } while (0)


#define VERSION "0.12"

#define BUFFER_SIZE 8192

#ifndef PAGE_SIZE
#define PAGE_SIZE 8192
#endif

extern int errno;

static int end_program;
static int program_interrupted;

static int default_interval = 2000;

static int max_rate = -1;
static int rate_read_bytes = 0;
struct timeval rate_time;

static int input_fd;
static size_t n_files;
static size_t n_files_read;
static size_t n_files_allocated;
static char **files;
static long long estimatedbytes;

#define SPEED_WINDOW_SIZE 3
static double speed_window[SPEED_WINDOW_SIZE];

#define SPACE_LENGTH 80
static char space[SPACE_LENGTH];
static int old_line_length;
static int use_carriage_return;


static void append_eta(char *info, double bw, long long tbytes)
{
  int i;
  double sum = 0.0;
  int nvalid = 0;
  long long bytesleft;
  char str[256];

  /* Compute a speed estimate. The estimate is an average of N previous
     speeds. */
  for (i = 1; i < SPEED_WINDOW_SIZE; i++) {
    double s = speed_window[i - 1];
    speed_window[i] = s;
    sum += s;
    if (s > 0.0)
      nvalid++;
  }

  speed_window[0] = bw;
  sum += bw;
  if (bw > 0.0)
    nvalid++;

  if (nvalid == 0)
    return;

  bytesleft = estimatedbytes - tbytes;
  if (bytesleft < 0) {
    sprintf(str, "\tETA: -");
  } else {
    double eta = bytesleft / (sum / nvalid);

    sprintf(str, "\tETA: %.1fs", eta);
  }

  strcat(info, str);
}


static void ctrlc_handler(int sig)
{
  if (sig == SIGINT)
    program_interrupted = 1;
  end_program = 1;
}


static double inverse_size_transformation(const char *valuestr)
{
  char *endptr;
  double value;

#define NUNITS (9)
  const char *iec_units[NUNITS] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB",
				   "EiB", "ZiB", "YiB"};
  const char *si_byte_units[NUNITS] = {"B", "kB", "MB", "GB", "TB", "PB", "EB",
				       "ZB", "YB"};
  const char *si_bit_units[NUNITS] = {"b", "kbit", "Mbit", "Gbit", "Tbit",
				      "Pbit", "Ebit", "Zbit", "Ybit"};


  double multiplier;
  int i;

  value = strtod(valuestr, &endptr);
  if (*endptr == 0)
    return value;

  multiplier = 1.0;

  /* we do case-sensitive comparison here to not mix bits and bytes */
  if (strcmp(endptr, "B") == 0)
    return value;

  /* case-insensitive comparisons for IEC units */
  for (i = 0; i < NUNITS; i++) {
    if (strncasecmp(endptr, iec_units[i], 2) == 0)
      return multiplier * value;
    multiplier *= 1024.0;
  }

  /* case-sensitive comparisons for bytes with SI units */
  multiplier = 1.0;
  for (i = 0; i < NUNITS; i++) {
    if (strncmp(endptr, si_byte_units[i], 2) == 0)
      return multiplier * value;
    multiplier *= 1000.0;
  }

  /* case-sensitive comparisons for bits with SI multipliers */
  multiplier = 1.0 / 8;
  for (i = 0; i < NUNITS; i++) {
    if (strncmp(endptr, si_bit_units[i], 2) == 0)
      return multiplier * value;
    multiplier *= 1000.0;
  }

  fprintf(stderr, "pmr error: unknown unit: %s\n", endptr);
  exit(-1);
  return 0.0;
}


static int open_new_file(void)
{
  char *fname;

  close(input_fd);

  if (n_files_read == n_files)
    return 0;

  fname = files[n_files_read++];

  input_fd = open(fname, O_RDONLY);
  if (input_fd < 0) {
    fprintf(stderr, "Error opening file %s: %s\n", fname, strerror(errno));
    exit(-1);
  }

  return 1;
}


static void size_transformation(double *size, char *unit, double srcsize)
{
  const char *units[9] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB", "ZiB",
			  "YiB"};
  int order = 0;
  if (srcsize < 0) {
    fprintf(stderr, "pmr bug: negative size\n");
    *size = 0;
    strcpy(unit, "B");
    return;
  }
  while (srcsize >= 1024.0f) {
    srcsize /= 1024.0f;
    order++;
  }

  if (order >= (int) (sizeof(units) / sizeof(units[0]))) {
    fprintf(stderr, "pmr bug: too high number order\n");
    *size = 0;
    strcpy(unit, "B");
    return;
  }

  *size = srcsize;
  strcpy(unit, units[order]);
}


static int timetest(double *bw, char *s, size_t maxlen,
		    struct timeval *ot, long long *bytes, int force)
{
  struct timeval nt;
  int t;

  *bw = 0.0;

  strcpy(s, "bandwidth: NaN");
  if (gettimeofday(&nt, 0)) {
    /* time failed. bandwidth = NaN. return false. */
    return 0;
  }
  t = 1000 * (nt.tv_sec - ot->tv_sec) + ((int) nt.tv_usec)/1000 - ((int) ot->tv_usec)/1000;
  if (t < 0) {
    fprintf(stderr, "pmr: chronoton particles detected. clock ran backwards. k3wl!\n");
    t = default_interval + 1;
  }
  if (t > default_interval || force) {
    if (t) {
      double canonical_bw, real_bw;
      char id[16];

      real_bw = (1000.0 * (*bytes)) / t;

      *bw = real_bw;

      size_transformation(&canonical_bw, id, real_bw);
      snprintf(s, maxlen, "bandwidth: %.2f %s/s", canonical_bw, id);
    }
    *ot = nt;
    *bytes = 0;
    return 1;
  }
  return 0;
}


static int wait_input(void)
{
  fd_set rfds;
  FD_ZERO(&rfds);
  FD_SET(input_fd, &rfds);
  if (select(input_fd + 1, &rfds, NULL, NULL, NULL) < 0) {
    if (errno != EINTR) {
      fprintf(stderr, "Read select error: %s\n", strerror(errno));
      return -1;
    }
  }
  return 0;
}


int read_no_rate_limit(char *buf, int size)
{
  return read(input_fd, buf, size);
}

int read_rate_limit(char *buf, int size)
{
  int ret;
  int to_read, read_bytes;
  int t;
  struct timeval new_rate_time;

  if (max_rate == -1)
    return read_no_rate_limit(buf, size);
  
  if (gettimeofday(&new_rate_time, 0)) {
    perror ("pmr: gettimeofday failed. can not limit rate. going max speed.");
    max_rate = -1;
    return read_no_rate_limit(buf, size);
  }

  if (rate_read_bytes > max_rate) {
    fprintf(stderr, "fatal error: rate_read_bytes > max_rate!\n");
    exit(-1);
  }

  if (rate_read_bytes == max_rate) {
    t = 1000 * (new_rate_time.tv_sec - rate_time.tv_sec) + ((int) new_rate_time.tv_usec)/1000 - ((int) rate_time.tv_usec)/1000;
    if (t < 0) {
      fprintf(stderr, "pmr: chronoton particles detected. clock ran backwards. k3wl!\n");
      t = default_interval + 1;
    }
    if (t < 1000) {
      usleep (1000000 - 1000 * t);
      if (gettimeofday(&new_rate_time, 0)) {
	perror ("pmr: gettimeofday failed. can not limit rate. going max speed.");
	max_rate = -1;
	return read_no_rate_limit(buf, size);
      }
    }
    rate_time = new_rate_time;
    rate_read_bytes = 0;
  }

  to_read = max_rate - rate_read_bytes;
  if (to_read > size)
    to_read = size;

  read_bytes = 0;

  while (read_bytes < to_read) {

    ret = read(input_fd, buf + read_bytes , to_read - read_bytes);

    if (ret > 0) {
      read_bytes += ret;

    } else if (ret == 0) {
      break;

    } else {
      /* The upper-level will do error handling (EAGAIN, EINTR, ...) */
      if (read_bytes == 0)
	return -1;
      break;
    }

    if (program_interrupted) {
      if (read_bytes == 0) {
	/* we must simulate some error condition for upper-level */
	errno = EINTR;
	return -1;
      }
      break;
    }
  }

  rate_read_bytes += read_bytes;

  return read_bytes;
}


static void print_usage(const char *pname)
{
  fprintf(stderr, "pmr %s by Heikki Orsila <heikki.orsila@iki.fi>\n\nUsage:\n\n", VERSION);
  fprintf(stderr, " %s [-l Bps] [-s size] [-m] [-t seconds] [-p] [-b size] [-r] [-v] FILE ...\n\n", pname);
  fprintf(stderr, " -b size\tset input buffer size (default %d)\n", BUFFER_SIZE);
  fprintf(stderr, " -l Bps\t\tLimit throughput to 'Bps' bytes per second. It is also\n");
  fprintf(stderr, "\t\tpossible to use SI, IEC 60027 and bit units in the value.\n");
  fprintf(stderr, "\t\tSI units include kB, MB, ..., IEC units include KiB, MiB, ...\n");
  fprintf(stderr, "\t\tand bit units include kbit, Mbit, ...\n");
  fprintf(stderr, " -m / --md5\tCompute an md5 checksum of the stream (useful for verifying\n");
  fprintf(stderr, "\t\tdata integrity through TCP networks)\n");
  fprintf(stderr, " -p\t\tEnables 4k page poking (useless)\n");
  fprintf(stderr, " -r\t\tUse carriage return on output, no newline\n");
  fprintf(stderr, " -s size\tCalculate ETA given a size estimate. Giving regular files\n");
  fprintf(stderr, "\t\tas pmr parameters implies -s SUM, where SUM is the total\n");
  fprintf(stderr, "\t\tsize of those files.\n");
  fprintf(stderr, " -t secs\tUpdate interval in seconds\n");
  fprintf(stderr, " -v\t\tPrint version, about, contact and home page information\n");
}


static void write_info(const char *info)
{
  if (use_carriage_return) {

    /* Writes length amount of spaces and then a carriage return (\r) */
    assert(old_line_length >= 0);

    while (old_line_length >= SPACE_LENGTH) {
      fwrite(space, 1, SPACE_LENGTH, stderr);
      old_line_length -= SPACE_LENGTH;
    }

    fwrite(space, 1, old_line_length, stderr);

    fprintf(stderr, "\r");

    /* Write info */
    old_line_length = fprintf(stderr, "%s\r", info) - 1;

    /* In case stderr is lost (how??) */
    if (old_line_length < 0)
      old_line_length = 0;

  } else {
    /* Write info */
    fprintf(stderr, "%s\n", info);
  }
}


static void read_config(int *use_md5)
{
  char *home;
  char cfilename[PATH_MAX];
  FILE *cfile;
  char word[256];
  int t;
  int ret;

  home = getenv("HOME");
  if (home == NULL)
    return;

  snprintf(cfilename, sizeof cfilename, "%s/.pmr", home);

  cfile = fopen(cfilename, "r");
  if (cfile == NULL)
    return;

  while (1) {
    ret = fscanf(cfile, "%s", word);
    if (ret <= 0)
      break;

    if (strncasecmp(word, "carriage_return", 8) == 0) {
      use_carriage_return = 1;

    } else if (strncasecmp(word, "md5", 3) == 0) {
      *use_md5 = 1;

    } else if (strncasecmp(word, "update_interval", 3) == 0) {
      ret = fscanf(cfile, "%d", &t);
      if (ret <= 0) {
	fprintf(stderr, "Missing update interval\n");
	continue;
      }
      assert(t >= 0);
      default_interval = 1000 * t;

    } else {
      fprintf(stderr, "Unknown configuration directive: %s\n", word);
    }
  }

  fclose(cfile);
}


int main(int argc, char **argv)
{
  int aligned_size = BUFFER_SIZE;
  char *real_buf;
  char *aligned_buf;

  struct sigaction act = {
    .sa_handler = ctrlc_handler
  };

  struct timeval ot, vot;
  int i, ret, readoffs, rbytes;
  long long tbytes, wbytes;
  double bw;

  int poke_mem = 0;
  int valid_time = 1;
  int (*read_function)(char *buf, int size) = read_no_rate_limit;
  int use_md5 = 0;
  MD5_CTX md5ctx;

  char info[256];

  int no_options = 0;

  double presize = 0.0;

  read_config(&use_md5);

  for (i = 1; i < argc;) {

    if (no_options || argv[i][0] != '-') {

      char *fname = argv[i];
      struct stat st;

      if (stat(fname, &st)) {
	fprintf(stderr, "%s can not be read: %s\n", fname, strerror(errno));
	exit(-1);
      }

      if (!S_ISREG(st.st_mode)) {
	fprintf(stderr, "One of the files is not a regular file -> no ETA estimation\n");
	estimatedbytes = -1;
      }

      if (estimatedbytes >= 0)
	estimatedbytes += st.st_size;

      darray_append(n_files, n_files_allocated, files, fname);

      no_options = 1;
      i++;
      continue;
    }

    if (!strcmp(argv[i], "-t")) {
      if ((i + 1) < argc) {
	default_interval = 1000 * atoi(argv[i + 1]);
      } else {
	fprintf (stderr, "expecting a value for -t\n");
	return -1;
      }
      i += 2;
      continue;
    }

    if (!strcmp(argv[i], "-b")) {
      if ((i + 1) < argc) {
	aligned_size = atoi(argv[i + 1]);
      } else {
	fprintf (stderr, "expecting a value for -b\n");
	return -1;
      }
      i += 2;
      continue;
    }

    if (!strcmp(argv[i], "-l")) {
      if ((i + 1) < argc) {
	double value = inverse_size_transformation(argv[i + 1]);
	if (value <= 0) {
	  fprintf (stderr, "illegal bytes per second value (%s)\n", argv[i + 1]);
	  return -1;
	} else if (value >= 2147483648UL) {
	  fprintf(stderr, "too high bytes per second value (%s)\n", argv[i + 1]);
	  return -1;
	}
        max_rate = value;
	read_function = read_rate_limit;
      } else {
        fprintf (stderr, "expecting a value for bandwidth limit (bytes per second)\n");
        return -1;
      }
      i += 2;
      continue;
    }

    if (!strcmp(argv[i], "-m") || !strcmp(argv[i], "--md5")) {
      use_md5 = 1;
      i++;
      continue;
    }

    if (!strcmp(argv[i], "-r")) {
      use_carriage_return = 1;
      memset(space, 0, SPACE_LENGTH);
      i++;
      continue;
    }

    if (!strcmp(argv[i], "-s")) {
      if ((i + 1) >= argc) {
	fprintf(stderr, "Not enough args. Missing size estimate.\n");
	exit(-1);
      }
      presize = inverse_size_transformation(argv[i + 1]);
      if (presize < 0.0) {
	fprintf(stderr, "Braindamaged size estimate: %f.\n", presize);
	presize = 0.0;
      }
      i += 2;
      continue;
    }

    if (!strcmp(argv[i], "-p")) {
      poke_mem = 1;
      i++;
      continue;
    }

    if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
      print_usage(argv[0]);
      return 0;
    }

    if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--version")) {
      fprintf(stderr, "pmr %s by Heikki Orsila <heikki.orsila@iki.fi>\n", VERSION);
      fprintf(stderr, "This program is public domain.\n");
      fprintf(stderr, "You can get the latest version of the program from:\n");
      fprintf(stderr, "\n  http://www.iki.fi/shd/foss/pmr/\n\n");
      return 0;
    }

    if (!strcmp(argv[i], "--")) {
      no_options = 1;
      i++;
      continue;
    }

    fprintf(stderr, "unknown args: %s\n", argv[i]);
    return -1;
  }

  if (use_md5)
    MD5Init(&md5ctx);

  if (presize > 0.0)
    estimatedbytes = presize;

  /* get page size aligned buffer of size 'aligned_size' */
  real_buf = malloc(aligned_size + PAGE_SIZE);
  if (!real_buf) {
    fprintf(stderr, "pmr: not enough memory\n");
    exit(-1);
  }
  aligned_buf = real_buf;
  aligned_buf += PAGE_SIZE - (((long) aligned_buf) & (PAGE_SIZE - 1));

  sigaction(SIGPIPE, &act, 0);
  sigaction(SIGINT, &act, 0);

  if (gettimeofday(&ot, 0)) {
    memset(&ot, 0, sizeof(ot));
    perror ("pmr: gettimeofday failed");
    valid_time = 0;
  }
  vot = ot;
  rate_time = ot; /* useless if max_rate was not set */

  wbytes = 0;
  tbytes = 0;

  if (n_files > 0)
    open_new_file();

  while (!end_program) {

    ret = read_function(aligned_buf, aligned_size);

    /* Note, we will try to write all data received so far even if the
       program has been aborted (SIGINT) */

    if (ret > 0) {
      /* you may ignore poke_mem code (just for performance evaluation) */
      if (poke_mem) {
	do {
	  volatile char *vb = aligned_buf;
	  /* 4k page is purposeful. don't replace this with PAGE_SIZE */
	  for (i = 0; i < ret; i += 4096) {
	    vb[i] = vb[i];
	  }
	} while (0);
      }

      rbytes = ret;
      readoffs = 0;
      while (readoffs < rbytes) {
	ret = write(1, &aligned_buf[readoffs], rbytes - readoffs);
	if (ret > 0) {
	  wbytes += ret;
	  tbytes += ret;
	  readoffs += ret;
	} else if (ret == 0) {
	  fprintf(stderr, "pmr: interesting: write returned 0\n");
	  end_program = 1;
	  break;
	} else if (ret < 0) {
	  if (errno == EINTR) {
	    continue;
	  } else if (errno == EAGAIN) {
	    fd_set wfds;
	    FD_ZERO(&wfds);
	    FD_SET(1, &wfds);
	    if (select(2, NULL, &wfds, NULL, NULL) < 0) {
	      if (errno != EINTR) {
		fprintf(stderr, "Write select error: %s\n", strerror(errno));
		end_program = 1;
		break;
	      }
	    }
	  } else {
	    perror("pmr: Write error");
	    end_program = 1;
	    break;
	  }
	}
      }

      if (use_md5)
	MD5Update(&md5ctx, (unsigned char *) aligned_buf, rbytes);

      if (valid_time && timetest(&bw, info, sizeof(info), &ot, &wbytes, 0)) {
	char byte_info[256];
	char unit[16];
	double total;
	size_transformation(&total, unit, tbytes);
	sprintf(byte_info, "\ttotal: %.2f %s (%lld bytes)", total, unit, tbytes);

	if (estimatedbytes > 0)
	  append_eta(byte_info, bw, tbytes);

	/* A check for just being pedantic. info[] is long enough always. */
	if ((strlen(info) + strlen(byte_info) + 1) <= sizeof(info)) {
	  strcat(info, byte_info);
	  write_info(info);
	}
      }

    } else if (ret == 0) {
      if (n_files > 0) {
	if (open_new_file() == 0)
	  end_program = 1;
      } else {
	end_program = 1;
      }

    } else {
      if (errno == EAGAIN) {
	if (wait_input())
	  end_program = 1;

      } else if (errno != EINTR) {
	perror("pmr: Read error");
	end_program = 1;
      }
    }
  }

  do {
    long long bytes = tbytes;
    unsigned char md5[16];
    double total;
    char unit[16];

    fprintf(stderr, "                                                     \r");

    if (program_interrupted) {
      fprintf(stderr, "Program interrupted%s\n",
	      use_md5 ? " -> no MD5 sum" : "");
    }

    timetest(&bw, info, sizeof(info), &vot, &bytes, 1);
    fprintf(stderr, "average %s\n", info);

    size_transformation(&total, unit, tbytes);

    fprintf(stderr, "total: %.2f %s (%lld bytes)\n", total, unit, tbytes);

    if (use_md5 && program_interrupted == 0) {
      MD5Final(md5, &md5ctx);
      fprintf(stderr, "md5sum: %.2x%.2x%.2x%.2x%.2x%.2x%.2x%.2x%.2x%.2x%.2x%.2x%.2x%.2x%.2x%.2x\n",md5[0],md5[1],md5[2],md5[3],md5[4],md5[5],md5[6],md5[7],md5[8],md5[9],md5[10],md5[11],md5[12],md5[13],md5[14],md5[15]);
    }
  } while (0);

  free(real_buf);
  close(input_fd);
  close(1);
  return program_interrupted ? EXIT_FAILURE : EXIT_SUCCESS;
}
