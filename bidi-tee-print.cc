#include <cstdlib>
#include <unistd.h>  // NOLINT for getopt

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>

#include <algorithm>
#include <set>

#include "block-header.h"

struct PrintOptions {
  bool colored = true;
  bool ascii_escape = false;
  bool ascii_escape_break_after_newline = true;
};

static int usage(const char *progname, int retval) {
  fprintf(stderr, "Usage: %s [<options>] <bidi-tee-logfile>\n", progname);
  fprintf(stderr,
          "-h            : this help\n"
          "-c            : toggle print in color (default: on)\n"
          "-e            : toggle c-escape output (default: off)\n"
          "-n            : if -e: do start new line after '\\n' (default: on)\n"
          "-ts           : Print timestamp since start of recording.\n"
          "-ta           : Print timestamps as absolute timestamps.\n"
          "-td           : Print delta timestamps relative to last print\n"
          "-s <select-channel> : comma-separated list of channels to print, e.g. 0,2 prints stdin and stderr\n"
          "-o <filename> : Output to filename\n");
  return retval;
}

// Things to wrap communication around, so that we have a colored
// output.
static constexpr const char *kColors[3] = {
    "\033[1;31m", // bold red
    "\033[1;34m", // bold blue
    "",           // regular
};

static constexpr char kResetColor[] = "\033[0m";

static void PrintCEscaped(FILE *out, const PrintOptions &opts,
                          const char *content, size_t size) {
  const char *const end = content + size;
  for (/**/; content < end; ++content) {
    const char c = *content;
    switch (c) {
    case '\n':
      if (opts.ascii_escape_break_after_newline) {
        fwrite("\\n\n", 3, 1, out);
      } else {
        fwrite("\\n", 2, 1, out);
      }
      break;
    case '\r': fwrite("\\r", 2, 1, out); break;
    case '\t': fwrite("\\t", 2, 1, out); break;
    default:
      if (c < ' ') {
        fprintf(out, "\\x%02x", c);
      } else {
        fwrite(&c, 1, 1, out);  // todo: collect in bulk if no escapes.
      }
    }
  }
}

static void PrintContent(FILE *out, const PrintOptions &opts, int channel,
                         const char *content, size_t size) {
  if (opts.colored && channel < 3) {
    fwrite(kColors[channel], 1, strlen(kColors[channel]), out);
  }
  // TODO: apply color line by line as less is not be able to do multiline
  if (opts.ascii_escape) {
    PrintCEscaped(out, opts, content, size);
  } else {
    fwrite(content, size, 1, out);
  }
  if (opts.colored) fwrite(kResetColor, strlen(kResetColor), 1, out);
}

