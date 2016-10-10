# ratelimit

A high-performance rate limiter written in C++ that speaks Redis protocol.

## Building from source

* Ensure [Bazel](https://www.bazel.io/) is installed
* Check out the [cpp-build](https://github.com/smyte/cpp-build) repo
* Ensure your submodules are up-to-date: `git submodule update`
* Build the project: `bazel build ratelimit`

## Running it

The binary lives at `./bazel-bin/ratelimit/ratelimit`. It takes a few options:
* `--port`: the TCP port to listen on
* `--rocksdb_db_path`: path where ratelimit should persist its state
* `--rocksdb_create_if_missing_one_off`: pass this flag to create the database if it does not exist
* `--version_timestamp_ms`: this timestamp is stored in the DB when it is created. If this timestamp is older than the one in the DB it will refuse to start. This timestamp must be some time in the future.

## Supported commands
* `RL.REDUCE key max refilltime [REFILL refillamount] [TAKE tokens] [AT timestamp]`: create a bucket identified by `key` if it does not exist that can hold up to `max` tokens and return the number of tokens remaining. Every `refilltime` seconds `refillamount` tokens will be added to the bucket up to `max`. If `refillamount` is not provided it defaults to `max`. If there are at least `tokens` remaining in the bucket it will return true and reduce the amount by `tokens`. If `tokens` is not provided it defaults to `1`. If you want to provide your own current timestamp for refills rather than use the server's clock you can pass a `timestamp`.
* `RL.GET key max refilltime [REFILL refillamount] [TAKE tokens] [AT timestamp]`: same as `RL.REDUCE`, except `RL.GET` does not reduce the number of tokens in the bucket.
* `RL.PREDUCE`: same as `RL.REDUCE`, but uses milliseconds instead of seconds.
* `RL.PGET`: same as `RL.GET`, but uses milliseconds instead of seconds.

### Example

Start the server:
```
./bazel-bin/ratelimit/ratelimit --port 9008 --rocksdb_db_path /data/ratelimit --rocksdb_create_if_missing_one_off --version_timestamp_ms 2461796973728
```

Send it some traffic:
```
redis-cli -p 9008 'RL.REDUCE foo 2 10'
redis-cli -p 9008 'RL.REDUCE foo 2 10'
redis-cli -p 9008 'RL.REDUCE foo 2 10'
```
