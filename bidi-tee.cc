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
#include <cstdint>
#include <time.h>
#include <cstring>

#include "block-header.h"

static int usage(const char *progname) {
  fprintf(stderr, "Usage: %s <output-logfile> -- "
          "</path/to/program> <program-options...>\n", progname);
  return 2;
}

using timestamp_t = int64_t;

static timestamp_t GetTimeNanoseconds() {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  // since CLOCK_MONOTONIC is not based on start of epoch (but typically
  // since machine was booted), let's rebase that. First time we're called,
  // we determine the time offset.
  static timestamp_t global_second_offset = time(nullptr) - t.tv_sec;
  return (global_second_offset + t.tv_sec) * 1000000000 + t.tv_nsec;
}

static void reliable_write(int fd, char *buffer, ssize_t size) {
  while (size > 0) {
    int w = write(fd, buffer, size);
    if (w < 0) return;  // Uhm.
    size -= w;
    buffer += w;
  }
}

class ChannelCopier {
public:
  ChannelCopier(int channel, int read_fd, int write_fd)
    : read_fd_(read_fd), write_fd_(write_fd) {
    memset(&header_, 0x00, sizeof(header_));
    header_.channel = channel;
    block_[0].iov_base = &header_;
    block_[0].iov_len = sizeof(header_);
  }

  int readfd() const { return read_fd_; }
  bool valid() const { return !header_.channel_closed; }
  void AddToFdset(fd_set *s) const {
    if (valid()) {
      FD_SET(read_fd_, s);
    } else {
      FD_CLR(read_fd_, s);
    }
  }

  void CopyUsingBuffer(timestamp_t timestamp, int tee_fd,
                       char *buf, size_t size) {
    int r = read(read_fd_, buf, size);
    block_[1].iov_base = buf;
    block_[1].iov_len = r > 0 ? r : 0;
    reliable_write(write_fd_, buf, r);
    header_.channel_closed = (r <= 0);
    header_.timestamp_ns = timestamp;
    header_.block_size = block_[1].iov_len;
    writev(tee_fd, block_, 2);
  }

private:
  const int read_fd_;
  const int write_fd_;
  BlockHeader header_;
  iovec block_[2];
};

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

  static constexpr int kReadSide = 0;
  static constexpr int kWriteSide = 1;

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

  char copy_buf[65535];

  ChannelCopier stdin_cp(0, STDIN_FILENO, parent_to_child_stdin[kWriteSide]);
  ChannelCopier stdout_cp(1, child_to_parent_stdout[kReadSide], STDOUT_FILENO);
  ChannelCopier stderr_cp(2, child_to_parent_stderr[kReadSide], STDERR_FILENO);

  fd_set rd_fds;
  FD_ZERO(&rd_fds);

  const int max_fd = std::max(stdout_cp.readfd(), stderr_cp.readfd());

  while (stdin_cp.valid() || stdout_cp.valid() || stderr_cp.valid()) {
    for (const ChannelCopier &channel : { stdin_cp, stdout_cp, stderr_cp }) {
      channel.AddToFdset(&rd_fds);
    }

    int sret = select(max_fd+1, &rd_fds, NULL, NULL, NULL);
    if (sret < 0) return 0;

    const timestamp_t timestamp = GetTimeNanoseconds();
    for (ChannelCopier *channel : { &stdin_cp, &stdout_cp, &stderr_cp }) {
      if (FD_ISSET(channel->readfd(), &rd_fds)) {
        channel->CopyUsingBuffer(timestamp, outfd, copy_buf, sizeof(copy_buf));
      }
    }
  }
}
