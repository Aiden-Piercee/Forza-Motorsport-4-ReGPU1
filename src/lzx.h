#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

// The Xbox LZX stream (XMemCompress) that FM4's zips store as method 21: chunks of a
// big-endian compressed size (0xFF, then the uncompressed and compressed sizes, for a
// chunk that is not 32 KB), each one LZX frame. False if the stream doesn't decode to
// exactly `size` bytes.
bool LzxDecompress(const uint8_t* in, size_t inSize, size_t size, std::vector<uint8_t>& out);
