cc_binary(
    name = "ratelimit",
    srcs = [
        "RateLimitServer.cpp",
    ],
    deps = [
        ":ratelimit_handler",
        "//pipeline:redis_pipeline_bootstrap",
    ],
    copts = [
        "-std=c++14",
    ],
)

cc_library(
    name = "ratelimit_handler",
    srcs = [
        "RateLimitCompactionFilter.cpp",
        "RateLimitHandler.cpp",
    ],
    hdrs = [
        "RateLimitCompactionFilter.h",
        "RateLimitHandler.h",
    ],
    deps = [
        "//codec:redis_value",
        "//external:boost",
        "//external:folly",
        "//external:glog",
        "//external:rocksdb",
        "//external:wangle",
        "//pipeline:redis_handler",
    ],
    copts = [
        "-std=c++14",
    ],
)

cc_test(
    name = "ratelimit_handler_test",
    srcs = [
        "RateLimitHandlerTest.cpp",
    ],
    size = "small",
    deps = [
        ":ratelimit_handler",
        "//codec:redis_value",
        "//external:folly",
        "//external:gmock_main",
        "//external:gtest",
        "//external:rocksdb",
        "//external:wangle",
        "//stesting:test_helpers",
    ],
    copts = [
        "-std=c++14",
    ],
)
