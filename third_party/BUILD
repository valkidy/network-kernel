# Copyright 2019 The MediaPipe Authors.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

# load("@bazel_skylib//:bzl_library.bzl", "bzl_library")
# load("@rules_foreign_cc//foreign_cc:cmake.bzl", "cmake")

licenses(["notice"])  # Apache License 2.0

package(default_visibility = ["//visibility:public"])

alias(
    name = "zlib",
    actual = select({
        "//mediapipe:macos": "@macos_zlib//:zlib",
        # "//mediapipe:mingw_w64": "@zlib//:zlib",
        "//conditions:default": "@macos_zlib//:zlib",
    }),
)

alias(
    name = "openssl",
    actual = select({
        "//mediapipe:macos": "@macos_openssl//:openssl",
        # "//mediapipe:mingw_w64": "@zlib//:zlib",
        "//conditions:default": "@macos_openssl//:openssl",
    }),
)
