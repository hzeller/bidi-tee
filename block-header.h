#ifndef BLOCK_HEADER_H
#define BLOCK_HEADER_H

#include <cstdint>

struct BlockHeader {
  int64_t timestamp_ns;
  uint64_t channel : 4;
  uint64_t channel_closed : 1;
  uint64_t reserved : 43;
  uint64_t block_size : 16;
};
static_assert(sizeof(BlockHeader) == 16, "Block header unexpected size");

#endif // BLOCK_HEADER_H
