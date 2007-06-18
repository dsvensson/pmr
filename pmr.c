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
#include <ctype.h>

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
        exit(1); \
      } \
    } \
      \
    (array)[(n)] = (item); \
    (n)++; \
  } while (0)


#define VERSION "0.13"

#define DEFAULT_BUFFER_SIZE 8192
static size_t buffer_size = DEFAULT_BUFFER_SIZE;


#ifndef PAGE_SIZE
#define PAGE_SIZE 8192
#endif


struct pipe {
    int id;

    int terminated;

    int in_fd;
    int out_fd;

    char *real_buf;
    char *aligned_buf;

    int readoffs;
    long long rbytes; /* bytes read */
    long long wbytes; /* bytes written */

    struct timeval measurement_time;
    long long measurement_bytes;

    int max_rate; /* -1 if no rate is not used */
    int valid_time;
    struct timeval rate_time;
    int rate_read_bytes;

    MD5_CTX md5ctx;
};


static void initialize_pipe(int id, int in_fd, int out_fd);
static double inverse_size_transformation(const char *valuestr);
static int open_new_file(struct pipe *p);
static int read_no_rate_limit(struct pipe *p);
static int read_rate_limit(struct pipe *p);
static void size_transformation(double *size, char *unit, double srcsize);
static int skip_ws(const char *line, int ind);
static int skip_nws(const char *line, int ind);
static int timetest(double *bw, char *s, size_t maxlen, struct pipe *p,
		    int force);
static void write_info(const char *info);


extern int errno;

static int terminated_pipes;
static int program_interrupted;

static int default_interval = 2000;

static int use_md5;

static int default_max_rate = -1;

static int n_pipes;

struct pipe pipes[2];

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


#define NUNITS (9)

static const char *iec_units[NUNITS] = {"B", "KiB", "MiB", "GiB", "TiB",
					"PiB", "EiB", "ZiB", "YiB"};

static const char *si_units[NUNITS] = {"B", "kB", "MB", "GB", "TB",
				       "PB", "EB", "ZB", "YB"};

static const char *bit_units[NUNITS] = {"b", "kbit", "Mbit", "Gbit", "Tbit",
					"Pbit", "Ebit", "Zbit", "Ybit"};


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

    terminated_pipes = 2;
}


static int get_bw_limit(const char *limit)
{
    double value = inverse_size_transformation(limit);

    if (value < 1) {
	fprintf(stderr, "illegal bytes per second value (%s)\n", limit);
	exit(1);
    } else if (value >= 2147483648UL) {
	fprintf(stderr, "too high bytes per second value (%s)\n", limit);
	exit(1);
    }

    return (int) value;
}