static const char *PrintChannel(int channel) {
  switch (channel) {
  case 0: return "->";  // stdin
  case 1: return "<-";  // stdout
  case 2: return "<=";  // stderr
  case 0x0f: return "EXIT";
  default: return "??";
  }
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    return usage(argv[0], 2);
  }

  FILE *out = stdout;
  std::set<int> selected_channels;
  enum class TSPrint { kNone, kStartFile, kDelta, kAbsolute };
  TSPrint print_timestamp = TSPrint::kNone;
  PrintOptions print_opts;

  int opt;
  while ((opt = getopt(argc, argv, "ht:ceno:s:")) != -1) {
    switch (opt) {
    case 'h':
      return usage(argv[0], 0);
    case 't':
      switch (optarg[0]) {
      case 's':
        print_timestamp = TSPrint::kStartFile;
        break;
      case 'a':
        print_timestamp = TSPrint::kAbsolute;
        break;
      case 'd':
        print_timestamp = TSPrint::kDelta;
        break;
      default:
        fprintf(stderr, "-t requires a letter to qualify timestamp printing\n");
        return usage(argv[0], 2);
      }
      break;
    case 'c':
      print_opts.colored = !print_opts.colored;
      break;
    case 'e':
      print_opts.ascii_escape = !print_opts.ascii_escape;
      break;
    case 'n':
      print_opts.ascii_escape_break_after_newline =
          !print_opts.ascii_escape_break_after_newline;
      break;
    case 'o':
      out = fopen(optarg, "wb");
      break;
    case 's': {
      int s[4]; // In case there will be an extra channel.
      const int num = sscanf(optarg, "%d,%d,%d,%d", &s[0], &s[1], &s[2], &s[3]);
      for (int i = 0; i < num; ++i) {
        selected_channels.insert(s[i]);
    }
    } break;

      // ΤODO: case 'f' for 'follow'

    default:
      return usage(argv[0], 2);
    }
  }

  if (!out) {
    perror("Couldn't open output file");
    return 1;
  }

  if (selected_channels.empty()) {
    selected_channels = {0, 1, 2, 15};
  }

  const char *in_filename = argv[optind];
  FILE *instream = fopen(in_filename, "r");
  if (!instream) {
    perror("Couldn't open input");
    return 1;
  }

  char copy_buf[65535];
  BlockHeader header;
  int64_t start_timestamp = -1;
  bool last_was_newline = true;
  char delta_timestamp_prefix = ' ';

  // Printing messages such as channel closed or exit status.
  // Currently essentially when we have timestamp printing on.
  const bool print_out_of_band = print_timestamp != TSPrint::kNone;

  int exit_code = EXIT_SUCCESS;
  while (fread(&header, sizeof(header), 1, instream)) {
    if (start_timestamp < 0) start_timestamp = header.timestamp_ns;

    // Don't attempt to read zero bytes when channel is closed.
    if (header.block_size > 0 &&
        fread(copy_buf, header.block_size, 1, instream) != 1) {
      fprintf(stderr, "Unexpected end of file reading %d bytes\n",
              header.block_size);
      exit_code = EXIT_FAILURE;
      break;
    }

    if (selected_channels.count(header.channel) == 0) {
      continue;  // Not interested in printing this channel. Skip.
    }

    if (print_timestamp != TSPrint::kNone && !last_was_newline) {
      fprintf(out, "\n");  // make sure timestamps start in a new line.
    }

    const int64_t since_start = header.timestamp_ns - start_timestamp;
    const char *channel_text = PrintChannel(header.channel);
    switch (print_timestamp) {
    case TSPrint::kNone: break;
    case TSPrint::kStartFile:
      fprintf(out, "%6ld.%06ldms %s: ",
              since_start / 1000000, since_start % 1000000, channel_text);
      break;
    case TSPrint::kDelta:
      fprintf(out, "%c%5ld.%06ldms %s: ", delta_timestamp_prefix,
              since_start / 1000000, since_start % 1000000, channel_text);
      delta_timestamp_prefix = '+';
      start_timestamp = header.timestamp_ns;
      break;
    case TSPrint::kAbsolute: {
      const time_t seconds = header.timestamp_ns / 1000000000;
      struct tm prdate;
      localtime_r(&seconds, &prdate);
      char buffer[32];
      strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &prdate);
      fprintf(out, "[%s.%09ld] %s: ",
              buffer, header.timestamp_ns % 1000000000, channel_text);
      break;
    }
    }

    if (header.channel_closed) {
      if (print_out_of_band) {
        fprintf(out, "<channel %u closed>\n", header.channel);
        last_was_newline = true;
      }
      continue;
    }

    if (header.channel == 15) {
      if (print_out_of_band) {
        fprintf(out, "Exit code %d\n", header.exit_code);
        last_was_newline = true;
      }
      continue;
    }

    PrintContent(out, print_opts, header.channel, copy_buf, header.block_size);

    last_was_newline =
      (copy_buf[header.block_size - 1] == '\n') &&
      (!print_opts.ascii_escape || print_opts.ascii_escape_break_after_newline);
  }
  fclose(instream);
  fclose(out);
  return exit_code;
}
