/* pmr by Heikki Orsila <heikki.orsila@iki.fi>
 *
 * This source is in public domain. You may do anything with it.
 *
 * Get latest version of pmr from: http://www.iki.fi/shd/foss/pmr/
 *
 * pmr displays the bandwidth of the pipe going through the process, and
 * does several other things.
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
#include <ctype.h>

#include "md5.h"
#include "support.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define VERSION "1.01"

#define DEFAULT_BUFFER_SIZE 8192
static size_t buffer_size = DEFAULT_BUFFER_SIZE;

#ifndef PAGE_SIZE
#define PAGE_SIZE 8192
#endif

#define RATE_QUANTUM 50                     /* in milliseconds */
#define MAX_RATE_WINDOW_SIZE 250            /* in milliseconds */

struct range {
	int valid;
	long long start;
	long long end;
};

struct pipe {
	int id;

	int terminated;

	int in_fd;
	int out_fd;

	char *real_buf;
	char *aligned_buf;

	int readoffs;
	long long rbytes;	/* bytes read */
	long long wbytes;	/* bytes written */

	struct timeval measurement_time;
	long long measurement_bytes;

	long long max_rate;		/* -1 if no rate is not used */
	long long effective_rate;       /* quota for a single time quantum */
	int valid_time;
	struct timeval rate_time;
	unsigned int rate_quota; /* measures bytes to read for rate limiting */

	MD5_CTX md5ctx;

	struct range range;
};


static void initialize_pipe(int id, int in_fd, int out_fd);
static double inverse_size_transformation(const char *valuestr);
static int open_new_file(struct pipe *p);
static int read_no_rate_limit(struct pipe *p);
static int read_rate_limit(struct pipe *p);
static void size_transformation(double *size, char *unit, double srcsize);
static int timetest(double *bw, char *s, size_t maxlen, struct pipe *p,
		    int force);
static void write_info(const char *info);

static int terminated_pipes;
static int program_return_value = EXIT_SUCCESS;
static int childpipe[2] = { -1, -1 };

static int default_interval = 2000; /* time in milliseconds */

static int use_md5;

static int default_max_rate = -1;

static int n_pipes;

static struct pipe pipes[2];

static size_t n_files;
static size_t n_files_read;
static size_t n_files_allocated;
static char **files;
static long long estimatedbytes;

#define SPEED_WINDOW_SIZE 10
static double speed_window[SPEED_WINDOW_SIZE];

#define SPACE_LENGTH 80
static char space[SPACE_LENGTH];
static int old_line_length;
static int use_carriage_return;

#define NUNITS (9)

static const char *iec_units[NUNITS] = { "B", "KiB", "MiB", "GiB", "TiB",
	"PiB", "EiB", "ZiB", "YiB"
};

static const char *si_units[NUNITS] = { "B", "kB", "MB", "GB", "TB",
	"PB", "EB", "ZB", "YB"
};

static const char *bit_units[NUNITS] = { "b", "kbit", "Mbit", "Gbit", "Tbit",
	"Pbit", "Ebit", "Zbit", "Ybit"
};

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

static void child_handler(int sig)
{
	char token = 0;
	write(childpipe[1], &token, 1);
}

static void close_pipe(struct pipe *p)
{
	terminated_pipes++;
	p->terminated = 1;

	/* Close output fd to cause EOF for the spawned process */
	close(p->out_fd);
	p->out_fd = -1;
}

static void ctrlc_and_pipe_handler(int sig)
{
	if (sig == SIGINT)
		program_return_value = 1;

	terminated_pipes = 2;
}

static int get_bw_limit(const char *limit)
{
	double value = inverse_size_transformation(limit);

	if (value < 1) {
		die("Invalid bytes per second value (%s -> %lf)\n",
		    limit, value);
	} else if (value >= 2147483648UL) {
		die("Too high bytes per second value (%s -> %lf)\n",
		    limit, value);
	}

	return (int)value;
}

