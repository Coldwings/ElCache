#include "elcache/protocol.hpp"

// Most protocol implementation is in codec.cpp
// This file contains additional protocol utilities

namespace elcache::protocol {

// Protocol version compatibility check
bool is_compatible_version(uint16_t remote_version) {
    // For now, only exact match
    return remote_version == VERSION;
}

}  // namespace elcache::protocol
