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

// For forkserver
#include <string>
#include <vector>
#include <iostream>
#include <string_view>
#include <utility>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <future>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <tuple>
#include <sstream>


/* Implement the top-level "main" within the driver in terms of
   driver::main (implemented in gcc.cc).  */

extern int main (int, char **);

int gcc_main(int argc, char**argv);
void start_forkserver(int argc, char**argv);
std::vector<std::string> copy_argv(int argc, char **argv);
std::vector<std::vector<std::string>> init(const std::vector<std::string>& argv_template, const std::string& src_path);
std::vector<std::string> modify_argv_for_optimization(const std::vector<std::string>& original_argv, const std::string& opt, const std::string& src_path);
int fork_gcc(const std::vector<std::string> &argv_set);
bool wait_for_child_exit(pid_t child_pid, std::string_view opt_level, std::stringstream& ss);
void kill_child_wait(pid_t child_pid, std::string_view opt_level, std::stringstream& ss);
std::stringstream wait_child_thread(pid_t child_pid, std::string_view opt_level);
std::string wait_child();
bool fork_handshake();
void flush_stdcout(std::string_view);
void make_result(std::stringstream& ss, std::string_view opt_level, int status);
void send_json(std::string result, std::string binary_base);
void exit_compiler(int ret, std::string_view);

namespace fork_server {
  std::vector<std::string> opt_levels {"-O0", "-O1", "-O2", "-O3"};
  std::vector<std::tuple<pid_t, std::string>> children; //pid, opt_level
  int compile_timeout_sec = 10;
  const int time_out_ms = 50; // 0.05초
  const char* fork_client_hello_msg = "fork client hello\n";
  const char* fork_server_hello_msg = "fork server hello";
  const char* fork_handshake_done_msg = "done\n";
  const char* exit_msg = "exit\n";
  std::string time_out_set_msg = "time_out_set";
  const std::string compiler_string = "gcc_";
  const char* bob_argv = "bob.c";

}

namespace string_helper {
std::string extract_left_of_delimiter(const std::string& src, char delimiter);
std::string extract_right_of_delimiter(const std::string& src, char delimiter);
std::string extract_prefix_up_to_last_slash(const std::string& src);

std::string extract_left_of_delimiter(const std::string& src, char delimiter) {
    size_t pos = src.find(delimiter);
    return (pos != std::string::npos) ? src.substr(0, pos) : "";
}

std::string extract_right_of_delimiter(const std::string& src, char delimiter) {
    size_t pos = src.find(delimiter);
    return (pos != std::string::npos) ? src.substr(pos + 1) : src;
}

std::string extract_prefix_up_to_last_slash(const std::string& src) {
    size_t last_slash = src.rfind('/');
    return (last_slash == std::string::npos) ? "" : src.substr(0, last_slash + 1);
}
}

int gcc_main(int argc, char** argv) {
    driver d (false, /* can_finalize */
	    false); /* debug */
  return d.main (argc, argv);
}


int fork_gcc(const std::vector<std::string> &argv_set) {
  pid_t pid = fork();
  if (pid == 0) {   // 자식 프로세스
    std::vector<char*> argv_pointers;
    for (const auto& arg : argv_set) {
      argv_pointers.push_back(const_cast<char*>(arg.c_str()));
    }
    argv_pointers.push_back(nullptr);
    auto ret = gcc_main(argv_pointers.size() - 1, argv_pointers.data());
    exit(ret);  // 자식 프로세스 종료
  } else if (pid > 0) { // parent
    std::string opt_level = argv_set.back();
    opt_level.erase(0, 1); // -O3 -> O3
    fork_server::children.push_back({pid, opt_level});
    return 0;
  } else {
    exit_compiler(1, "fork error");// 시스템콜 에러 이므로 컴파일러 종료
    exit(1);
  }
}

void make_result(std::stringstream& ss, std::string_view opt_level, int status) {
  ss << "        \"" << opt_level << "\": \""<< status << "\",";
}

bool wait_for_child_exit(pid_t child_pid, std::string_view opt_level, std::stringstream& ss) {
    int status;
    auto wpid = waitpid(child_pid, &status, WNOHANG);
    if (wpid == -1) {
        exit_compiler(1, "waitpid error");// 시스템콜 에러 이므로 컴파일러 종료
    } else if (wpid == child_pid) {
        make_result(ss, opt_level, status);
        // 정상 종료된거 처리
        return true;  // 자식 프로세스가 종료됨
    }
    //std::this_thread::sleep_for(std::chrono::milliseconds(fork_server::time_out_ms));
    return false;  // 자식 프로세스가 종료되지 않음
}

void kill_child_wait(pid_t child_pid, std::string_view opt_level, std::stringstream& ss) {
    auto ret = kill(child_pid, SIGALRM);
    if(ret == -1) {
        exit_compiler(1, "kill");// 시스템콜 에러 이므로 컴파일러 종료
        exit(1);
    }
    // Wait for child process to terminate after sending SIGALRM
    while (!wait_for_child_exit(child_pid, opt_level, ss));
}

