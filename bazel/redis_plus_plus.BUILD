load("@rules_foreign_cc//foreign_cc:defs.bzl", "cmake")

package(default_visibility = ["//visibility:public"])

filegroup(
    name = "all_srcs",
    srcs = glob(["**"]),
)

cmake(
    name = "redis_plus_plus",
    lib_source = ":all_srcs",
    cache_entries = {
        "CMAKE_POSITION_INDEPENDENT_CODE": "ON",
        "BUILD_SHARED_LIBS": "OFF",
        "REDIS_PLUS_PLUS_BUILD_TEST": "OFF",  # 禁用测试构建
        "REDIS_PLUS_PLUS_BUILD_SHARED": "OFF",  # 只构建静态库
        "REDIS_PLUS_PLUS_CXX_STANDARD": "17",  # 设置 C++ 标准
        "CMAKE_INSTALL_LIBDIR": "lib",

    },
    out_include_dir = "include",
    out_static_libs = ["libredis++.a"],
    deps = ["@com_github_hiredis//:hiredis"],
    working_directory = ".",
    install = True,
)