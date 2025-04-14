cc_test(
    name = "channel_test",
    srcs = ["channel_test.cpp"],
    linkopts = [
        "-framework CoreFoundation",
        "-framework Foundation",
    ],
    deps = [
        "//src:gaia_net",
        "@com_github_gflags_gflags//:gflags",  # 添加 gflags 依赖
        "@com_github_grpc_grpc//:grpc++",      # 添加 gRPC 依赖
        "@com_google_protobuf//:protobuf",     # 添加 protobuf 依赖
        "@com_github_grpc_grpc//:grpc",
    ],
)

cc_test(
    name = "channel_bench",
    srcs = ["channel_bench.cpp"],
    linkopts = [
        "-framework CoreFoundation",
        "-framework Foundation",
    ],
    deps = [
        "//src:gaia_net",
        "@com_github_fmtlib_fmt//:fmtlib",  
        "@com_github_gflags_gflags//:gflags",
        "@com_github_lz4//:lz4",
        "@com_github_hiredis//:hiredis",
        "@com_github_redis_plus_plus//:redis_plus_plus",
        "@com_github_nlohmann_json//:json",
        "@com_google_protobuf//:protoc_lib",
        "@com_google_protobuf//:protobuf",
        "@com_github_grpc_grpc//:grpc",
        "@com_github_grpc_grpc//:grpc++",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "connect_test",
    srcs = ["connect_test.cpp"],
    deps = [
        "//src:gaia_net",
    ]
)

cc_test(
    name = "redis_test",
    srcs = ["redis_test.cc"],
    deps = [
        "@com_google_googletest//:gtest_main",
    ]
)

cc_test(
    name = "test_grpc_memleak",
    srcs = ["test_grpc_memleak.cpp"],
    deps = [
        "//src:gaia_net",
    ]
)

cc_test(
    name = "util_test",
    srcs = ["util_test.cc"],
    deps = [
        "//src:gaia_net",
    ]
)
