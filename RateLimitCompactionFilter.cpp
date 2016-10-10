#include "ratelimit/RateLimitCompactionFilter.h"

#include <algorithm>
#include <chrono>
#include <string>

#include "glog/logging.h"
#include "pipeline/RedisHandler.h"

#include "ratelimit/RateLimitHandler.h"

namespace ratelimit {

bool RateLimitCompactionFilter::Filter(int level, const rocksdb::Slice& key, const rocksdb::Slice& existingValue,
                                       std::string* newValue, bool* valueChanged) const {
  *valueChanged = false;

  RateLimitHandler::KeyParams keyParams;
  CHECK(RateLimitHandler::decodeRateLimitKey(key, &keyParams)) << "RateLimit key in RocksDB is corrupted";
  RateLimitHandler::ValueParams valueParams;
  CHECK(RateLimitHandler::decodeRateLimitValue(existingValue, &valueParams))
      << "RateLimit value in RocksDB is corrupted";

  RedisIntType nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  RedisIntType idleTimeMs = nowMs - valueParams.lastReducedAtMs;

  if (idleTimeMs / keyParams.refillTimeMs * keyParams.refillAmount >= keyParams.maxAmount) {
    // we would have a full bucket anyway, so no longer need the key
    return true;
  }

  return false;
}

}  // namespace ratelimit
