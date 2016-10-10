#ifndef RATELIMIT_RATELIMITCOMPACTIONFILTER_H_
#define RATELIMIT_RATELIMITCOMPACTIONFILTER_H_

#include <string>

#include "codec/RedisValue.h"
#include "rocksdb/compaction_filter.h"
#include "rocksdb/slice.h"

namespace ratelimit {

class RateLimitCompactionFilter : public rocksdb::CompactionFilter {
 public:
  bool Filter(int level, const rocksdb::Slice& key, const rocksdb::Slice& existingValue, std::string* newValue,
              bool* valueChanged) const override;
  const char* Name() const override { return "RateLimitCompactionFilter"; }

 private:
  using RedisIntType = codec::RedisValue::IntType;
};

}  // namespace ratelimit

#endif  // RATELIMIT_RATELIMITCOMPACTIONFILTER_H_
