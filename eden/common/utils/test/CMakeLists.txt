# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.


add_executable(
  process_name_cache_test
    "ProcessNameCacheTest.cpp"
)

target_link_libraries(
  process_name_cache_test
  PUBLIC
    edencommon_utils
    Folly::folly_test_util
    ${LIBGMOCK_LIBRARIES}
)

gtest_discover_tests(process_name_cache_test)

add_executable(
  string_conv_test
    "StringConvTest.cpp"
)

target_link_libraries(
  string_conv_test
  PUBLIC
    edencommon_utils
    Folly::folly_test_util
    ${LIBGMOCK_LIBRARIES}
)

gtest_discover_tests(string_conv_test)

add_executable(
  option_set_test
    "OptionSetTest.cpp"
)

target_link_libraries(
  option_set_test
  PUBLIC
    edencommon_utils
    Folly::folly_test_util
    ${LIBGMOCK_LIBRARIES}
)

gtest_discover_tests(option_set_test)
