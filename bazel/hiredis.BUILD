load("@rules_foreign_cc//foreign_cc:defs.bzl", "cmake")

package(default_visibility = ["//visibility:public"])

filegroup(
    name = "all_srcs",
    srcs = glob(["**"]),
)

cmake(
    name = "hiredis",
    lib_source = ":all_srcs",
    generate_args = ["-GNinja"],
    cache_entries = {
        "CMAKE_POSITION_INDEPENDENT_CODE": "ON",
    },
    out_include_dir = "include",
    out_static_libs = ["libhiredis.a"],
)
