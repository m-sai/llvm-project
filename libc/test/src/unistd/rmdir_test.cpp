//===-- Unittests for rmdir -----------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/sys/stat/mkdir.h"
#include "src/unistd/rmdir.h"
#include "test/UnitTest/ErrnoCheckingTest.h"
#include "test/UnitTest/ErrnoSetterMatcher.h"
#include "test/UnitTest/Test.h"

#include "hdr/fcntl_macros.h"

using LlvmLibcRmdirTest = LIBC_NAMESPACE::testing::ErrnoCheckingTest;

TEST_F(LlvmLibcRmdirTest, CreateAndRemove) {
  using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Succeeds;
  constexpr const char *FILENAME = "rmdir.testdir";
  auto TEST_DIR = libc_make_test_file_path(FILENAME);
  ASSERT_THAT(LIBC_NAMESPACE::mkdir(TEST_DIR, S_IRWXU), Succeeds(0));
  ASSERT_THAT(LIBC_NAMESPACE::rmdir(TEST_DIR), Succeeds(0));
}

TEST_F(LlvmLibcRmdirTest, RemoveNonExistentDir) {
  using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Fails;
  ASSERT_THAT(LIBC_NAMESPACE::rmdir("non-existent-dir"), Fails(ENOENT));
}
