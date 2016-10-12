# ratelimit

A high-performance rate limiter written in C++ that speaks Redis protocol. See our [blog post](https://medium.com/the-smyte-blog/rate-limiter-df3408325846) to learn more.

## Building from source

* Ensure [Bazel](https://www.bazel.io/) is installed
* Check out the [cpp-build](https://github.com/smyte/cpp-build) repo
* Ensure your submodules are up-to-date: `git submodule update`
* Build the project: `bazel build -c opt ratelimit`

## Running it

Once compiled (as above), the binary lives at `./bazel-bin/ratelimit/ratelimit`.

It takes a few options:
* `--port`: the TCP port to listen on  (default 9049)
* `--rocksdb_db_path`: path where ratelimit should persist its state
* `--rocksdb_create_if_missing`: pass this flag to create the database if it does not exist

Example: `./bazel-bin/ratelimit/ratelimit --rocksdb_db_path ratelimit-data --rocksdb_create_if_missing`

You can also run it inside docker: `docker run -p 9049:9049 smyte/ratelimit`

## Supported commands
* `RL.REDUCE key max refilltime [REFILL refillamount] [TAKE tokens] [AT timestamp]`: create a bucket identified by `key` if it does not exist that can hold up to `max` tokens and return the number of tokens remaining. Every `refilltime` seconds `refillamount` tokens will be added to the bucket up to `max`. If `refillamount` is not provided it defaults to `max`. If there are at least `tokens` remaining in the bucket it will return true and reduce the amount by `tokens`. If `tokens` is not provided it defaults to `1`. If you want to provide your own current timestamp for refills rather than use the server's clock you can pass a `timestamp`.
* `RL.GET key max refilltime [REFILL refillamount] [AT timestamp]`: same as `RL.REDUCE`, except `RL.GET` does not reduce the number of tokens in the bucket.
* `RL.PREDUCE`: same as `RL.REDUCE`, but uses milliseconds instead of seconds.
* `RL.PGET`: same as `RL.GET`, but uses milliseconds instead of seconds.

### Example

Start the compiled server (see section on [running it](#running-it))

Send it some traffic:
```
$ redis-cli -p 9049 RL.REDUCE twoPerMin 2 60
(integer) 2
$ redis-cli -p 9049 RL.REDUCE twoPerMin 2 60
(integer) 1
$ redis-cli -p 9049 RL.REDUCE twoPerMin 2 60
(integer) 0
```
