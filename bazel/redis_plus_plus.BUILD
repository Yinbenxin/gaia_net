 
load("@rules_foreign_cc//foreign_cc:defs.bzl", "cmake")

package(default_visibility = ["//visibility:public"])

filegroup(
    name = "all_srcs",
    srcs = glob(["**"]),
)

cmake(
    name = "redis_plus_plus",
    lib_source = ":all_srcs",
    # out_headers_only = True,
    deps = ["@com_github_hiredis//:hiredis"],
    out_static_libs = ["libredis++.a"],
    cache_entries = {
        "REDIS_PLUS_PLUS_BUILD_SHARED": "OFF",
        "REDIS_PLUS_PLUS_BUILD_STATIC": "ON",
    },
)

