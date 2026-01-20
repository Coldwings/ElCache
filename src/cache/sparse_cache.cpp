#include "elcache/sparse_cache.hpp"

namespace elcache {

// Global sparse cache instance - thread-safe singleton
SparseCache& global_sparse_cache() {
    static SparseCache instance;
    return instance;
}

}  // namespace elcache