static void get_range(struct range *r, const char *parameter)
{
	char *delimiter, *s;
	double start, end;

	s = strdup(parameter);
	if (s == NULL)
		die("Not enough memory for range parameter.\n");

	delimiter = strchr(s, ':');
	if (delimiter == NULL)
		die("Invalid range parameter, missing : character\n");

	*delimiter = 0;

	start = inverse_size_transformation(s);
	end = inverse_size_transformation(delimiter + 1);

	if (start < 0) {
		if (s[0] == 0) {
			start = 0;
		} else {
			die("Invalid beginning of range %s\n", parameter);
		}
	}

	if (end < 0) {
		if (strcasecmp(delimiter + 1, "inf") == 0 || delimiter[1] == 0) {
			end = -1;
		} else {
			die("Invalid end of range %s\n", parameter);
		}
	}

	if (start >= 0 && end >= 0 && end < start)
		die("Range end must be at least the same as start\n");

	r->start = start;
	r->end = end;
	r->valid = 1;

	free(s);
}

static ssize_t write_to_pipe(struct pipe *p, const char *buf, size_t count)
{
	ssize_t ret;

	if (p->range.valid) {

		/* Notice that because range.end >= initial range.start, we will
		   never handle both the start and the end range here */

		if (p->range.start > 0) {
			/* Skip bytes */
			ret = MIN(p->range.start, count);
			p->range.start -= ret;
			return ret;
		}

		if (p->range.end > 0) {
			/* Test write end range */
			if (p->wbytes + count >= p->range.end) {
				/* Partial write */
				size_t maxwrite = p->range.end - p->wbytes;

				ret = write(p->out_fd, buf, maxwrite);

				/* If end range is reached, close the pipe */
				if (ret == maxwrite)
					close_pipe(p);

				return ret;
			}
		}
	}

	/* The normal case: full write */
	return write(p->out_fd, buf, count);
}

static void handle_pipe(struct pipe *p)
{
	ssize_t ret;
	void *rdata;
	double bw;
	char info[256];
	size_t towrite;

	if (p->rbytes == p->wbytes) {

		if (p->range.valid && p->range.end >= 0 &&
		    (p->rbytes == p->range.end)) {
			ret = 0;
		} else if (p->max_rate == -1) {
			ret = read_no_rate_limit(p);
		} else {
			ret = read_rate_limit(p);
		}

		/* Note, we will try to write all data received so far even if the
		   program has been aborted (SIGINT) */

		if (ret > 0) {
			p->rbytes += ret;
			p->readoffs = 0;

		} else if (ret == 0) {
			/* Only pipe 0 can have regular files queued for it */
			if (p->id != 0 || open_new_file(p) == 0)
				close_pipe(p);

		} else if (errno != EAGAIN && errno != EINTR) {
			perror("pmr: Read error");
			close_pipe(p);
			program_return_value = 1;
		}

	} else {

		towrite = p->rbytes - p->wbytes;
		rdata = &p->aligned_buf[p->readoffs];

		ret = write_to_pipe(p, rdata, towrite);

		if (ret > 0) {
			p->wbytes += ret;
			p->measurement_bytes += ret;
			p->readoffs += ret;

			if (use_md5)
				MD5Update(&p->md5ctx, (unsigned char *)p->aligned_buf, ret);

		} else if (ret == 0) {
			fprintf(stderr, "pmr: interesting: write returned 0\n");
			close_pipe(p);
			return;

		} else if (errno != EINTR && errno != EAGAIN) {
			perror("pmr: Write error");
			close_pipe(p);
			program_return_value = 1;
			return;
		}

		/* We only measure things for pipe 0 */
		if (p->id == 0 && p->valid_time &&
		    timetest(&bw, info, sizeof(info), p, 0)) {

			char byte_info[256];
			char unit[16];
			double total;

			size_transformation(&total, unit, p->wbytes);
			sprintf(byte_info, "\ttotal: %.2f %s (%lld bytes)",
				total, unit, p->wbytes);

			if (estimatedbytes > 0)
				append_eta(byte_info, bw, p->wbytes);

			/* A check for just being pedantic. info[] is always long enough */
			if ((strlen(info) + strlen(byte_info) + 1) <=
			    sizeof(info)) {
				strcat(info, byte_info);
				write_info(info);
			}
		}
	}
}

