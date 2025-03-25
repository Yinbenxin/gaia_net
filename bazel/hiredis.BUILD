load("@rules_foreign_cc//foreign_cc:defs.bzl", "cmake")

package(default_visibility = ["//visibility:public"])

filegroup(
    name = "all_srcs",
    srcs = glob(["**"]),
)

cmake(
    name = "hiredis",
    lib_source = ":all_srcs",
    # out_headers_only = True,
    out_static_libs = ["libhiredis.a"],
    cache_entries = {
        "ENABLE_SSL": "OFF",
        "BUILD_SHARED_LIBS": "OFF",
        "BUILD_STATIC_LIBS": "ON",
    },
)
