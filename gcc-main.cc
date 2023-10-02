/* "main" for the compiler driver.
   Copyright (C) 1987-2023 Free Software Foundation, Inc.

This file is part of GCC.

GCC is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free
Software Foundation; either version 3, or (at your option) any later
version.

GCC is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or
FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
for more details.

You should have received a copy of the GNU General Public License
along with GCC; see the file COPYING3.  If not see
<http://www.gnu.org/licenses/>.  */

/* This source file contains "main" for the compiler driver.
   All of the real work is done within gcc.cc; we implement "main"
   in here for the "gcc" binary so that gcc.o can be used in
   libgccjit.so.  */

#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "tm.h"
#include "xregex.h"
#include "obstack.h"
#include "intl.h"
#include "prefix.h"
#include "opt-suggestions.h"
#include "gcc.h"

/* Implement the top-level "main" within the driver in terms of
   driver::main (implemented in gcc.cc).  */

#include <iostream>
#include <vector>
#include <cstring>
#include <sstream>

extern int main (int, char **);
std::vector<std::string> modify_argv_for_optimization(const std::vector<std::string>&, const std::string&, const std::string&);
std::vector<std::vector<std::string>> init(const std::vector<std::string>&, const std::string&);
std::vector<std::string> copy_argv(int argc, char **argv);
void fork_hand_shake();

int gcc_main(int argc, char** argv) {
  driver d (false, /* can_finalize */
	    false); /* debug */
  return d.main (argc, argv);
}
std::vector<std::string> copy_argv(int argc, char **argv) {
    std::vector<std::string> copy(argc);
    for (int i = 0; i < argc; i++) {
        copy[i] = argv[i];
    }
    return copy;
}

std::vector<std::vector<std::string>> init(const std::vector<std::string>& argv_template, const std::string& src_path) {
  const char* opt_levels[] = {"-O0", "-O1", "-O2", "-O3"};
  std::vector<std::vector<std::string>> optimized_argv_sets(4);

  for (int i = 0; i < 4; i++) {
    optimized_argv_sets[i] = modify_argv_for_optimization(argv_template, opt_levels[i], src_path);
  }

  return optimized_argv_sets;
}
std::vector<std::string> modify_argv_for_optimization(const std::vector<std::string>& original_argv, const std::string& opt, const std::string& src_path) {
  std::vector<std::string> new_argv;
  new_argv.push_back(original_argv[0]);
  new_argv.push_back(src_path);
  new_argv.push_back("-o");

  // make binary path 
  size_t last_slash = src_path.rfind('/');
  if (last_slash == std::string::npos) {
    new_argv.push_back("gcc_" + opt.substr(1));
  } else {
    new_argv.push_back(src_path.substr(0, last_slash + 1) + "gcc_" + opt.substr(1));
  }

  new_argv.push_back(opt);

  return new_argv;
}
void fork_hand_shake() {
    std::cout << "fork client hello\n";
    std::cout.flush();
    char server_hello[19] = {0}; // Initialize with zeros
    read(0, server_hello, 19);   // Read one byte less to account for the NULL terminator
    server_hello[18] = '\0';     // Null-terminate the string
    
    if(strcmp(server_hello, "fork server hello\n") != 0) {
        exit(1);
    }
    std::cout << "done\n";
    std::cout.flush();
}

int
main (int argc, char **argv)
{
   std::vector<std::string> argv_template;  
   if(argc > 1 && strcmp(argv[1], "bob.c") == 0) {
      int status;
      fork_hand_shake();
      // O0, O1, O2, O3 
      // ./gcc-trunk
      // bob.c

      // ./gcc-trunk hello.c -o gcc_O0 -O0
      argv_template = copy_argv(argc, argv);
      while(true) {
         std::string src_path;
         std::getline(std::cin, src_path);

         if (src_path == "exit") {
           printf("compile exit!\n");
           exit(0);
         }
         // O0 ~ O3
         auto optimized_argv_sets = init(argv_template, src_path);
         std::vector<pid_t> children;
         for (const auto& argv_set : optimized_argv_sets) {
        pid_t pid = fork();
        if (pid == 0) {   // 자식 프로세스
          std::vector<char*> argv_pointers;
          for (const auto& arg : argv_set) {
            argv_pointers.push_back(const_cast<char*>(arg.c_str()));
          }
          argv_pointers.push_back(nullptr);
          auto ret = gcc_main(argv_pointers.size() - 1, argv_pointers.data());
          exit(ret);  // 자식 프로세스 종료
        }
        else if (pid > 0) {
          children.push_back(pid);
        }
        else {
          perror("fork error!!!\n");
          exit(1);
        }
      }
      // 모든 자식 프로세스가 완료될 때까지 대기
      std::stringstream ss;
      ss << "{";
      int i = 0;
      for (pid_t child_pid : children) {
        auto wpid = waitpid(child_pid, &status, WUNTRACED | WCONTINUED);
        if (wpid == -1) {
          perror("waitpid");
          exit(EXIT_FAILURE);
        }
        ss << "    \"" << "O" + std::to_string(i) << "\": \""<< status << "\",";
         i++;
      }
      ss << "}\n";
      size_t last_slash = src_path.rfind('/');

      if (last_slash != std::string::npos) {
         src_path.replace(last_slash + 1, src_path.length() - last_slash - 1, "gcc_");
      } else {
          std::cout << "'/' not found in the string." << std::endl;
      }

      std::cout << src_path << "|" << ss.str();
   }
   } else {
      return gcc_main(argc, argv);
   }
}