std::stringstream wait_child_thread(pid_t child_pid, std::string_view opt_level) {
    std::stringstream result_stream;

    time_t start_time = time(nullptr);
    
    while (time(nullptr) - start_time < fork_server::compile_timeout_sec) {
        if (wait_for_child_exit(child_pid, opt_level, result_stream)) {  // 0.05초마다 확인
            return result_stream;  // 자식 프로세스가 종료됨
        }
    }
    // timeout 후에도 child process가 종료되지 않은 경우 처리
    kill_child_wait(child_pid, opt_level, result_stream);
    return result_stream;
}


std::string wait_child() {
  std::vector<std::future<std::stringstream>> futures;

  for (const auto& [child_pid, opt_level] : fork_server::children) {
        futures.push_back(std::async(std::launch::async, wait_child_thread, child_pid, opt_level));
  }

  std::string result_str;
  for (auto& future : futures) {
      auto json = future.get();  // 비동기 작업의 결과를 가져옴
      // childExited를 사용하여 필요한 처리 수행...
      result_str.append(json.str());
      //std::cout << json.str();
  }
  fork_server::children.clear();
  
  return result_str;


}

void flush_stdcout(std::string_view msg) {
  std::cout << msg;
  std::cout.flush();
}

bool fork_handshake() {
    // Write Client Hello
    std::string tmp_msg;
    flush_stdcout(fork_server::fork_client_hello_msg);

    // Read Server Hello
    std::getline(std::cin, tmp_msg);
    if(strcmp(tmp_msg.c_str(), fork_server::fork_server_hello_msg) != 0) {
      exit_compiler(1, "forkserver hello failed");
      return false;
    }
    // Send Done
    flush_stdcout(fork_server::fork_handshake_done_msg);

    // Read & set compile time out
    std::getline(std::cin, tmp_msg);

    // Possible Exception!
    fork_server::compile_timeout_sec = std::stoi(tmp_msg);

    flush_stdcout(fork_server::time_out_set_msg + " " + std::to_string(fork_server::compile_timeout_sec) + "\n");
    return true;
}

void send_json(std::string result, std::string binary_base) {
  std::stringstream json_stream;
  json_stream << "    \"" << "binary_base" << "\": \"" << binary_base << "\",";
  json_stream << "    \"" << "result" << "\": {";
  json_stream << result;
  json_stream << "    }\n";
  std::cout << json_stream.str();
  std::cout.flush();
}

void exit_compiler(int ret, std::string_view msg) {
  std::cout << "\"exit_code\" : \""<< ret <<"\", \"error_message\" : \"" << msg << "\"\n";
  std::cout.flush();
  exit(ret);
}

void start_forkserver(int argc, char**argv) {
    std::vector<std::string> argv_template;
    argv_template = copy_argv(argc, argv);
    // 여기까지는 딱 한번
    auto handshake_ret = fork_handshake();
    if(!handshake_ret) {
      exit_compiler(1, "handshake failed");// handshake 에러 이므로 컴파일러 종료
    }
    while(1) {
      // get source code file!
      std::string command;
      std::getline(std::cin, command);
      if(command == "exit") {
        exit_compiler(0, "normal exit");
      }

      auto optimized_argv_sets = init(argv_template, command);

      for (const auto& argv_set : optimized_argv_sets) {
        auto fork_ret = fork_gcc(argv_set);
        if(fork_ret) {
          exit_compiler(1, "fork error");// 시스템콜 에러 이므로 컴파일러 종료
        }
      }

      // wait the compile & result
      auto result_str = std::move(wait_child());
      // make binary_base
      std::string prefix = string_helper::extract_prefix_up_to_last_slash(string_helper::extract_right_of_delimiter(command, '|'));
      auto binary_base = std::move(prefix + fork_server::compiler_string);
      send_json(result_str, binary_base);
    }
}

std::vector<std::string> copy_argv(int argc, char **argv) {
    std::vector<std::string> copy(argc);
    for (int i = 0; i < argc; i++) {
        copy[i] = argv[i];
    }
    return copy;
}

std::vector<std::vector<std::string>> init(const std::vector<std::string>& argv_template, const std::string& src_path) {
  std::vector<std::vector<std::string>> optimized_argv_sets(4);

  for (int i = 0; i < 4; i++) {
    optimized_argv_sets[i] = modify_argv_for_optimization(argv_template, fork_server::opt_levels[i], src_path);
  }

  return optimized_argv_sets;
}

std::vector<std::string> modify_argv_for_optimization(const std::vector<std::string>& original_argv, 
                                                      const std::string& opt, 
                                                      const std::string& src_path) {
    std::vector<std::string> new_argv;
    
    new_argv.push_back(original_argv[0]);
    
    std::string left_src = string_helper::extract_left_of_delimiter(src_path, '|');
    if(!left_src.empty()) {
        new_argv.push_back(left_src);
    }
    
    std::string l_src_path = string_helper::extract_right_of_delimiter(src_path, '|');
    new_argv.push_back(l_src_path);
    new_argv.push_back("-o");

    std::string prefix = string_helper::extract_prefix_up_to_last_slash(l_src_path);
    new_argv.push_back(prefix + fork_server::compiler_string + opt.substr(1));
    
    new_argv.push_back(opt);

    return new_argv;
}



int
main (int argc, char **argv)
{
  if(argc > 1 && strcmp(argv[1], fork_server::bob_argv) == 0) {
    // start forkserver
    start_forkserver(argc, argv);
  } else {
    return gcc_main(argc, argv);
  }
}