static void handle_pipe(struct pipe *p)
{
    ssize_t ret;
    void *rdata;
    double bw;
    char info[256];

    if (p->rbytes == p->wbytes) {

	if (p->max_rate == -1)
	    ret = read_no_rate_limit(p);
	else
	    ret = read_rate_limit(p);

	/* Note, we will try to write all data received so far even if the
	   program has been aborted (SIGINT) */

	if (ret > 0) {
	    p->rbytes += ret;
	    p->readoffs = 0;

	} else if (ret == 0) {
	    /* Only pipe 0 can have regular files queued for it */
	    if (p->id != 0 || open_new_file(p) == 0) {
		terminated_pipes++;
		p->terminated = 1;

		/* Close output fd to cause EOF for the spawned process */
		close(p->out_fd);
		p->out_fd = -1;
	    }

	} else {
	    if (errno != EAGAIN && errno != EINTR) {
		perror("pmr: Read error");
		terminated_pipes = 2;
	    }
	}

    } else {

	rdata = &p->aligned_buf[p->readoffs];
	ret = write(p->out_fd, rdata, p->rbytes - p->wbytes - p->readoffs);

	if (ret > 0) {
	    p->wbytes += ret;
	    p->measurement_bytes += ret;
	    p->readoffs += ret;

	    if (use_md5)
		MD5Update(&p->md5ctx, (unsigned char *) p->aligned_buf, ret);

	} else if (ret == 0) {
	    fprintf(stderr, "pmr: interesting: write returned 0\n");
	    terminated_pipes = 2;
	    return;

	} else if (ret < 0) {
	    if (errno != EINTR && errno != EAGAIN) {
		perror("pmr: Write error");
		terminated_pipes = 2;
		return;
	    }
	}

	/* We only measure things for pipe 0 */
	if (p->id == 0 && p->valid_time &&
	    timetest(&bw, info, sizeof(info), p, 0)) {

	    char byte_info[256];
	    char unit[16];
	    double total;

	    size_transformation(&total, unit, p->wbytes);
	    sprintf(byte_info, "\ttotal: %.2f %s (%lld bytes)", total, unit,
		    p->wbytes);

	    if (estimatedbytes > 0)
		append_eta(byte_info, bw, p->wbytes);

	    /* A check for just being pedantic. info[] is always long enough */
	    if ((strlen(info) + strlen(byte_info) + 1) <= sizeof(info)) {
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

    /* get page size aligned buffer of size 'aligned_size' */
    p->real_buf = malloc(buffer_size + PAGE_SIZE);
    if (!p->real_buf) {
	fprintf(stderr, "pmr: not enough memory\n");
	exit(1);
    }
    p->aligned_buf = p->real_buf;
    p->aligned_buf += PAGE_SIZE - (((long) p->aligned_buf) & (PAGE_SIZE - 1));

    p->valid_time = 1;
    if (gettimeofday(&p->rate_time, 0)) {
	perror("pmr: gettimeofday failed");
	p->valid_time = 0;
    }

    p->measurement_time = p->rate_time;

    fcntl(in_fd, F_SETFL, O_NONBLOCK);

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

    fprintf(stderr, "pmr error: unknown unit: %s\n", endptr);
    exit(1);
    return 0.0;
}


static int open_new_file(struct pipe *p)
{
    char *fname;

    close(p->in_fd);

    if (n_files_read == n_files)
	return 0;

    fname = files[n_files_read++];

    p->in_fd = open(fname, O_RDONLY);
    if (p->in_fd < 0) {
	fprintf(stderr, "Error opening file %s: %s\n", fname, strerror(errno));
	exit(1);
    }

    return 1;
}


static void print_usage(const char *pname)
{
    fprintf(stderr, "pmr %s by Heikki Orsila <heikki.orsila@iki.fi>\n\nUsage:\n\n", VERSION);
    fprintf(stderr, " %s [-l Bps] [-s size] [-m] [-e com] [-t seconds] [-b size] [-r] [-v] FILE ..\n\n",
	    pname);
    fprintf(stderr, " -b size\tset input buffer size (default %d)\n", DEFAULT_BUFFER_SIZE);
    fprintf(stderr,
	    " -e com args\tRun command \"com\" with args, copy stdin of pmr to the stdin of\n"
	    "\t\tthe command, and copy stdout of the command to the stdout of\n"
	    "\t\tpmr. pmr acts as a measuring filter between the shell and\n"
	    "\t\tthe command.\n"
	    " -l Bps\t\tLimit throughput to 'Bps' bytes per second. It is also\n"
	    "\t\tpossible to use SI, IEC 60027 and bit units in the value.\n"
	    "\t\tSI units include kB, MB, ..., IEC units include KiB, MiB, ...\n"
	    "\t\tand bit units include kbit, Mbit, ...\n"
	    " -m / --md5\tCompute an md5 checksum of the stream (useful for verifying\n"
	    "\t\tdata integrity through TCP networks)\n"
	    " -r\t\tUse carriage return on output, no newline\n"
	    " -s size\tCalculate ETA given a size estimate. Giving regular files\n"
	    "\t\tas pmr parameters implies -s SUM, where SUM is the total\n"
	    "\t\tsize of those files.\n"
	    " -t secs\tUpdate interval in seconds\n"
	    " -v\t\tPrint version, about, contact and home page information\n");
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
    char *lineret;

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

	lineret = fgets(line, sizeof line, cfile);
	if (lineret == NULL)
	    break;
	if (line[0] == '#')
	    continue;		/* comment line */

	ret = skip_ws(line, 0);
	if (ret < 0)
	    continue;		/* empty line */

	word = &line[ret];

	/* Get second option from the line, if exists */
	opt = NULL;
	ret = skip_nws(line, ret);
	if (ret > 0) {
	    ret = skip_ws(line, ret);
	    if (ret > 0) {
		/* We have an option. zero-terminate it. */
		opt = &line[ret];
		ret = skip_nws(opt, 0);
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
	    fprintf(stderr, "Unknown configuration directive: %s\n", word);
	}
    }

    fclose(cfile);
}


static int read_no_rate_limit(struct pipe *p)
{
    return read(p->in_fd, p->aligned_buf, buffer_size);
}

static int read_rate_limit(struct pipe *p)
{
    int ret;
    int to_read, read_bytes;
    int t;
    struct timeval new_rate_time;

    if (p->max_rate == -1)
	return read_no_rate_limit(p);

    if (gettimeofday(&new_rate_time, 0)) {
	perror
	    ("pmr: gettimeofday failed. can not limit rate. going max speed.");
	p->max_rate = -1;
	return read_no_rate_limit(p);
    }

    if (p->rate_read_bytes > p->max_rate) {
	fprintf(stderr, "fatal error: rate_read_bytes > max_rate!\n");
	exit(1);
    }

    if (p->rate_read_bytes == p->max_rate) {
	t = 1000 * (new_rate_time.tv_sec - p->rate_time.tv_sec) +
	    ((int) new_rate_time.tv_usec) / 1000 -
	    ((int) p->rate_time.tv_usec) / 1000;
	if (t < 0) {
	    fprintf(stderr,
		    "pmr: chronoton particles detected. clock ran backwards. "
		    "k3wl!\n");
	    t = default_interval + 1;
	}
	if (t < 1000) {
	    usleep(1000000 - 1000 * t);
	    if (gettimeofday(&new_rate_time, 0)) {
		perror("pmr: gettimeofday failed. can not limit rate. going "
		       "max speed.");
		p->max_rate = -1;
		return read_no_rate_limit(p);
	    }
	}
	p->rate_time = new_rate_time;
	p->rate_read_bytes = 0;
    }

    to_read = p->max_rate - p->rate_read_bytes;
    if (to_read > buffer_size)
	to_read = buffer_size;

    read_bytes = 0;

    while (read_bytes < to_read) {

	ret = read(p->in_fd, p->aligned_buf + read_bytes, to_read - read_bytes);

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

    p->rate_read_bytes += read_bytes;

    return read_bytes;
}


static inline void set_fd(int *maxfd, int fd, fd_set *set)
{
    if (*maxfd < fd)
	*maxfd = fd;

    FD_SET(fd, set);
}


static void size_transformation(double *size, char *unit, double srcsize)
{
    int order;

    if (srcsize < 0) {
	fprintf(stderr, "pmr bug: negative size in size_transformation()\n");
	exit(1);
    }

    for (order = 0; order < NUNITS; order++) {
	if (srcsize < 1024.0)
	    break;
	srcsize /= 1024.0;
    }

    if (order == NUNITS) {
	fprintf(stderr, "pmr warning: too high a number for size_transformation()r\n");
	order = NUNITS - 1;
    }

    *size = srcsize;
    strcpy(unit, iec_units[order]);
}


/* skip whitespace characters. return -1 if end of line reached */
static int skip_ws(const char *line, int ind)
{
    while (isspace(line[ind]))
	ind++;

    if (line[ind] == 0)
	ind = -1;

    return ind;
}


/* skip non-whitespace characters. return -1 if end of line reached */
static int skip_nws(const char *line, int ind)
{
    while (line[ind] != 0 && isspace(line[ind]) == 0)
	ind++;

    if (line[ind] == 0)
	ind = -1;

    return ind;
}


void spawn_process(const char **args)
{
    int toprocess[2];
    int fromprocess[2];

    if (pipe(toprocess) || pipe(fromprocess)) {
	perror("pmr: can not pipe() for -e");
	exit(1);
    }

    if (fork() == 0) {

	close(toprocess[1]);
	close(fromprocess[0]);

	if (dup2(toprocess[0], 0) < 0 || dup2(fromprocess[1], 1) < 0) {
	    perror("Can not dup for -e");
	    exit(1);
	}

	execv(args[0], (char * const *) args);

	fprintf(stderr, "pmr: can not execute %s (%s)\n",
		args[0], strerror(errno));
	
	abort();
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

    if (gettimeofday(&nt, 0)) {
	/* time failed. bandwidth = NaN. return false. */
	return 0;
    }

    t = 1000 * (nt.tv_sec - ot->tv_sec) + ((int) nt.tv_usec) / 1000 - ((int) ot->tv_usec) / 1000;

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
	    snprintf(s, maxlen, "bandwidth: %.2f %s/s", canonical_bw, id);
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

	/* Writes length amount of spaces and then a carriage return (\r) */
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


int main(int argc, char **argv)
{
    struct sigaction act = {
	.sa_handler = ctrlc_handler
    };

    int valid_time;
    struct timeval vot;

    int i, ret;
    int no_options = 0;
    double presize = 0.0;
    int exec_mode = 0;

    read_config();

    for (i = 1; i < argc;) {

	if (no_options || argv[i][0] != '-') {

	    char *fname = argv[i];
	    struct stat st;

	    if (stat(fname, &st)) {
		fprintf(stderr, "%s can not be read: %s\n", fname,
			strerror(errno));
		exit(1);
	    }

	    if (!S_ISREG(st.st_mode)) {
		fprintf(stderr, "One of the files is not a regular file -> "
			"no ETA estimation\n");
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
	    if ((i + 1) < argc) {
		buffer_size = atoi(argv[i + 1]);
	    } else {
		fprintf(stderr, "expecting a value for -b\n");
		exit(1);
	    }
	    i += 2;
	    continue;
	}

	if (!strcmp(argv[i], "-e") || !strcmp(argv[i], "--exec")) {
	    if ((i + 1) >= argc) {
		fprintf(stderr, "Too few arguments for -e\n");
		exit(1);
	    }
	    exec_mode = 1;
	    break;
	}

	if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
	    print_usage(argv[0]);
	    exit(0);
	}

	if (!strcmp(argv[i], "-l")) {
	    if ((i + 1) < argc) {
		default_max_rate = get_bw_limit(argv[i + 1]);
	    } else {
		fprintf(stderr,
			"expecting a value for bandwidth limit (bytes per second)\n");
		exit(1);
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
		fprintf(stderr,
			"Not enough args. Missing size estimate.\n");
		exit(1);
	    }

	    presize = inverse_size_transformation(argv[i + 1]);

	    if (presize < 0.0) {
		fprintf(stderr, "Braindamaged size estimate: %f.\n",
			presize);
		presize = 0.0;
	    }

	    i += 2;
	    continue;
	}

	if (!strcmp(argv[i], "-t")) {
	    if ((i + 1) < argc) {
		default_interval = 1000 * atoi(argv[i + 1]);
	    } else {
		fprintf(stderr, "expecting a value for -t\n");
		exit(1);
	    }
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

	fprintf(stderr, "unknown args: %s\n", argv[i]);
	exit(1);
    }

    initialize_pipe(0, 0, 1);

    if (exec_mode) {
	/* Pipe 1 is initialized here */
	spawn_process((const char **) &argv[i + 1]);
    }

    /* Record creation time of pipe 0 to determine total run-time of pmr */
    valid_time = pipes[0].valid_time;
    vot = pipes[0].measurement_time;

    sigaction(SIGPIPE, &act, 0);
    sigaction(SIGINT, &act, 0);

    /* For ETA calculation when size was given with -s. This overrides
       byte size computed for regular file queue. */
    if (presize > 0.0)
	estimatedbytes = presize;

    /* Open the first file in the regular file queue */
    if (n_files > 0)
	open_new_file(&pipes[0]);

    while (terminated_pipes < n_pipes) {
	fd_set rfds, wfds;
	int maxfd;

	/* Optimise the single pipe case */
	if (n_pipes == 1) {
	    handle_pipe(&pipes[0]);
	    continue;
	}

	/* We have 2 pipes */

	FD_ZERO(&rfds);
	FD_ZERO(&wfds);

	maxfd = 0;

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

	if (ret < 0 && errno != EINTR) {
	    perror("pmr: select() error");
	    exit(1);
	}

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

	if (program_interrupted) {
	    fprintf(stderr, "Program interrupted%s\n",
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

	if (use_md5 && program_interrupted == 0) {
	    MD5Final(md5, &pipes[0].md5ctx);
	    fprintf(stderr, "md5sum: %.2x%.2x%.2x%.2x%.2x%.2x%.2x%.2x%.2x%.2x%.2x%.2x%.2x%.2x%.2x%.2x\n",
		    md5[0], md5[1], md5[2], md5[3], md5[4], md5[5], md5[6],
		    md5[7], md5[8], md5[9], md5[10], md5[11], md5[12],
		    md5[13], md5[14], md5[15]);
	}
    } while (0);

    close(1);

    return program_interrupted ? EXIT_FAILURE : EXIT_SUCCESS;
}