static void initialize_pipe(int id, int in_fd, int out_fd)
{
	struct pipe *p;

	p = &pipes[id];

	memset(p, 0, sizeof(p[0]));

	p->id = id;

	p->in_fd = in_fd;
	p->out_fd = out_fd;

	p->max_rate = default_max_rate;
	p->effective_rate = MAX(1, (p->max_rate * RATE_QUANTUM) / 1000);

	/* get page size aligned buffer of size 'aligned_size' */
	p->real_buf = malloc(buffer_size + PAGE_SIZE);
	if (!p->real_buf)
		die("Not enough memory\n");

	p->aligned_buf = p->real_buf;
	p->aligned_buf +=
	    PAGE_SIZE - (((long)p->aligned_buf) & (PAGE_SIZE - 1));

	p->valid_time = 1;
	if (gettimeofday(&p->rate_time, NULL)) {
		perror("pmr: gettimeofday() failed");
		p->valid_time = 0;
	}

	p->measurement_time = p->rate_time;

	MD5Init(&p->md5ctx);

	n_pipes++;
}

static double inverse_size_transformation(const char *valuestr)
{
	char *endptr;
	double value;
	double multiplier;
	int i;

	value = strtod(valuestr, &endptr);

	if (endptr == valuestr)
		return -1.0;

	while (*endptr != 0 && isspace(*endptr))
		endptr++;

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
		if (strncmp(endptr, si_units[i], 2) == 0)
			return multiplier * value;
		multiplier *= 1000.0;
	}

	/* case-sensitive comparisons for bits with SI multipliers */
	multiplier = 1.0 / 8;
	for (i = 0; i < NUNITS; i++) {
		if (strncmp(endptr, bit_units[i], 2) == 0)
			return multiplier * value;
		multiplier *= 1000.0;
	}

	die("Unknown unit: %s\n", endptr);
	/* never reached */
	return 0.0;
}

static int open_new_file(struct pipe *p)
{
	char *fname;

	if (n_files_read == n_files)
		return 0;

	close(p->in_fd);

	fname = files[n_files_read++];

	p->in_fd = open(fname, O_RDONLY);
	if (p->in_fd < 0)
		dieerror("Error opening file %s", fname);

	return 1;
}

static const char *USAGE =
"pmr %s by Heikki Orsila <heikki.orsila@iki.fi>\n"
"\n"
"Usage:\n"
"\n"
" pmr [-l Bps] [-s size] [-m] [-t seconds] [-b size] [-i R] [-o R] [-r] [-v] [-e com args... | FILE ..]\n"
"\n"
" -b size\tSet input buffer size (default %d)\n"
"\n"
" -e com args..\tRun command \"com\" with args, copy stdin of pmr to the stdin of\n"
"\t\tthe command, and copy stdout of the command to the stdout of\n"
"\t\tpmr. pmr measures the data rate from stdin to the executed\n"
"\t\tcommand. Note that this option changes the behavior of -i.\n"
"\n"
" -i R / --input-range R    Limit copying of data from stdin by argument R.\n"
"                R is in format x:y, where x is the number of bytes\n"
"                to be skipped in the beginning of standard input. y is the\n"
"                total number of bytes to be read from standard input.\n"
"\n"
"                Either x or y may be omitted. With \"-i x:\", x bytes will be\n"
"                skipped. With \"-i :y\", only y bytes will be read.\n"
"\n"
"                One may use IEC units with x and y, e.g. \"-i 1KiB:\".\n"
"\n"
"                In -e mode this limits data that is copied to the external\n"
"                process.\n"
"\n"
" -l Bps\t\tLimit throughput to 'Bps' bytes per second. It is also\n"
"\t\tpossible to use SI, IEC 60027 and bit units in the value.\n"
"\t\tSI units include kB, MB, ..., IEC units include KiB, MiB, ...\n"
"\t\tand bit units include kbit, Mbit, ...\n"
"\n"
" -m / --md5\tCompute an md5 checksum of the stream (useful for verifying\n"
"\t\tdata integrity through TCP networks)\n"
"\n"
" -o R / --output-range R    Similar to --i / --input-range, but only has an\n"
"                effect in -e mode. This can be used to skip data from the\n"
"                executed process, and limit the total number of data written\n"
"                to standard output.\n"
"\n"
" -r\t\tUse carriage return on output, no newline\n"
"\n"
" -s size\tCalculate ETA given a size estimate. Giving regular files\n"
"\t\tas pmr parameters implies -s SUM, where SUM is the total\n"
"\t\tsize of those files.\n"
"\n"
" -t secs\tUpdate interval in seconds\n"
"\n"
" -v\t\tPrint version, about, contact and home page information\n";

