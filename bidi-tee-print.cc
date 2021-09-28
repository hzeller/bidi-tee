#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

#include <algorithm>
#include <set>

#include "block-header.h"

static int usage(const char *progname, int retval) {
  fprintf(stderr, "Usage: %s [<options>] <bidi-tee-logfile>\n", progname);
  fprintf(stderr,
          "-h            : this help\n"
          "-c            : toggle print in color\n"
          "-t            : Print timestamp\n"
          "-r            : Print timestamp relative to last print\n"
          "-s <select-channel> : comma-separated list of channels to print, e.g. 0,2 prints stdin and stderr\n"
          "-o <filename> : Output to filename\n");
  return retval;
}

static bool reliable_read(int fd, void *buf, ssize_t desired_size) {
  char *read_buf = (char*)buf;
  while (desired_size > 0) {
    int r = read(fd, read_buf, desired_size);
    if (r <= 0) return false;
    desired_size -= r;
    read_buf += r;
  }
  return true;
}

// Things to wrap communication around, so that we have a colored
// output.
static constexpr const char *kColors[3] = {
    "\033[1;31m", // bold red
    "\033[1;34m", // bold blue
    "",           // regular
};

static constexpr char kSuffix[] = "\033[0m";

void write_colored(FILE *out, int channel, const char *buf, size_t size) {
  if (channel < 3) {
    fwrite(kColors[channel], 1, strlen(kColors[channel]), out);
  }
  // TODO: apply color line by line as less is not be able to do multiline
  fwrite(buf, 1, size, out);
  fwrite(kSuffix, 1, strlen(kSuffix), out);
}


int main(int argc, char *argv[]) {
  if (argc < 2) {
    return usage(argv[0], 2);
  }

  FILE *out = stdout;
  std::set<int> selected_channels;
  bool print_timestamp = false;
  bool print_relative_time = false;
  bool print_colored = true;

  int opt;
  while ((opt = getopt(argc, argv, "htrco:s:")) != -1) {
    switch (opt) {
    case 'h': return usage(argv[0], 0);
    case 't': print_timestamp = !print_timestamp; break;
    case 'r': print_relative_time = !print_relative_time; break;
    case 'c': print_colored = !print_colored; break;
    case 'o': out = fopen(optarg, "wb"); break;
    case 's': {
      int s[4];   // In case there will be an extra channel.
      int count = sscanf(optarg, "%d,%d,%d,%d", &s[0], &s[1], &s[2], &s[3]);
      for (int i = 0; i < count; ++i) selected_channels.insert(s[i]);
    }
    }
  }

  if (!out) {
    perror("Couldn't open output file");
    return 1;
  }

  if (selected_channels.empty()) {
    selected_channels = {0, 1, 2};
  }

  const char *in_filename = argv[optind];

  const int in_fd = open(in_filename, O_RDONLY);
  if (in_fd < 0) {
    perror("Couldn't open input");
    return 1;
  }

  char copy_buf[65535];
  BlockHeader header;
  int64_t start_timestamp = -1;
  bool last_was_newline = true;

  while (reliable_read(in_fd, &header, sizeof(header))) {
    if (!reliable_read(in_fd, copy_buf, header.block_size)) {
      fprintf(stderr, "Unexpected end of file reading %d bytes\n",
              header.block_size);
      return 1;
    }
    if (selected_channels.count(header.channel) == 0)
      continue;  // Not interested in printing this channel. Skip.

    if (print_timestamp) {
      // To make sure that timestamp starts at the beginning of the line
      // add a newline unless it already was naturally in the last buffer.
      if (!last_was_newline) fprintf(out, "\n");
      const bool print_plus = print_relative_time && start_timestamp > 0;
      if (start_timestamp < 0) start_timestamp = header.timestamp_ns;
      const int64_t since_start = header.timestamp_ns - start_timestamp;
      fprintf(out, "%s%5ld.%06ldms: ", print_plus ? "+" : " ",
              since_start / 1000000, since_start % 1000000);
      if (print_relative_time) start_timestamp = header.timestamp_ns;
    }

    if (print_colored) {
      write_colored(out, header.channel, copy_buf, header.block_size);
    } else {
      fwrite(copy_buf, 1, header.block_size, out);
    }
    last_was_newline = copy_buf[header.block_size - 1] == '\n';
  }
  return 0;
}
