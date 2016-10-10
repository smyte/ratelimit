#include <memory>
#include <string>
#include <vector>

#include "codec/RedisValue.h"
#include "folly/String.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "ratelimit/RateLimitCompactionFilter.h"
#include "ratelimit/RateLimitHandler.h"
#include "rocksdb/db.h"
#include "stesting/TestWithRocksDb.h"
#include "wangle/channel/Handler.h"

namespace ratelimit {

class RateLimitHandlerTest : public stesting::TestWithRocksDb {
 protected:
  RateLimitHandlerTest() : stesting::TestWithRocksDb({}, { {"default", RateLimitHandler::optimizeColumnFamily}}) {}
};

class MockRateLimitHandler : public RateLimitHandler {
 public:
  explicit MockRateLimitHandler(std::shared_ptr<pipeline::DatabaseManager> databaseManager)
      : RateLimitHandler(databaseManager) {}

  MOCK_METHOD2(write, folly::Future<folly::Unit>(Context*, codec::RedisValue));
};

TEST_F(RateLimitHandlerTest, EncodeDecodeRateLimitKey) {
  std::string keyName = "abc";
  RateLimitHandler::KeyParams inputKeyParams{ 100, 5, 20 };
  std::string key;
  RateLimitHandler::encodeRateLimitKey(keyName, inputKeyParams, &key);
  EXPECT_EQ(keyName, key.substr(0, 3));
  EXPECT_EQ(keyName.size() + 3 * sizeof(RateLimitHandler::RedisIntType), key.size());

  RateLimitHandler::KeyParams outputKeyParams;
  ASSERT_TRUE(RateLimitHandler::decodeRateLimitKey(key, &outputKeyParams));
  EXPECT_EQ(inputKeyParams.maxAmount, outputKeyParams.maxAmount);
  EXPECT_EQ(inputKeyParams.refillAmount, outputKeyParams.refillAmount);
  EXPECT_EQ(inputKeyParams.refillTimeMs, outputKeyParams.refillTimeMs);
}

TEST_F(RateLimitHandlerTest, EncodeDecodeRateLimitValue) {
  RateLimitHandler::ValueParams inputValueParams{ 100, 10000, nowMs() };
  std::string value;
  RateLimitHandler::encodeRateLimitValue(inputValueParams, &value);
  EXPECT_EQ(3 * sizeof(RateLimitHandler::RedisIntType), value.size());

  RateLimitHandler::ValueParams outputValueParams;
  ASSERT_TRUE(RateLimitHandler::decodeRateLimitValue(value, &outputValueParams));
  EXPECT_EQ(inputValueParams.amount, outputValueParams.amount);
  EXPECT_EQ(inputValueParams.lastRefilledAtMs, outputValueParams.lastRefilledAtMs);
  EXPECT_EQ(inputValueParams.lastReducedAtMs, outputValueParams.lastReducedAtMs);
}

TEST_F(RateLimitHandlerTest, RateLimitCompactionFilterTrue) {
  RateLimitCompactionFilter filter;
  std::string newValue;
  bool valueChanged;

  std::string keyName = "abc";
  // refill to max after 20 minutes
  RateLimitHandler::KeyParams keyParams{ 100, 5, 60000 };
  std::string key;
  RateLimitHandler::encodeRateLimitKey(keyName, keyParams, &key);
  RateLimitHandler::ValueParams valueParams{ 100, 10000, nowMs() - 1800 * 1000 };  // accessed 30 minutes ago
  std::string value;
  RateLimitHandler::encodeRateLimitValue(valueParams, &value);

  EXPECT_TRUE(filter.Filter(0, key, value, &newValue, &valueChanged));
  EXPECT_FALSE(valueChanged);
}

TEST_F(RateLimitHandlerTest, RateLimitCompactionFilterFalse) {
  RateLimitCompactionFilter filter;
  std::string newValue;
  bool valueChanged;

  std::string keyName = "abc";
  // refill to max after 20 minutes
  RateLimitHandler::KeyParams keyParams{ 100, 5, 60000 };
  std::string key;
  RateLimitHandler::encodeRateLimitKey(keyName, keyParams, &key);
  RateLimitHandler::ValueParams valueParams{ 100, 10000, nowMs() - 600 * 1000 };  // accessed 10 minutes ago
  std::string value;
  RateLimitHandler::encodeRateLimitValue(valueParams, &value);

  EXPECT_FALSE(filter.Filter(0, key, value, &newValue, &valueChanged));
  EXPECT_FALSE(valueChanged);
}

TEST_F(RateLimitHandlerTest, AdjustAmount) {
  RateLimitHandler::RedisIntType newRefilledAtMs;
  RateLimitHandler::RedisIntType lastRefilledAtMs = 2000;

  // No refill
  RateLimitHandler::RedisIntType clientTimeMs1 = 53000;
  RateLimitHandler::RateLimitArgs args1{ 1000, 60000, 100, 1, clientTimeMs1 };
  EXPECT_EQ(100, RateLimitHandler::adjustAmount(100, lastRefilledAtMs, args1, &newRefilledAtMs));
  EXPECT_EQ(2000, newRefilledAtMs);

  // Partial refill
  RateLimitHandler::RedisIntType clientTimeMs2 = 123000;
  RateLimitHandler::RateLimitArgs args2{ 1000, 60000, 100, 1, clientTimeMs2 };
  EXPECT_EQ(300, RateLimitHandler::adjustAmount(100, lastRefilledAtMs, args2, &newRefilledAtMs));
  EXPECT_EQ(122000, newRefilledAtMs);

  // Full refill
  RateLimitHandler::RedisIntType clientTimeMs3 = 1312000;
  RateLimitHandler::RateLimitArgs args3{ 1000, 60000, 100, 1, clientTimeMs3 };
  EXPECT_EQ(args3.maxAmount, RateLimitHandler::adjustAmount(100, lastRefilledAtMs, args3, &newRefilledAtMs));
  // full refill adjusts refilled at
  EXPECT_EQ(1262000, newRefilledAtMs);
}

TEST_F(RateLimitHandlerTest, ParseRateLimitArgsWithOutErrors) {
  std::vector<std::string> cmd1;
  folly::split(" ", "rl.get abc 10 60", cmd1);
  RateLimitHandler::RateLimitArgs args1 = {};
  bool strict1 = false;
  EXPECT_EQ(RateLimitHandler::simpleStringOk(),
            RateLimitHandler::parseRateLimitArgs(cmd1, false, false, &args1, &strict1));
  EXPECT_EQ(10, args1.maxAmount);
  EXPECT_EQ(60000, args1.refillTimeMs);
  EXPECT_FALSE(strict1);
  // default values
  EXPECT_EQ(args1.maxAmount, args1.refillAmount);
  EXPECT_EQ(0, args1.tokenAmount);
  EXPECT_LE(args1.clientTimeMs, RateLimitHandler::nowMs());
  EXPECT_GE(args1.clientTimeMs, RateLimitHandler::nowMs() - 10 * 1000);

  std::vector<std::string> cmd2;
  folly::split(" ", "rl.pget abc 10 500", cmd2);
  RateLimitHandler::RateLimitArgs args2 = {};
  bool strict2 = false;
  EXPECT_EQ(RateLimitHandler::simpleStringOk(),
            RateLimitHandler::parseRateLimitArgs(cmd2, true, false, &args2, &strict2));
  EXPECT_EQ(10, args2.maxAmount);
  EXPECT_EQ(500, args2.refillTimeMs);
  EXPECT_FALSE(strict2);
  // default values
  EXPECT_EQ(args2.maxAmount, args2.refillAmount);
  EXPECT_EQ(0, args2.tokenAmount);
  EXPECT_LE(args2.clientTimeMs, RateLimitHandler::nowMs());
  EXPECT_GE(args2.clientTimeMs, RateLimitHandler::nowMs() - 10 * 1000);

  std::vector<std::string> cmd3;
  folly::split(" ", "rl.reduce abc 10 500 REFILL 5 take 2 at 1005", cmd3);
  RateLimitHandler::RateLimitArgs args3 = {};
  bool strict3 = false;
  EXPECT_EQ(RateLimitHandler::simpleStringOk(),
            RateLimitHandler::parseRateLimitArgs(cmd3, false, true, &args3, &strict3));
  EXPECT_EQ(10, args3.maxAmount);
  EXPECT_EQ(500000, args3.refillTimeMs);
  EXPECT_EQ(5, args3.refillAmount);
  EXPECT_EQ(2, args3.tokenAmount);
  EXPECT_EQ(1005000, args3.clientTimeMs);
  EXPECT_FALSE(strict3);

  std::vector<std::string> cmd4;
  folly::split(" ", "rl.preduce abc 20 500 at 1005 take 2", cmd4);
  RateLimitHandler::RateLimitArgs args4 = {};
  bool strict4 = false;
  EXPECT_EQ(RateLimitHandler::simpleStringOk(),
            RateLimitHandler::parseRateLimitArgs(cmd4, true, true, &args4, &strict4));
  EXPECT_EQ(20, args4.maxAmount);
  EXPECT_EQ(500, args4.refillTimeMs);
  EXPECT_EQ(2, args4.tokenAmount);
  EXPECT_EQ(1005, args4.clientTimeMs);
  EXPECT_FALSE(strict4);
  // default value
  EXPECT_EQ(args4.maxAmount, args4.refillAmount);

  std::vector<std::string> cmd5;
  folly::split(" ", "rl.get abc 10 700 REFILL 3 at 2005", cmd5);
  RateLimitHandler::RateLimitArgs args5 = {};
  bool strict5 = false;
  EXPECT_EQ(RateLimitHandler::simpleStringOk(),
            RateLimitHandler::parseRateLimitArgs(cmd5, false, false, &args5, &strict5));
  EXPECT_EQ(10, args5.maxAmount);
  EXPECT_EQ(700000, args5.refillTimeMs);
  EXPECT_EQ(2005000, args5.clientTimeMs);
  EXPECT_EQ(3, args5.refillAmount);
  EXPECT_FALSE(strict5);
  // default value
  EXPECT_EQ(0, args5.tokenAmount);

  std::vector<std::string> cmd6;
  folly::split(" ", "rl.pget abc 20 700 at 3005", cmd6);
  RateLimitHandler::RateLimitArgs args6 = {};
  bool strict6 = false;
  EXPECT_EQ(RateLimitHandler::simpleStringOk(),
            RateLimitHandler::parseRateLimitArgs(cmd6, true, false, &args6, &strict6));
  EXPECT_EQ(20, args6.maxAmount);
  EXPECT_EQ(700, args6.refillTimeMs);
  EXPECT_EQ(3005, args6.clientTimeMs);
  EXPECT_FALSE(strict6);
  // default value
  EXPECT_EQ(args6.maxAmount, args6.refillAmount);
  EXPECT_EQ(0, args6.tokenAmount);

  std::vector<std::string> cmd7;
  folly::split(" ", "rl.reduce abc 20 700 STRICT at 3005 REFILL 4", cmd7);
  RateLimitHandler::RateLimitArgs args7 = {};
  bool strict7 = false;
  EXPECT_EQ(RateLimitHandler::simpleStringOk(),
            RateLimitHandler::parseRateLimitArgs(cmd7, true, true, &args7, &strict7));
  EXPECT_EQ(20, args7.maxAmount);
  EXPECT_EQ(700, args7.refillTimeMs);
  EXPECT_EQ(3005, args7.clientTimeMs);
  EXPECT_EQ(4, args7.refillAmount);
  EXPECT_TRUE(strict7);
  // default value
  EXPECT_EQ(1, args7.tokenAmount);

  std::vector<std::string> cmd8;
  folly::split(" ", "rl.preduce abc 20 700000 at 3005000 REFILL 4 TAKE 3 STRICT", cmd8);
  RateLimitHandler::RateLimitArgs args8 = {};
  bool strict8 = false;
  EXPECT_EQ(RateLimitHandler::simpleStringOk(),
            RateLimitHandler::parseRateLimitArgs(cmd8, true, true, &args8, &strict8));
  EXPECT_EQ(20, args8.maxAmount);
  EXPECT_EQ(700000, args8.refillTimeMs);
  EXPECT_EQ(3005000, args8.clientTimeMs);
  EXPECT_EQ(4, args8.refillAmount);
  EXPECT_EQ(3, args8.tokenAmount);
  EXPECT_TRUE(strict8);
}

TEST_F(RateLimitHandlerTest, ParseRateLimitArgsWithErrors) {
  std::vector<std::string> cmd;
  folly::split(" ", "rl.get abc 10 abc", cmd);
  RateLimitHandler::RateLimitArgs args = {};
  bool strict = false;
  EXPECT_EQ(RateLimitHandler::errorInvalidInteger(),
            RateLimitHandler::parseRateLimitArgs(cmd, false, false, &args, &strict));

  cmd.clear();
  folly::split(" ", "rl.pget abc 0 60", cmd);
  EXPECT_EQ(RateLimitHandler::errorInvalidInteger(),
            RateLimitHandler::parseRateLimitArgs(cmd, true, false, &args, &strict));

  cmd.clear();
  folly::split(" ", "rl.reduce abc 10 60 refill strict 1", cmd);
  EXPECT_EQ(RateLimitHandler::errorInvalidInteger(),
            RateLimitHandler::parseRateLimitArgs(cmd, false, true, &args, &strict));

  cmd.clear();
  folly::split(" ", "rl.pget abc 0 60 take 2", cmd);
  EXPECT_EQ(RateLimitHandler::errorSyntaxError(),
            RateLimitHandler::parseRateLimitArgs(cmd, true, false, &args, &strict));

  cmd.clear();
  folly::split(" ", "rl.reduce abc 10 60 refill", cmd);
  EXPECT_EQ(RateLimitHandler::errorSyntaxError(),
            RateLimitHandler::parseRateLimitArgs(cmd, false, true, &args, &strict));

  cmd.clear();
  folly::split(" ", "rl.preduce abc 10 60 refill 1 strict abc", cmd);
  EXPECT_EQ(RateLimitHandler::errorSyntaxError(),
            RateLimitHandler::parseRateLimitArgs(cmd, true, true, &args, &strict));
}

TEST_F(RateLimitHandlerTest, GetReduceCommands) {
  MockRateLimitHandler handler(databaseManager());
  std::vector<std::string> cmd;
  // have 10 left initially, and reduce 1 by default
  EXPECT_CALL(handler, write(nullptr, codec::RedisValue(10))).Times(1);
  folly::split(" ", "rl.reduce a 10 5 refill 3 at 2", cmd);
  EXPECT_TRUE(handler.handleCommand("rl.reduce", cmd, nullptr));

  // reduce 1 by default
  cmd.clear();
  folly::split(" ", "rl.preduce a 10 5000 at 2000 refill 3", cmd);
  EXPECT_CALL(handler, write(nullptr, codec::RedisValue(9))).Times(1);
  EXPECT_TRUE(handler.handleCommand("rl.preduce", cmd, nullptr));

  // 8 left after two reduces
  cmd.clear();
  folly::split(" ", "rl.get a 10 5 at 2 refill 3", cmd);
  EXPECT_CALL(handler, write(nullptr, codec::RedisValue(8))).Times(1);
  EXPECT_TRUE(handler.handleCommand("rl.get", cmd, nullptr));
  // `p` version has the same result
  cmd.clear();
  folly::split(" ", "rl.pget a 10 5000 refill 3 at 2000", cmd);
  EXPECT_CALL(handler, write(nullptr, codec::RedisValue(8))).Times(1);
  EXPECT_TRUE(handler.handleCommand("rl.pget", cmd, nullptr));

  // reduce by an explicit amount
  cmd.clear();
  folly::split(" ", "rl.reduce a 10 5 at 2 refill 3 take 5", cmd);
  EXPECT_CALL(handler, write(nullptr, codec::RedisValue(8))).Times(1);
  EXPECT_TRUE(handler.handleCommand("rl.reduce", cmd, nullptr));

  // 3 left after reduce by 5, reduce by an explicit amount
  cmd.clear();
  folly::split(" ", "rl.preduce a 10 5000 refill 3 take 5 at 2000", cmd);
  EXPECT_CALL(handler, write(nullptr, codec::RedisValue(3))).Times(1);
  EXPECT_TRUE(handler.handleCommand("rl.preduce", cmd, nullptr));

  // not enough left after the second reduce 5
  cmd.clear();
  folly::split(" ", "rl.get a 10 5 at 2 refill 3", cmd);
  EXPECT_CALL(handler, write(nullptr, codec::RedisValue(0))).Times(1);
  EXPECT_TRUE(handler.handleCommand("rl.get", cmd, nullptr));

  // refill with 3
  cmd.clear();
  folly::split(" ", "rl.get a 10 5 at 8 refill 3", cmd);
  EXPECT_CALL(handler, write(nullptr, codec::RedisValue(3))).Times(1);
  EXPECT_TRUE(handler.handleCommand("rl.get", cmd, nullptr));

  // reduce 2 after refill
  cmd.clear();
  folly::split(" ", "rl.reduce a 10 5 at 8 take 2 refill 3", cmd);
  EXPECT_CALL(handler, write(nullptr, codec::RedisValue(3))).Times(1);
  EXPECT_TRUE(handler.handleCommand("rl.reduce", cmd, nullptr));

  // 1 left after reduce and take them all
  cmd.clear();
  folly::split(" ", "rl.preduce a 10 5000 at 8000 refill 3", cmd);
  EXPECT_CALL(handler, write(nullptr, codec::RedisValue(1))).Times(1);
  EXPECT_TRUE(handler.handleCommand("rl.preduce", cmd, nullptr));

  // nothing left
  cmd.clear();
  folly::split(" ", "rl.preduce a 10 5000 at 9000 refill 3", cmd);
  EXPECT_CALL(handler, write(nullptr, codec::RedisValue(0))).Times(1);
  EXPECT_TRUE(handler.handleCommand("rl.preduce", cmd, nullptr));

  // get refilled again
  cmd.clear();
  folly::split(" ", "rl.get a 10 5 at 13 refill 3", cmd);
  EXPECT_CALL(handler, write(nullptr, codec::RedisValue(3))).Times(1);
  EXPECT_TRUE(handler.handleCommand("rl.get", cmd, nullptr));

  // drain the bucket in strict mode
  cmd.clear();
  folly::split(" ", "rl.reduce a 10 5 at 14 refill 3 take 4 strict", cmd);
  EXPECT_CALL(handler, write(nullptr, codec::RedisValue(3))).Times(1);
  EXPECT_TRUE(handler.handleCommand("rl.reduce", cmd, nullptr));

  // no token left before next refill
  cmd.clear();
  folly::split(" ", "rl.reduce a 10 5 at 16 refill 3 take 4 strict", cmd);
  EXPECT_CALL(handler, write(nullptr, codec::RedisValue(0))).Times(1);
  EXPECT_TRUE(handler.handleCommand("rl.reduce", cmd, nullptr));

  // still no token left after next refill time in strict mode
  cmd.clear();
  folly::split(" ", "rl.reduce a 10 5 at 18 refill 3 take 1 strict", cmd);
  EXPECT_CALL(handler, write(nullptr, codec::RedisValue(0))).Times(1);
  EXPECT_TRUE(handler.handleCommand("rl.reduce", cmd, nullptr));

  // refill again once waited the full refill time
  cmd.clear();
  folly::split(" ", "rl.reduce a 10 5 at 23 refill 3 take 2 strict", cmd);
  EXPECT_CALL(handler, write(nullptr, codec::RedisValue(3))).Times(1);
  EXPECT_TRUE(handler.handleCommand("rl.reduce", cmd, nullptr));
}

}  // namespace ratelimit