static void print_usage(const char *pname)
{
	fprintf(stderr, USAGE, VERSION, DEFAULT_BUFFER_SIZE);
}

static void read_config(void)
{
	char *home;
	char cfilename[PATH_MAX];
	FILE *cfile;
	char line[256];
	char *word, *opt;
	int t;
	int ret;

	/* First try home directory */
	home = getenv("HOME");
	if (home == NULL)
		return;

	snprintf(cfilename, sizeof cfilename, "%s/.pmr", home);

	cfile = fopen(cfilename, "r");
	if (cfile == NULL) {

		/* Then try /etc directory */
		snprintf(cfilename, sizeof cfilename, "/etc/pmr");

		cfile = fopen(cfilename, "r");
		if (cfile == NULL)
			return;
	}

	while (1) {
		if (fgets(line, sizeof line, cfile) == NULL) {
			if (feof(cfile))
				break;
			continue;
		}

		if (line[0] == '#')
			continue;	/* comment line */

		ret = skipws(line, 0);
		if (ret < 0)
			continue;	/* empty line */

		word = &line[ret];

		/* Get second option from the line, if exists */
		opt = NULL;
		ret = skipnws(line, ret);
		if (ret > 0) {
			ret = skipws(line, ret);
			if (ret > 0) {
				/* We have an option. zero-terminate it. */
				opt = &line[ret];
				ret = skipnws(opt, 0);
				if (ret > 0)
					opt[ret] = 0;
			}
		}

		if (strncasecmp(word, "carriage_return", 8) == 0) {
			use_carriage_return = 1;

		} else if (strncasecmp(word, "limit", 5) == 0) {
			if (opt == NULL) {
				fprintf(stderr, "Missing limit value\n");
				continue;
			}
			default_max_rate = get_bw_limit(opt);

		} else if (strncasecmp(word, "md5", 3) == 0) {
			use_md5 = 1;

		} else if (strncasecmp(word, "update_interval", 3) == 0) {
			if (opt == NULL) {
				fprintf(stderr, "Missing update interval\n");
				continue;
			}
			t = atoi(opt);
			assert(t >= 0 && t < INT_MAX / 1000);
			default_interval = 1000 * t;

		} else {
			fprintf(stderr, "Unknown configuration directive: %s\n",
				word);
		}
	}

	fclose(cfile);
}

static int read_no_rate_limit(struct pipe *p)
{
	return read(p->in_fd, p->aligned_buf, buffer_size);
}

/* Return time difference in milliseconds. Can be negative. */
static int time_delta(struct timeval *new, struct timeval *old)
{
	return 1000 * (new->tv_sec - old->tv_sec) +
		((int) new->tv_usec - (int) old->tv_usec) / 1000;
}

static int read_rate_limit(struct pipe *p)
{
	int ret;
	int tdelta;
	int to_read;
	int read_bytes;
	struct timeval new_rate_time;

	if (gettimeofday(&new_rate_time, NULL)) {
		perror("pmr: gettimeofday() failed. Can not limit rate. "
		       "Going max speed.");
		p->max_rate = -1;
		return read_no_rate_limit(p);
	}

	if (p->rate_quota == 0) {
		tdelta = time_delta(&new_rate_time, &p->rate_time);

		if (tdelta < 0) {
			tdelta = 0; /* time ran backwards */

		} else if (tdelta < RATE_QUANTUM) {
			usleep(1000 * (RATE_QUANTUM - tdelta));

			if (gettimeofday(&new_rate_time, NULL)) {
				perror("pmr: gettimeofday failed. Can not "
				       "limit rate. Going max speed.");
				p->max_rate = -1;
				return read_no_rate_limit(p);
			}

			tdelta = time_delta(&new_rate_time, &p->rate_time);
			if (tdelta < 0)
				tdelta = 0;
		}

		tdelta = MIN(tdelta, MAX_RATE_WINDOW_SIZE);

		p->rate_quota += (p->effective_rate * tdelta) / RATE_QUANTUM;

		p->rate_time = new_rate_time;
	}

	to_read = MIN(p->rate_quota, buffer_size);
	to_read = MAX(to_read, 1); /* read at least 1 byte to avoid EOF */

	read_bytes = 0;

	while (read_bytes < to_read) {

		ret = read(p->in_fd, p->aligned_buf + read_bytes,
			   to_read - read_bytes);
		if (ret <= 0) {
			/* If 0 bytes read so far and an error happens,
			   handle it on a higher level */
			if (ret < 0 && read_bytes == 0)
				return -1;

			break;
		}

		read_bytes += ret;
	}

	p->rate_quota -= read_bytes;

	return read_bytes;
}

