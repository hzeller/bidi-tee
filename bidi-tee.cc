#include <fcntl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>

static int usage(const char *progname) {
  fprintf(stderr, "Usage: %s <output> -- "
          "</path/to/program> <program-options...>\n", progname);
  return 2;
}

// Things to wrap communication around, so that we have a colored
// output.
static constexpr char kParentChildPrefix[] = "\033[1;31m";
static constexpr char kChildParentPrefixStdout[] = "\033[7;34m";
static constexpr char kChildParentPrefixStderr[] = "\033[0;34m";
static constexpr char kSuffix[] = "\033[0m";
static constexpr char kEOFMarker[] = "<<EOF>>";

static constexpr int kReadSide = 0;
static constexpr int kWriteSide = 1;

static void reliable_write(int fd, char *buffer, size_t size) {
  while (size) {
    int w = write(fd, buffer, size);
    if (w < 0) return;  // Uhm.
    size -= w;
    buffer += w;
  }
}

static bool CopyFromTo(char *buf, size_t buf_size,
                       int from, int to,
                       iovec *coloring, int tee_fd) {
  int r = read(from, buf, buf_size);
  reliable_write(to, buf, r);
  if (r <= 0) {
    strcpy(buf, kEOFMarker);
    r = strlen(buf);
  }
  coloring[1].iov_len = r;
  writev(tee_fd, coloring, 3);

  return r > 0;
}

int main(int argc, char *argv[]) {
  if (argc < 3) {
    return usage(argv[0]);
  }

  const char *out_filename = argv[1];

  if (strcmp(argv[2], "--") != 0) {
    // Right now, we don't actually have options yet, but to be ready
    // for options, let's enforce -- already now.
    fprintf(stderr, "Expected -- before name of program to start\n");
    return 1;
  }

  const int start_of_program = 3;

  // Pipes in two directions
  int parent_to_child_stdin[2];
  int child_to_parent_stdout[2];
  int child_to_parent_stderr[2];

  if (pipe(parent_to_child_stdin) < 0 ||
      pipe(child_to_parent_stdout) < 0 ||
      pipe(child_to_parent_stderr) < 0) {
    perror("Couldn't open pipes\n");
    return 1;
  }

  const int pid = fork();
  if (pid < 0) {
    perror("fork");
    return 1;
  }

  if (pid == 0) {
    // Child.
    close(parent_to_child_stdin[kWriteSide]);
    close(child_to_parent_stdout[kReadSide]);
    close(child_to_parent_stderr[kReadSide]);

    dup2(parent_to_child_stdin[kReadSide], STDIN_FILENO);
    dup2(child_to_parent_stdout[kWriteSide], STDOUT_FILENO);
    dup2(child_to_parent_stderr[kWriteSide], STDERR_FILENO);

    execv(argv[start_of_program], argv + start_of_program);

    // Uh, still here ? exec failed...
    fprintf(stderr, "Failed to execute %s: %s\n",
            argv[start_of_program], strerror(errno));
    close(parent_to_child_stdin[kReadSide]);
    close(child_to_parent_stdout[kWriteSide]);
    close(child_to_parent_stderr[kWriteSide]);

    return 1;
  }

  // Parent
  close(parent_to_child_stdin[kReadSide]);
  close(child_to_parent_stdout[kWriteSide]);
  close(child_to_parent_stderr[kWriteSide]);

  const int outfd = open(out_filename, O_WRONLY|O_CREAT|O_TRUNC, 0644);
  if (outfd < 0) {
    perror("Couldn't open output file");
    return 1;
  }

  char copy_buf[1024];

  // Wrapping the coloring additions in iovec, so that we can
  // writev() them in one go.
  iovec parent_child_coloring_stdin[3] = {
    { (void*)kParentChildPrefix, strlen(kParentChildPrefix) },
    { copy_buf, 0},  // buffer already known, len to be filled later
    { (void*)kSuffix, strlen(kSuffix) },
  };
  iovec child_parent_coloring_stdout[3] = {
    { (void*)kChildParentPrefixStdout, strlen(kChildParentPrefixStdout) },
    { copy_buf, 0},  // buffer already known, len to be filled later
    { (void*)kSuffix, strlen(kSuffix) },
  };
  iovec child_parent_coloring_stderr[3] = {
    { (void*)kChildParentPrefixStderr, strlen(kChildParentPrefixStderr) },
    { copy_buf, 0},  // buffer already known, len to be filled later
    { (void*)kSuffix, strlen(kSuffix) },
  };

  fd_set rd_fds;
  FD_ZERO(&rd_fds);

  const int max_fd = std::max(child_to_parent_stdout[kReadSide],
                              child_to_parent_stderr[kReadSide]);
  bool parent_input_open = true;
  bool child_output_stdout_open = true;
  bool child_output_stderr_open = true;
  while (parent_input_open ||
         child_output_stdout_open || child_output_stderr_open) {
    if (parent_input_open)
      FD_SET(STDIN_FILENO, &rd_fds);
    if (child_output_stdout_open)
      FD_SET(child_to_parent_stdout[kReadSide], &rd_fds);
    if (child_output_stderr_open)
      FD_SET(child_to_parent_stderr[kReadSide], &rd_fds);

    int sret = select(max_fd+1, &rd_fds, NULL, NULL, NULL);
    if (sret < 0) return 0;

    if (FD_ISSET(STDIN_FILENO, &rd_fds)) {
      if (!CopyFromTo(copy_buf, sizeof(copy_buf),
                      STDIN_FILENO, parent_to_child_stdin[kWriteSide],
                      parent_child_coloring_stdin, outfd)) {
        parent_input_open = false;
        FD_CLR(STDIN_FILENO, &rd_fds);
      }
    }

    if (FD_ISSET(child_to_parent_stdout[kReadSide], &rd_fds)) {
      if (!CopyFromTo(copy_buf, sizeof(copy_buf),
                      child_to_parent_stdout[kReadSide], STDOUT_FILENO,
                      child_parent_coloring_stdout, outfd)) {
        child_output_stdout_open = false;
        FD_CLR(child_to_parent_stdout[kReadSide], &rd_fds);
      }
    }

    if (FD_ISSET(child_to_parent_stderr[kReadSide], &rd_fds)) {
      if (!CopyFromTo(copy_buf, sizeof(copy_buf),
                      child_to_parent_stderr[kReadSide], STDERR_FILENO,
                      child_parent_coloring_stderr, outfd)) {
        child_output_stderr_open = false;
        FD_CLR(child_to_parent_stderr[kReadSide], &rd_fds);
      }
    }
  }
}
