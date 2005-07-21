/* pmr by Heikki Orsila <heikki.orsila@iki.fi> (2003.12.28)
   This source is public domain. Do what you want with it.

   Get latest version of pmr from: http://www.iki.fi/shd/foss/pmr/

   Pmr displays the bandwidth of the pipe going through the process.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <sys/time.h>

#include <errno.h>

#define VERSION "0.09"

#define BUFFER_SIZE 8192

extern int errno;

static int end_program = 0;
static int default_interval = 2000;

static int max_rate = -1;
static int rate_read_bytes = 0;
struct timeval rate_time;

static void sh(int sig)
{
  sig = sig;
  end_program = 1;
}


static int timetest(char *s, struct timeval *ot, long long *bytes, int force)
{
  struct timeval nt;
  int t;
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
    int order;
    double bw;
    char id[256];
    if (t) {
      bw = ((double) *bytes) * 1000.0f / ((double) t);
      order = 0;
      while (bw >= 1024.0f) {
	bw /= 1024.0f;
	order++;
	if (order == 4)
	  break;
      }
      switch (order) {
      case 0:
	strcpy(id, "B");
	break;
      case 1:
	strcpy(id, "KiB");
	break;
      case 2:
	strcpy(id, "MiB");
	break;
      case 3:
	strcpy(id, "GiB");
	break;
      case 4:
	strcpy(id, "TiB");
	break;
      default:
	fprintf(stderr, "pmr: a bug in number order!\n");
	strcpy(id, "Strange Unit");
	break;
      }
      sprintf(s, "bandwidth: %.2f %s/s", bw, id);
    }
    *ot = nt;
    *bytes = 0;
    return 1;
  }
  return 0;
}

int read_no_rate_limit(char *buf, int size)
{
  return read(0, buf, size);
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
    ret = read(0, buf + read_bytes , to_read - read_bytes);
    if (ret > 0) {
      read_bytes += ret;
    } else if (ret == 0) {
      break;
    } else {
      if (errno != EINTR)
	return -1;
    }
  }
  rate_read_bytes += read_bytes;
  return read_bytes;
}


int main(int argc, char **argv)
{
  int aligned_size = BUFFER_SIZE;
  int carriage_return = 0;
  char *real_buf;
  char *aligned_buf;
  struct sigaction act;
  struct timeval ot, vot;
  int i, ret, readoffs, rbytes;
  long long wbytes;
  long long tbytes;
  int poke_mem = 0;
  char info[256];
  int valid_time = 1;
  int (*read_function)(char *buf, int size) = read_no_rate_limit;

  for (i = 1; i < argc;) {
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
        max_rate = atoi(argv[i + 1]);
	if (max_rate <= 0) {
	  fprintf (stderr, "illegal bytes per second value (%d)\n", max_rate);
	  return -1;
	}
	read_function = read_rate_limit;
      } else {
        fprintf (stderr, "expecting a value for bandwidth limit (bytes per second)\n");
        return -1;
      }
      i += 2;
      continue;
    }

    if (!strcmp(argv[i], "-r")) {
      carriage_return = 1;
      i++;
      continue;
    }
    if (!strcmp(argv[i], "-p")) {
      poke_mem = 1;
      i++;
      continue;
    }
    if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
      fprintf(stderr, "pmr %s: usage:\n\n", VERSION);
      fprintf(stderr, " %s [-l Bps] [-t seconds] [-p] [-b size] [-r] [-h/--help] [-v]\n\n", argv[0]);
      fprintf(stderr, " -b size\tset input buffer size (default %d)\n", BUFFER_SIZE);
      fprintf(stderr, " -l Bps\t\tlimit throughput to 'Bps' bytes per second\n");
      fprintf(stderr, " -p\t\tenables 4k page poking (useless)\n");
      fprintf(stderr, " -r\t\tuse carriage return on output, no newline\n");
      fprintf(stderr, " -t secs\tupdate interval in seconds\n");
      fprintf(stderr, " -v\t\tprint version, about, contact and home page information\n");
      return 0;
    }
    if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--version")) {
      fprintf(stderr, "pmr %s by Heikki Orsila <heikki.orsila@iki.fi>\n", VERSION);
      fprintf(stderr, "This program is public domain.\n");
      fprintf(stderr, "You can get the latest version of the program from:\n");
      fprintf(stderr, "\n  http://www.iki.fi/shd/foss/pmr/\n\n");
      return 0;
    }
    fprintf(stderr, "unknown args: %s\n", argv[i]);
    return -1;
  }

#ifndef PAGE_SIZE
#define PAGE_SIZE 8192
#endif

  /* get page size aligned buffer of size 'aligned_size' */
  real_buf = malloc(aligned_size + PAGE_SIZE);
  if (!real_buf) {
    fprintf(stderr, "pmr: not enough memory\n");
    exit(-1);
  }
  aligned_buf = real_buf;
  aligned_buf += PAGE_SIZE - (((long) aligned_buf) & (PAGE_SIZE - 1));

  memset(&act, 0, sizeof(act));
  act.sa_handler = sh;
  sigaction(SIGPIPE, &act, 0);

  if (gettimeofday(&ot, 0)) {
    memset(&ot, 0, sizeof(ot));
    perror ("pmr: gettimeofday failed");
    valid_time = 0;
  }
  vot = ot;
  rate_time = ot; /* useless if max_rate was not set */

  wbytes = 0;
  tbytes = 0;

  while (!end_program) {

    ret = read_function(aligned_buf, aligned_size);
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
	  if (errno != EINTR) { 
	    perror("pmr: write error");
	    end_program = 1;
	    break;
	  }
	}
      }
      if (valid_time && timetest(info, &ot, &wbytes, 0)) {
	char byte_info[256];
	sprintf(byte_info, "\tbytes: %lld", tbytes);
	strcat(info, byte_info);

	if (carriage_return) {
	  fprintf(stderr, "                                                     \r");
	  fprintf(stderr, "%s\r", info);
	} else {
	  fprintf(stderr, "%s\n", info);
	}
      }

    } else if (ret == 0) {
      end_program = 1;
    } else {
      if (errno != EINTR) {
	perror("pmr: read error");
	end_program = 1;
      }
    }
  }

  do {
    long long bytes = tbytes;
    timetest(info, &vot, &bytes, 1);
    fprintf(stderr, "                                                     \r");
    fprintf(stderr, "average %s\n", info);
    fprintf(stderr, "total bytes: %lld\n", tbytes);
  } while (0);

  free(real_buf);
  close(0);
  close(1);
  return 0;
}