static inline void set_fd(int *maxfd, int fd, fd_set * set)
{
	if (*maxfd < fd)
		*maxfd = fd;

	FD_SET(fd, set);
}

static void size_transformation(double *size, char *unit, double srcsize)
{
	int order;

	if (srcsize < 0)
		die("Negative size in size_transformation()\n");

	for (order = 0; order < NUNITS; order++) {
		if (srcsize < 1024.0)
			break;
		srcsize /= 1024.0;
	}

	if (order == NUNITS) {
		fprintf(stderr,
			"pmr warning: too high a number for size_transformation()r\n");
		order = NUNITS - 1;
	}

	*size = srcsize;
	strcpy(unit, iec_units[order]);
}

static void spawn_process(const char **args)
{
	int toprocess[2];
	int fromprocess[2];

	if (pipe(toprocess) || pipe(fromprocess))
		dieerror("Can not pipe() for -e");

	if (fork() == 0) {

		close(toprocess[1]);
		close(fromprocess[0]);

		if (dup2(toprocess[0], 0) < 0 || dup2(fromprocess[1], 1) < 0)
			dieerror("Can not dup for -e");

		execvp(args[0], (char *const *)args);

		dieerror("Can not execute %s", args[0]);
	}

	close(toprocess[0]);
	close(fromprocess[1]);

	/* pipe 0: pmr stdin -> process stdin */
	pipes[0].out_fd = toprocess[1];

	/* pipe 1: process stdout -> pmr stdout */
	initialize_pipe(1, fromprocess[0], 1);
}

static int timetest(double *bw, char *s, size_t maxlen, struct pipe *p,
		    int force)
{
	struct timeval nt;
	struct timeval *ot = &p->measurement_time;
	int t;

	*bw = 0.0;

	strcpy(s, "bandwidth: NaN");

	if (gettimeofday(&nt, NULL)) {
		/* time failed. bandwidth = NaN. return false. */
		return 0;
	}

	t = 1000 * (nt.tv_sec - ot->tv_sec) + ((int)nt.tv_usec) / 1000 -
	    ((int)ot->tv_usec) / 1000;

	if (t < 0) {
		fprintf(stderr, "pmr: chronoton particles detected. clock ran "
			"backwards. k3wl!\n");
		t = default_interval + 1;
	}

	if (t > default_interval || force) {

		if (t) {
			double canonical_bw, real_bw;
			char id[16];

			real_bw = 1000.0 * p->measurement_bytes / t;

			*bw = real_bw;

			size_transformation(&canonical_bw, id, real_bw);
			snprintf(s, maxlen, "bandwidth: %.2f %s/s",
				 canonical_bw, id);
		}

		*ot = nt;
		p->measurement_bytes = 0;

		return 1;
	}

	return 0;
}

static void write_info(const char *info)
{
	if (use_carriage_return) {

		/* Writes length amount of spaces and then
		   a carriage return (\r) */
		assert(old_line_length >= 0);

		while (old_line_length >= SPACE_LENGTH) {
			fwrite(space, 1, SPACE_LENGTH, stderr);
			old_line_length -= SPACE_LENGTH;
		}

		fwrite(space, 1, old_line_length, stderr);

		fprintf(stderr, "\r");

		old_line_length = 0;

		if (info != NULL) {
			/* Write info */
			old_line_length = fprintf(stderr, "%s\r", info) - 1;

			/* In case stderr is lost (how??) */
			if (old_line_length < 0)
				old_line_length = 0;
		}

	} else {
		if (info != NULL) {
			/* Write info */
			fprintf(stderr, "%s\n", info);
		}
	}
}

