/* Copyright (c) 2015, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA */

#include <gtest/gtest.h>
#include <fstream>
#include <iostream>

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);

  const char *generate_option = "--generate_test_groups=";
  if (argc > 1 && strncmp(argv[1], generate_option, strlen(generate_option)) == 0)
  {
    const char *path = strchr(argv[1], '=')+1;
    std::ofstream f(path);

    std::cout << "Updating "<<path<<"...\n";
    f << "# Automatically generated, use make testgroups to update\n";

    ::testing::UnitTest *ut = ::testing::UnitTest::GetInstance();
    for (int i = 0; i < ut->total_test_case_count(); i++)
    {
       const char *name = ut->GetTestCase(i)->name();
       f << "add_test(" << name << " run_unit_tests --gtest_filter=" << name << ".*)\n";
    }
    return 0;
  }

  return RUN_ALL_TESTS();
}
