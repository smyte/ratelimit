#ifndef RATELIMIT_RATELIMITHANDLER_H_
#define RATELIMIT_RATELIMITHANDLER_H_

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "codec/RedisValue.h"
#include "pipeline/RedisHandler.h"
#include "ratelimit/RateLimitCompactionFilter.h"
#include "rocksdb/db.h"
#include "rocksdb/slice.h"

namespace ratelimit {

class RateLimitHandler : public pipeline::RedisHandler {
 public:
  using RedisIntType = codec::RedisValue::IntType;
  // Parameters encoded along with string key name as RocksDB keys
  struct KeyParams {
    RedisIntType maxAmount;
    RedisIntType refillAmount;
    RedisIntType refillTimeMs;
  };
  static_assert(sizeof(KeyParams) == sizeof(RedisIntType) * 3, "Entries in `KeyParams` are not aligned");
  // Parameters encoded as RocksDB values
  struct ValueParams {
    RedisIntType amount;
    RedisIntType lastRefilledAtMs;
    RedisIntType lastReducedAtMs;
  };
  static_assert(sizeof(ValueParams) == sizeof(RedisIntType) * 3, "Entries in `ValueParams` are not aligned");
  // Arguments for Redis commands
  struct RateLimitArgs {
    RedisIntType maxAmount;
    RedisIntType refillTimeMs;
    RedisIntType refillAmount;
    RedisIntType tokenAmount;
    RedisIntType clientTimeMs;
  };
  static_assert(sizeof(RateLimitArgs) == sizeof(RedisIntType) * 5, "Entries in `RateLimitArgs` are not aligned");

  static rocksdb::Slice encodeRateLimitKey(const std::string& keyName, const KeyParams& params, std::string* keyBuf);
  static rocksdb::Slice encodeRateLimitValue(const ValueParams& params, std::string* valueBuf);

  // Allow others to decode key/value stored in rocksdb
  static bool decodeRateLimitKey(const rocksdb::Slice& encodedKey, KeyParams* params);
  static bool decodeRateLimitValue(const rocksdb::Slice& encodedValue, ValueParams* params);

  // Parse input arguments with default values for optional arguments
  static codec::RedisValue parseRateLimitArgs(const std::vector<std::string>& cmd, bool useMs, bool isReduce,
                                              RateLimitArgs* args, bool* strict);
  static bool getRateLimitArgsDeprecated(const std::vector<std::string>& cmd, RateLimitArgs* args);

  // Lazily adjust the current token bucket amount based on the given configuration and timestamps
  static RedisIntType adjustAmount(RedisIntType currAmount, RedisIntType lastRefilledAtMs, const RateLimitArgs& args,
                                   RedisIntType* newRefilledAtMs);

  static void optimizeColumnFamily(int defaultBlockCacheSizeMb, rocksdb::ColumnFamilyOptions* options) {
    options->OptimizeForPointLookup(defaultBlockCacheSizeMb);
    options->compaction_filter = new RateLimitCompactionFilter();
  }

  explicit RateLimitHandler(std::shared_ptr<pipeline::DatabaseManager> databaseManager)
      : pipeline::RedisHandler(databaseManager), mutexes_(new std::mutex[kMaxConcurrentWriters]) {}

  const CommandHandlerTable& getCommandHandlerTable() const override {
    static const CommandHandlerTable commandHandlerTable(mergeWithDefaultCommandHandlerTable({
      {"rl.get", {static_cast<CommandHandlerFunc>(&RateLimitHandler::rlGetCommand), 3, 8}},
      {"rl.reduce", {static_cast<CommandHandlerFunc>(&RateLimitHandler::rlReduceCommand), 3, 10}},
      {"rl.pget", {static_cast<CommandHandlerFunc>(&RateLimitHandler::rlPgetCommand), 3, 8}},
      {"rl.preduce", {static_cast<CommandHandlerFunc>(&RateLimitHandler::rlPreduceCommand), 3, 10}},
    }));
    return commandHandlerTable;
  }

  RedisIntType getAdjustedAmountFromDb(const std::string& keyName, const RateLimitArgs& args, std::string* keyBuf,
                                       RedisIntType* newRefilledAtMs);

 private:
  static constexpr int kMaxConcurrentWriters = 1024;

  // Commands that supports second precision
  codec::RedisValue rlGetCommand(const std::vector<std::string>& cmd, Context* ctx) {
    RateLimitArgs args = {};
    bool strict = false;
    codec::RedisValue parseStatus = parseRateLimitArgs(cmd, false, false, &args, &strict);
    if (parseStatus != simpleStringOk()) return parseStatus;
    return getAndReduceTokens(cmd[1], args, strict, ctx);
  }
  codec::RedisValue rlReduceCommand(const std::vector<std::string>& cmd, Context* ctx) {
    RateLimitArgs args = {};
    bool strict = false;
    codec::RedisValue parseStatus = parseRateLimitArgs(cmd, false, true, &args, &strict);
    if (parseStatus != simpleStringOk()) return parseStatus;
    return getAndReduceTokens(cmd[1], args, strict, ctx);
  }

  // Commands that supports millisecond precision
  codec::RedisValue rlPgetCommand(const std::vector<std::string>& cmd, Context* ctx) {
    RateLimitArgs args = {};
    bool strict = false;
    codec::RedisValue parseStatus = parseRateLimitArgs(cmd, true, false, &args, &strict);
    if (parseStatus != simpleStringOk()) return parseStatus;
    return getAndReduceTokens(cmd[1], args, strict, ctx);
  }
  codec::RedisValue rlPreduceCommand(const std::vector<std::string>& cmd, Context* ctx) {
    RateLimitArgs args = {};
    bool strict = false;
    codec::RedisValue parseStatus = parseRateLimitArgs(cmd, true, true, &args, &strict);
    if (parseStatus != simpleStringOk()) return parseStatus;
    return getAndReduceTokens(cmd[1], args, strict, ctx);
  }

  // Get current tokens remaining in the bucket and optionally take the specified amount
  // Note that the returned value is the remaining tokens before taking any
  codec::RedisValue getAndReduceTokens(const std::string& keyName, const RateLimitArgs& args,
                                       bool strict, Context* ctx);

  std::unique_ptr<std::mutex[]> mutexes_;
};

}  // namespace ratelimit

#endif  // RATELIMIT_RATELIMITHANDLER_H_