static void setup_signal_handlers(void)
{
	struct sigaction act;

	memset(&act, 0, sizeof act);
	act.sa_handler = ctrlc_and_pipe_handler;

	if (sigaction(SIGPIPE, &act, NULL))
		die("Can not set SIGPIPE\n");

	if (sigaction(SIGINT, &act, NULL))
		die("Can not set SIGINT\n");

	memset(&act, 0, sizeof act);
	act.sa_handler = child_handler;

	if (sigaction(SIGCHLD, &act, NULL))
		die("Can not set SIGCHLD\n");
}

int main(int argc, char **argv)
{
	int valid_time;
	struct timeval vot;

	int i, ret;
	int no_options = 0;
	double presize = 0.0;
	int exec_arg_i = -1;

	struct range rangeparameters[2] = {{.valid = 0},
					   {.valid = 0}};

	setup_signal_handlers();

	read_config();

	for (i = 1; i < argc;) {

		if (no_options || argv[i][0] != '-') {

			char *fname = argv[i];
			struct stat st;

			if (stat(fname, &st)) {
				fprintf(stderr, "Can not stat %s. Ignoring.\n",
					fname);
				i++;
				continue;
			}

			if (!S_ISREG(st.st_mode)) {
				fprintf(stderr, "%s is not a regular file "
					"-> no ETA estimation\n", fname);
				estimatedbytes = -1;
			}

			if (estimatedbytes >= 0)
				estimatedbytes += st.st_size;

			/* Build a regular file queue */
			darray_append(n_files, n_files_allocated, files, fname);

			no_options = 1;
			i++;
			continue;
		}

		if (!strcmp(argv[i], "-b")) {
			if ((i + 1) >= argc)
				die("Expecting a value for -b\n");

			buffer_size = atoi(argv[i + 1]);
			i += 2;
			continue;
		}

		if (!strcmp(argv[i], "-e") || !strcmp(argv[i], "--exec")) {
			if ((i + 1) >= argc)
				die("Too few arguments for -e\n");

			/* store the index of command and its args */
			exec_arg_i = i + 1;
			break;
		}

		if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
			print_usage(argv[0]);
			exit(0);
		}

		if (!strcmp(argv[i], "-i") || !strcmp(argv[i], "--input-range")) {
			if ((i + 1) >= argc)
				die("Not enough args. Missing range.\n");

			get_range(&rangeparameters[0], argv[i + 1]);
			i += 2;
			continue;
		}

		if (!strcmp(argv[i], "-l")) {
			if ((i + 1) >= argc) {
				die("Expecting a value for bandwidth limit"
				    " (bytes per second)\n");
			}

			default_max_rate = get_bw_limit(argv[i + 1]);
			i += 2;
			continue;
		}

		if (!strcmp(argv[i], "-m") || !strcmp(argv[i], "--md5")) {
			use_md5 = 1;
			i++;
			continue;
		}

		if (!strcmp(argv[i], "-o") || !strcmp(argv[i], "--output-range")) {
			if ((i + 1) >= argc)
				die("Not enough args. Missing range.\n");

			get_range(&rangeparameters[1], argv[i + 1]);
			i += 2;
			continue;
		}

		if (!strcmp(argv[i], "-r")) {
			use_carriage_return = 1;
			memset(space, 0, SPACE_LENGTH);
			i++;
			continue;
		}

		if (!strcmp(argv[i], "-s")) {
			if ((i + 1) >= argc)
				die("Not enough args. Missing size estimate.\n");

			presize = inverse_size_transformation(argv[i + 1]);

			if (presize < 0.0) {
				fprintf(stderr,	"Braindamaged size estimate: "
					"%f.\n", presize);
				presize = 0.0;
			}

			i += 2;
			continue;
		}

		if (!strcmp(argv[i], "-t")) {
			if ((i + 1) >= argc)
				die("Expecting a value for -t\n");

			default_interval = 1000 * atoi(argv[i + 1]);
			i += 2;
			continue;
		}

		if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--version")) {
			printf("pmr %s\n", VERSION);
			exit(0);
		}

		if (!strcmp(argv[i], "--")) {
			no_options = 1;
			i++;
			continue;
		}

		die("Unknown arg: %s\n", argv[i]);
	}

	initialize_pipe(0, 0, 1);

	if (exec_arg_i > 0) {
		/* Create a pipe to terminate select() in main loop from child
		   signal handler */
		if (pipe(childpipe))
			dieerror("Can not create a pipe for SIGCHLD");

		/* Pipe 1 is initialized here */
		spawn_process((const char **) &argv[exec_arg_i]);

		/* Use non-blocking IO (select()) with 2 pipes */
		for (i = 0; i < n_pipes; i++) {
			fcntl(pipes[i].in_fd, F_SETFL, O_NONBLOCK);
			fcntl(pipes[i].out_fd, F_SETFL, O_NONBLOCK);
		}
	}

	/* Record creation time of pipe 0 to determine total run-time of pmr */
	valid_time = pipes[0].valid_time;
	vot = pipes[0].measurement_time;

	/* For ETA calculation when size was given with -s. This overrides
	   byte size computed for regular file queue. */
	if (presize > 0.0)
		estimatedbytes = presize;

	/* Open the first file in the regular file queue */
	if (n_files > 0)
		open_new_file(&pipes[0]);

	/* Set pipe ranges */
	if (rangeparameters[0].valid)
		pipes[0].range = rangeparameters[0];

	if (rangeparameters[1].valid) {
		if (n_pipes == 1)
			die("Output range may only be used in -e mode\n");

		pipes[1].range = rangeparameters[1];
	}

	while (terminated_pipes < n_pipes) {
		fd_set rfds, wfds;
		int maxfd;

		/* Optimise the single pipe case (uses blocking IO) */
		if (n_pipes == 1) {
			handle_pipe(&pipes[0]);
			continue;
		}

		FD_ZERO(&rfds);
		FD_ZERO(&wfds);

		maxfd = 0;

		set_fd(&maxfd, childpipe[0], &rfds);

		if (!pipes[0].terminated) {
			if (pipes[0].wbytes == pipes[0].rbytes)
				set_fd(&maxfd, pipes[0].in_fd, &rfds);
			else
				set_fd(&maxfd, pipes[0].out_fd, &wfds);
		}

		if (!pipes[1].terminated) {
			if (pipes[1].wbytes == pipes[1].rbytes)
				set_fd(&maxfd, pipes[1].in_fd, &rfds);
			else
				set_fd(&maxfd, pipes[1].out_fd, &wfds);
		}

		ret = select(maxfd + 1, &rfds, &wfds, NULL, NULL);

		if (ret < 0 && errno != EINTR)
			dieerror("select() error");

		if (FD_ISSET(childpipe[0], &rfds))
			close_pipe(&pipes[0]);

		if (!pipes[0].terminated) {
			if (FD_ISSET(pipes[0].in_fd, &rfds) ||
			    FD_ISSET(pipes[0].out_fd, &wfds)) {
				handle_pipe(&pipes[0]);
			}
		}

		if (!pipes[1].terminated) {
			if (FD_ISSET(pipes[1].in_fd, &rfds) ||
			    FD_ISSET(pipes[1].out_fd, &wfds)) {
				handle_pipe(&pipes[1]);
			}
		}
	}

	do {
		unsigned char md5[16];
		double total;
		char unit[16];
		char info[256];
		double bw;

		write_info(NULL);

		if (program_return_value) {
			fprintf(stderr, "Unexpected termination%s\n",
				use_md5 ? " -> no MD5 sum" : "");
		}

		if (valid_time) {
			/* Print average speed in human-readable form */
			pipes[0].measurement_time = vot;
			pipes[0].measurement_bytes = pipes[0].wbytes;

			timetest(&bw, info, sizeof info, &pipes[0], 1);

			fprintf(stderr, "average %s\n", info);
		}

		/* Print total data size in a human-readable form */
		size_transformation(&total, unit, pipes[0].wbytes);

		fprintf(stderr, "total: %.2f %s (%lld bytes)\n", total, unit,
			pipes[0].wbytes);

		if (use_md5 && program_return_value == 0) {
			MD5Final(md5, &pipes[0].md5ctx);
			fprintf(stderr,
				"md5sum: %.2x%.2x%.2x%.2x%.2x%.2x%.2x%.2x%.2x%.2x%.2x%.2x%.2x%.2x%.2x%.2x\n",
				md5[0], md5[1], md5[2], md5[3], md5[4], md5[5],
				md5[6], md5[7], md5[8], md5[9], md5[10],
				md5[11], md5[12], md5[13], md5[14], md5[15]);
		}
	} while (0);

	close(1);

	return program_return_value;
}
