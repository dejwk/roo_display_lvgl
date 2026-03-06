load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

cc_library(
    name = "roo_display_lvgl",
    srcs = glob(
        [
            "src/**/*.cpp",
            "src/**/*.h",
        ],
        exclude = ["test/**"],
    ),
    includes = [
        "src",
    ],
    linkstatic = 1,
    visibility = ["//visibility:public"],
    deps = [
        "@lvgl//:lvgl",
        "@roo_display",
    ],
)
