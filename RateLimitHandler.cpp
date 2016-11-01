#include "ratelimit/RateLimitHandler.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include "boost/algorithm/string/case_conv.hpp"
#include "folly/Conv.h"
#include "folly/Format.h"
#include "glog/logging.h"
#include "rocksdb/options.h"
#include "rocksdb/slice.h"
#include "rocksdb/status.h"

namespace ratelimit {

codec::RedisValue RateLimitHandler::getAndReduceTokens(const std::string& keyName, const RateLimitArgs& args,
                                                       bool strict, RateLimitHandler::SessionParams* sessionParams,
                                                       Context* ctx) {
  RedisIntType adjustedAmount;
  // unlock ASAP (before writing the response to client)
  {
    // use hashed key name as mutex index to prevent concurrent writes
    size_t mutexIndex = std::hash<std::string>()(keyName) % kMaxConcurrentWriters;
    std::lock_guard<std::mutex> _guard(mutexes_[mutexIndex]);

    std::string key;
    RedisIntType newRefilledAtMs;
    adjustedAmount = getAdjustedAmountFromDb(keyName, args, &key, &newRefilledAtMs, sessionParams);
    if (args.tokenAmount > 0) {
      RedisIntType newAmount = std::max(adjustedAmount - args.tokenAmount, 0L);
      ValueParams valueParams{ newAmount, newRefilledAtMs, nowMs() };
      // In strict mode, once new amount reaches 0, we stop refilling until client waited at least
      // one full refill time by keeping advancing refilled at time to current client time
      if (strict && newAmount == 0) valueParams.lastRefilledAtMs = args.clientTimeMs;
      std::string valueBuf;
      rocksdb::Slice newValue = encodeRateLimitValue(valueParams, &valueBuf);
      if (sessionParams) {
        if (adjustedAmount >= args.tokenAmount) {
          // Start a new session when there are enough tokens remain
          // Once tokens are exhausted, subsequent requests will get the same sessionStartedAtMs until refill
          sessionParams->sessionStartedAtMs = args.clientTimeMs;
        }
        newValue = encodeRateLimitValue(*sessionParams, &valueBuf);
      }
      rocksdb::Status status = db()->Put(rocksdb::WriteOptions(), key, newValue);
      if (!status.ok()) {
        return errorResp(folly::sformat("RocksDB error: {}", status.ToString()));
      }
    }
  }

  return codec::RedisValue(adjustedAmount);
}

RateLimitHandler::RedisIntType RateLimitHandler::getAdjustedAmountFromDb(
    const std::string& keyName, const RateLimitHandler::RateLimitArgs& args, std::string* keyBuf,
    RateLimitHandler::RedisIntType* newRefilledAtMs, RateLimitHandler::SessionParams* sessionParams) {
  KeyParams keyParams{ args.maxAmount, args.refillAmount, args.refillTimeMs };
  rocksdb::Slice key = encodeRateLimitKey(keyName, keyParams, keyBuf);

  std::string encodedValue;
  rocksdb::Status status = db()->Get(rocksdb::ReadOptions(), key, &encodedValue);
  if (status.ok()) {
    ValueParams valueParams;
    CHECK(decodeRateLimitValue(encodedValue, &valueParams, sessionParams))
        << "RateLimit value in RocksDB is corrupted";
    return adjustAmount(valueParams.amount, valueParams.lastRefilledAtMs, args, newRefilledAtMs);
  } else {
    if (!status.IsNotFound()) {
      LOG(ERROR) << "RocksDB Get Error: " << status.ToString();
    }
    // no such key means the full amount is available
    *newRefilledAtMs = args.clientTimeMs;
    return args.maxAmount;
  }
}

RateLimitHandler::RedisIntType RateLimitHandler::adjustAmount(RateLimitHandler::RedisIntType currAmount,
    RateLimitHandler::RedisIntType lastRefilledAtMs, const RateLimitHandler::RateLimitArgs& args,
    RateLimitHandler::RedisIntType* newRefilledAtMs) {
  RedisIntType timeSpan = std::max(0L, args.clientTimeMs - lastRefilledAtMs);
  RedisIntType refills = timeSpan / args.refillTimeMs;
  // advance refilled at to the latest refill mark
  *newRefilledAtMs = lastRefilledAtMs + refills * args.refillTimeMs;
  return std::min(args.maxAmount, refills * args.refillAmount + currAmount);
}

codec::RedisValue RateLimitHandler::parseRateLimitArgs(const std::vector<std::string>& cmd, bool useMs, bool isReduce,
                                                       RateLimitHandler::RateLimitArgs* args, bool* strict) {
  // Timestamps are in milliseconds internally, so multiply by 1000 when clients are not using milliseconds
  int64_t tsMultiplier = useMs ? 1 : 1000;
  try {
    // required arguments, whose existence is checked by the framework
    args->maxAmount = folly::to<RedisIntType>(cmd[2]);
    args->refillTimeMs = static_cast<RedisIntType>(folly::to<int32_t>(cmd[3])) * tsMultiplier;

    // optional arguments with default values
    args->refillAmount = args->maxAmount;
    args->tokenAmount = isReduce ? 1 : 0;
    args->clientTimeMs = nowMs();
    // strict mode is not part of the rate limit configuration but a client-side toggle
    *strict = false;
    size_t i = 4;
    while (i < cmd.size()) {
      std::string argLower = boost::to_lower_copy(cmd[i]);
      // `strict` does not have an argument value
      if (argLower == "strict") {
        *strict = true;
        i++;
        continue;
      }
      // all others have an argument value
      if (i + 1 >= cmd.size()) return errorSyntaxError();
      RedisIntType value = folly::to<RedisIntType>(cmd[i + 1]);
      i += 2;
      if (argLower == "refill") {
        args->refillAmount = value;
      } else if (argLower == "take") {
        // you can only set TAKE in reduce operation
        if (!isReduce) return errorSyntaxError();
        args->tokenAmount = value;
      } else if (argLower == "at") {
        args->clientTimeMs = value * tsMultiplier;
      } else {
        return errorSyntaxError();
      }
    }

    if (args->maxAmount < 1 || args->refillTimeMs < 1  || args->refillAmount < 1 || args->tokenAmount < 0 ||
        args->clientTimeMs < 0) {
      return errorInvalidInteger();
    }
    return simpleStringOk();
  } catch (std::range_error&) {
    return errorInvalidInteger();
  }
}

rocksdb::Slice RateLimitHandler::encodeRateLimitKey(const std::string& keyName, const KeyParams& params,
                                                    std::string* keyBuf) {
  keyBuf->append(keyName);
  // Use fixed-length encoding to avoid conflicts when concatenated with the key name
  keyBuf->append(reinterpret_cast<const char *>(&params), sizeof(params));
  return rocksdb::Slice(*keyBuf);
}

bool RateLimitHandler::decodeRateLimitKey(const rocksdb::Slice& encodedKey, RateLimitHandler::KeyParams* params) {
  // the key should contain at least one char as key name and the key parameters
  if (encodedKey.size_ < sizeof(KeyParams)) return false;
  // assume native endian here, which means we cannot ship encodedValue across different machine architectures
  // this is the fastest way of for fixed-length encoding
  size_t start = encodedKey.size_ - sizeof(KeyParams);  // skip key name
  std::memcpy(params, encodedKey.data_ + start, sizeof(KeyParams));
  return true;
}

bool RateLimitHandler::decodeRateLimitValue(const rocksdb::Slice& encodedValue, RateLimitHandler::ValueParams* params,
                                            RateLimitHandler::SessionParams* sessionParams) {
  if (encodedValue.size() < sizeof(ValueParams)) return false;
  // assume native endian here, which means we cannot ship encodedValue across different machine architectures
  // this is the fastest way of for fixed-length encoding
  std::memcpy(params, encodedValue.data_, sizeof(RateLimitHandler::ValueParams));
  if (sessionParams) {
    if (encodedValue.size() != sizeof(ValueParams) + sizeof(SessionParams)) return false;
    std::memcpy(sessionParams, encodedValue.data_ + sizeof(ValueParams), sizeof(SessionParams));
  }
  return true;
}

constexpr int RateLimitHandler::kMaxConcurrentWriters;

}  // namespace ratelimit
