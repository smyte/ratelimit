#ifndef RATELIMIT_RATELIMITHANDLER_H_
#define RATELIMIT_RATELIMITHANDLER_H_

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
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
  // The following is needed for sessionization using rate limiter.
  // The basic idea is that requests that are being rate limited are considered belonging to the same session, which is
  // uniquely identified by the combination of rate limit configuration and sessions start time. Session start time is
  // defined as the client time for the request that takes the last token in the bucket.
  // Note that we use STRICT mode for sessionization since requests that come in quick succession should all be grouped
  // into the same session without leaving a gap. A normal, non-strict rate limiter would leave the requests that come
  // after every refill time out of the session.
  // TODO(yunjing): update README to describe sessionization and maybe write a blog post about it.
  struct SessionParams {
    RedisIntType sessionStartedAtMs;
  };
  static_assert(sizeof(SessionParams) == sizeof(RedisIntType) * 1, "Entries in `SessionParams` are not aligned");
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
  template <typename T>
  static rocksdb::Slice encodeRateLimitValue(const T& params, std::string* valueBuf) {
    valueBuf->append(reinterpret_cast<const char *>(&params), sizeof(params));
    return rocksdb::Slice(*valueBuf);
  }

  // Allow others to decode key/value stored in rocksdb
  // ValueParams is required, while SessionizationParams is optional
  static bool decodeRateLimitKey(const rocksdb::Slice& encodedKey, KeyParams* params);
  static bool decodeRateLimitValue(const rocksdb::Slice& encodedValue, ValueParams* params,
                                   SessionParams* sessionParams);

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
      {"rl.sessionize", {static_cast<CommandHandlerFunc>(&RateLimitHandler::rlSessionizeCommand), 3, 10}},
      {"rl.pget", {static_cast<CommandHandlerFunc>(&RateLimitHandler::rlPgetCommand), 3, 8}},
      {"rl.preduce", {static_cast<CommandHandlerFunc>(&RateLimitHandler::rlPreduceCommand), 3, 10}},
      {"rl.psessionize", {static_cast<CommandHandlerFunc>(&RateLimitHandler::rlPsessionizeCommand), 3, 10}},
    }));
    return commandHandlerTable;
  }

  RedisIntType getAdjustedAmountFromDb(const std::string& keyName, const RateLimitArgs& args, std::string* keyBuf,
                                       RedisIntType* newRefilledAtMs, SessionParams* sessionParams);

 private:
  static constexpr int kMaxConcurrentWriters = 1024;

  codec::RedisValue handleRlCommand(const std::vector<std::string>& cmd, bool useMs, bool isReduce, bool isSessionize,
                                    Context* ctx) {
    RateLimitArgs args = {};
    bool strict = false;
    codec::RedisValue parseStatus = parseRateLimitArgs(cmd, useMs, isReduce, &args, &strict);
    if (parseStatus != simpleStringOk()) return parseStatus;
    if (isSessionize) {
      SessionParams sessionParams;
      // By default, each request belongs to its own session, unless rate limit says otherwise
      sessionParams.sessionStartedAtMs = args.clientTimeMs;
      std::vector<codec::RedisValue> result;
      if (!strict) LOG(ERROR) << "Rate limiter for sessionization is not set STRICT explicitly";
      // Sessionization implies strict mode regardless of what the command specifies
      result.push_back(getAndReduceTokens(cmd[1], args, true, &sessionParams, ctx));
      result.emplace_back(sessionParams.sessionStartedAtMs);
      return codec::RedisValue(std::move(result));
    } else {
      return getAndReduceTokens(cmd[1], args, strict, nullptr, ctx);
    }
  }

  // Commands that supports second precision
  codec::RedisValue rlGetCommand(const std::vector<std::string>& cmd, Context* ctx) {
    return handleRlCommand(cmd, false, false, false, ctx);
  }
  codec::RedisValue rlReduceCommand(const std::vector<std::string>& cmd, Context* ctx) {
    return handleRlCommand(cmd, false, true, false, ctx);
  }
  codec::RedisValue rlSessionizeCommand(const std::vector<std::string>& cmd, Context* ctx) {
    // Sessionize uses reduce
    return handleRlCommand(cmd, false, true, true, ctx);
  }

  // Commands that supports millisecond precision
  codec::RedisValue rlPgetCommand(const std::vector<std::string>& cmd, Context* ctx) {
    return handleRlCommand(cmd, true, false, false, ctx);
  }
  codec::RedisValue rlPreduceCommand(const std::vector<std::string>& cmd, Context* ctx) {
    return handleRlCommand(cmd, true, true, false, ctx);
  }
  codec::RedisValue rlPsessionizeCommand(const std::vector<std::string>& cmd, Context* ctx) {
    // Sessionize uses reduce
    return handleRlCommand(cmd, true, true, true, ctx);
  }

  // Get current tokens remaining in the bucket and optionally take the specified amount
  // Note that the returned value is the remaining tokens before taking any
  codec::RedisValue getAndReduceTokens(const std::string& keyName, const RateLimitArgs& args,
                                       bool strict, SessionParams* sessionParams, Context* ctx);

  std::unique_ptr<std::mutex[]> mutexes_;
};

}  // namespace ratelimit

#endif  // RATELIMIT_RATELIMITHANDLER_H_
