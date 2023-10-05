//===-- driver.cpp - Clang GCC-Compatible Driver --------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This is the entry point to the clang driver; it is a thin wrapper
// for functionality in the Driver clang library.
//
//===----------------------------------------------------------------------===//

#include "clang/Driver/Driver.h"
#include "clang/Basic/DiagnosticOptions.h"
#include "clang/Basic/HeaderInclude.h"
#include "clang/Basic/Stack.h"
#include "clang/Config/config.h"
#include "clang/Driver/Compilation.h"
#include "clang/Driver/DriverDiagnostic.h"
#include "clang/Driver/Options.h"
#include "clang/Driver/ToolChain.h"
#include "clang/Frontend/ChainedDiagnosticConsumer.h"
#include "clang/Frontend/CompilerInvocation.h"
#include "clang/Frontend/SerializedDiagnosticPrinter.h"
#include "clang/Frontend/TextDiagnosticPrinter.h"
#include "clang/Frontend/Utils.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Option/OptTable.h"
#include "llvm/Option/Option.h"
#include "llvm/Support/BuryPointer.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/CrashRecoveryContext.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/LLVMDriver.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/Regex.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/StringSaver.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/Timer.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Host.h"
#include <memory>
#include <optional>
#include <set>
#include <system_error>
using namespace clang;
using namespace clang::driver;
using namespace llvm::opt;

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

int real_clang_main(int Argc, char **Argv, const llvm::ToolContext &ToolContext);

void start_forkserver(int argc, char**argv, const llvm::ToolContext &ToolContext);
std::vector<std::string> copy_argv(int argc, char **argv);
std::vector<std::vector<std::string>> init(const std::vector<std::string>& argv_template, const std::string& src_path);
std::vector<std::string> modify_argv_for_optimization(const std::vector<std::string>& original_argv, const std::string& opt, const std::string& src_path);
int fork_clang(const std::vector<std::string> &argv_set, const llvm::ToolContext &ToolContext);
bool wait_for_child_exit(pid_t child_pid, std::string_view opt_level, std::stringstream& ss);
void kill_child_wait(pid_t child_pid, std::string_view opt_level, std::stringstream& ss);
std::stringstream wait_child_thread(pid_t child_pid, std::string_view opt_level);
std::string wait_child();
bool fork_handshake();
void flush_stdcout(std::string_view);
void make_result(std::stringstream& ss, std::string_view opt_level, int status);
void send_json(std::string result, std::string binary_base);
void exit_compiler(int ret, std::string_view msg);

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
  const std::string compiler_string = "clang_";
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

void exit_compiler(int ret, std::string_view msg) {
  std::cout << "{ \"exit_code\" : \""<< ret <<"\", \"error_message\" : \"" << msg << "\" }\n";
  std::cout.flush();
  exit(ret);
}

int fork_clang(const std::vector<std::string> &argv_set, const llvm::ToolContext &ToolContext) {
  pid_t pid = fork();
  if (pid == 0) {   // 자식 프로세스
    std::vector<char*> argv_pointers;
    for (const auto& arg : argv_set) {
      argv_pointers.push_back(const_cast<char*>(arg.c_str()));
    }
    argv_pointers.push_back(nullptr);
    auto ret = real_clang_main(argv_pointers.size() - 1, argv_pointers.data(), ToolContext);
    exit(ret);  // 자식 프로세스 종료
  } else if (pid > 0) { // parent
    std::string opt_level = argv_set.back();
    opt_level.erase(0, 1); // -O3 -> O3
    fork_server::children.push_back({pid, opt_level});
    return 0;
  } else {
    exit_compiler(1, "fork error");
  }
}

void make_result(std::stringstream& ss, std::string_view opt_level, int status) {
  ss << "        \"" << opt_level << "\": \""<< status << "\",";
}

bool wait_for_child_exit(pid_t child_pid, std::string_view opt_level, std::stringstream& ss) {
    int status;
    auto wpid = waitpid(child_pid, &status, WNOHANG);
    if (wpid == -1) {
        exit_compiler(1, "waitpid");
    } else if (wpid == child_pid) {
        make_result(ss, opt_level, status);
        // 정상 종료된거 처리
        return true;  // 자식 프로세스가 종료됨
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(fork_server::time_out_ms));
    return false;  // 자식 프로세스가 종료되지 않음
}

void kill_child_wait(pid_t child_pid, std::string_view opt_level, std::stringstream& ss) {
    auto ret = kill(child_pid, SIGALRM);
    if(ret == -1) {
        exit_compiler(1, "kill");
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
  json_stream << "{";
  json_stream << "    \"" << "binary_base" << "\": \"" << binary_base << "\",";
  json_stream << "    \"" << "result" << "\": {";
  json_stream << result;
  json_stream << "    }";
  json_stream << "}\n";
  std::cout << json_stream.str();
  std::cout.flush();
}

void start_forkserver(int argc, char**argv, const llvm::ToolContext &ToolContext) {
    std::vector<std::string> argv_template;
    argv_template = copy_argv(argc, argv);
    // 여기까지는 딱 한번
    auto handshake_ret = fork_handshake();
    if(!handshake_ret) {
      exit_compiler(1, "handshake failed");
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
        auto fork_ret = fork_clang(argv_set, ToolContext);
        if(fork_ret) {
          printf("fork error\n");
          exit(1);
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

std::string GetExecutablePath(const char *Argv0, bool CanonicalPrefixes) {
  if (!CanonicalPrefixes) {
    SmallString<128> ExecutablePath(Argv0);
    // Do a PATH lookup if Argv0 isn't a valid path.
    if (!llvm::sys::fs::exists(ExecutablePath))
      if (llvm::ErrorOr<std::string> P =
              llvm::sys::findProgramByName(ExecutablePath))
        ExecutablePath = *P;
    return std::string(ExecutablePath.str());
  }

  // This just needs to be some symbol in the binary; C++ doesn't
  // allow taking the address of ::main however.
  void *P = (void*) (intptr_t) GetExecutablePath;
  return llvm::sys::fs::getMainExecutable(Argv0, P);
}

static const char *GetStableCStr(std::set<std::string> &SavedStrings,
                                 StringRef S) {
  return SavedStrings.insert(std::string(S)).first->c_str();
}

/// ApplyQAOverride - Apply a list of edits to the input argument lists.
///
/// The input string is a space separate list of edits to perform,
/// they are applied in order to the input argument lists. Edits
/// should be one of the following forms:
///
///  '#': Silence information about the changes to the command line arguments.
///
///  '^': Add FOO as a new argument at the beginning of the command line.
///
///  '+': Add FOO as a new argument at the end of the command line.
///
///  's/XXX/YYY/': Substitute the regular expression XXX with YYY in the command
///  line.
///
///  'xOPTION': Removes all instances of the literal argument OPTION.
///
///  'XOPTION': Removes all instances of the literal argument OPTION,
///  and the following argument.
///
///  'Ox': Removes all flags matching 'O' or 'O[sz0-9]' and adds 'Ox'
///  at the end of the command line.
///
/// \param OS - The stream to write edit information to.
/// \param Args - The vector of command line arguments.
/// \param Edit - The override command to perform.
/// \param SavedStrings - Set to use for storing string representations.
static void ApplyOneQAOverride(raw_ostream &OS,
                               SmallVectorImpl<const char*> &Args,
                               StringRef Edit,
                               std::set<std::string> &SavedStrings) {
  // This does not need to be efficient.

  if (Edit[0] == '^') {
    const char *Str =
      GetStableCStr(SavedStrings, Edit.substr(1));
    OS << "### Adding argument " << Str << " at beginning\n";
    Args.insert(Args.begin() + 1, Str);
  } else if (Edit[0] == '+') {
    const char *Str =
      GetStableCStr(SavedStrings, Edit.substr(1));
    OS << "### Adding argument " << Str << " at end\n";
    Args.push_back(Str);
  } else if (Edit[0] == 's' && Edit[1] == '/' && Edit.endswith("/") &&
             Edit.slice(2, Edit.size() - 1).contains('/')) {
    StringRef MatchPattern = Edit.substr(2).split('/').first;
    StringRef ReplPattern = Edit.substr(2).split('/').second;
    ReplPattern = ReplPattern.slice(0, ReplPattern.size()-1);

    for (unsigned i = 1, e = Args.size(); i != e; ++i) {
      // Ignore end-of-line response file markers
      if (Args[i] == nullptr)
        continue;
      std::string Repl = llvm::Regex(MatchPattern).sub(ReplPattern, Args[i]);

      if (Repl != Args[i]) {
        OS << "### Replacing '" << Args[i] << "' with '" << Repl << "'\n";
        Args[i] = GetStableCStr(SavedStrings, Repl);
      }
    }
  } else if (Edit[0] == 'x' || Edit[0] == 'X') {
    auto Option = Edit.substr(1);
    for (unsigned i = 1; i < Args.size();) {
      if (Option == Args[i]) {
        OS << "### Deleting argument " << Args[i] << '\n';
        Args.erase(Args.begin() + i);
        if (Edit[0] == 'X') {
          if (i < Args.size()) {
            OS << "### Deleting argument " << Args[i] << '\n';
            Args.erase(Args.begin() + i);
          } else
            OS << "### Invalid X edit, end of command line!\n";
        }
      } else
        ++i;
    }
  } else if (Edit[0] == 'O') {
    for (unsigned i = 1; i < Args.size();) {
      const char *A = Args[i];
      // Ignore end-of-line response file markers
      if (A == nullptr)
        continue;
      if (A[0] == '-' && A[1] == 'O' &&
          (A[2] == '\0' ||
           (A[3] == '\0' && (A[2] == 's' || A[2] == 'z' ||
                             ('0' <= A[2] && A[2] <= '9'))))) {
        OS << "### Deleting argument " << Args[i] << '\n';
        Args.erase(Args.begin() + i);
      } else
        ++i;
    }
    OS << "### Adding argument " << Edit << " at end\n";
    Args.push_back(GetStableCStr(SavedStrings, '-' + Edit.str()));
  } else {
    OS << "### Unrecognized edit: " << Edit << "\n";
  }
}

/// ApplyQAOverride - Apply a comma separate list of edits to the
/// input argument lists. See ApplyOneQAOverride.
static void ApplyQAOverride(SmallVectorImpl<const char*> &Args,
                            const char *OverrideStr,
                            std::set<std::string> &SavedStrings) {
  raw_ostream *OS = &llvm::errs();

  if (OverrideStr[0] == '#') {
    ++OverrideStr;
    OS = &llvm::nulls();
  }

  *OS << "### CCC_OVERRIDE_OPTIONS: " << OverrideStr << "\n";

  // This does not need to be efficient.

  const char *S = OverrideStr;
  while (*S) {
    const char *End = ::strchr(S, ' ');
    if (!End)
      End = S + strlen(S);
    if (End != S)
      ApplyOneQAOverride(*OS, Args, std::string(S, End), SavedStrings);
    S = End;
    if (*S != '\0')
      ++S;
  }
}

extern int cc1_main(ArrayRef<const char *> Argv, const char *Argv0,
                    void *MainAddr);
extern int cc1as_main(ArrayRef<const char *> Argv, const char *Argv0,
                      void *MainAddr);
extern int cc1gen_reproducer_main(ArrayRef<const char *> Argv,
                                  const char *Argv0, void *MainAddr,
                                  const llvm::ToolContext &);

static void insertTargetAndModeArgs(const ParsedClangName &NameParts,
                                    SmallVectorImpl<const char *> &ArgVector,
                                    std::set<std::string> &SavedStrings) {
  // Put target and mode arguments at the start of argument list so that
  // arguments specified in command line could override them. Avoid putting
  // them at index 0, as an option like '-cc1' must remain the first.
  int InsertionPoint = 0;
  if (ArgVector.size() > 0)
    ++InsertionPoint;

  if (NameParts.DriverMode) {
    // Add the mode flag to the arguments.
    ArgVector.insert(ArgVector.begin() + InsertionPoint,
                     GetStableCStr(SavedStrings, NameParts.DriverMode));
  }

  if (NameParts.TargetIsValid) {
    const char *arr[] = {"-target", GetStableCStr(SavedStrings,
                                                  NameParts.TargetPrefix)};
    ArgVector.insert(ArgVector.begin() + InsertionPoint,
                     std::begin(arr), std::end(arr));
  }
}

static void getCLEnvVarOptions(std::string &EnvValue, llvm::StringSaver &Saver,
                               SmallVectorImpl<const char *> &Opts) {
  llvm::cl::TokenizeWindowsCommandLine(EnvValue, Saver, Opts);
  // The first instance of '#' should be replaced with '=' in each option.
  for (const char *Opt : Opts)
    if (char *NumberSignPtr = const_cast<char *>(::strchr(Opt, '#')))
      *NumberSignPtr = '=';
}

template <class T>
static T checkEnvVar(const char *EnvOptSet, const char *EnvOptFile,
                     std::string &OptFile) {
  const char *Str = ::getenv(EnvOptSet);
  if (!Str)
    return T{};

  T OptVal = Str;
  if (const char *Var = ::getenv(EnvOptFile))
    OptFile = Var;
  return OptVal;
}

static bool SetBackdoorDriverOutputsFromEnvVars(Driver &TheDriver) {
  TheDriver.CCPrintOptions =
      checkEnvVar<bool>("CC_PRINT_OPTIONS", "CC_PRINT_OPTIONS_FILE",
                        TheDriver.CCPrintOptionsFilename);
  if (checkEnvVar<bool>("CC_PRINT_HEADERS", "CC_PRINT_HEADERS_FILE",
                        TheDriver.CCPrintHeadersFilename)) {
    TheDriver.CCPrintHeadersFormat = HIFMT_Textual;
    TheDriver.CCPrintHeadersFiltering = HIFIL_None;
  } else {
    std::string EnvVar = checkEnvVar<std::string>(
        "CC_PRINT_HEADERS_FORMAT", "CC_PRINT_HEADERS_FILE",
        TheDriver.CCPrintHeadersFilename);
    if (!EnvVar.empty()) {
      TheDriver.CCPrintHeadersFormat =
          stringToHeaderIncludeFormatKind(EnvVar.c_str());
      if (!TheDriver.CCPrintHeadersFormat) {
        TheDriver.Diag(clang::diag::err_drv_print_header_env_var)
            << 0 << EnvVar;
        return false;
      }

      const char *FilteringStr = ::getenv("CC_PRINT_HEADERS_FILTERING");
      HeaderIncludeFilteringKind Filtering;
      if (!stringToHeaderIncludeFiltering(FilteringStr, Filtering)) {
        TheDriver.Diag(clang::diag::err_drv_print_header_env_var)
            << 1 << FilteringStr;
        return false;
      }

      if ((TheDriver.CCPrintHeadersFormat == HIFMT_Textual &&
           Filtering != HIFIL_None) ||
          (TheDriver.CCPrintHeadersFormat == HIFMT_JSON &&
           Filtering != HIFIL_Only_Direct_System)) {
        TheDriver.Diag(clang::diag::err_drv_print_header_env_var_combination)
            << EnvVar << FilteringStr;
        return false;
      }
      TheDriver.CCPrintHeadersFiltering = Filtering;
    }
  }

  TheDriver.CCLogDiagnostics =
      checkEnvVar<bool>("CC_LOG_DIAGNOSTICS", "CC_LOG_DIAGNOSTICS_FILE",
                        TheDriver.CCLogDiagnosticsFilename);
  TheDriver.CCPrintProcessStats =
      checkEnvVar<bool>("CC_PRINT_PROC_STAT", "CC_PRINT_PROC_STAT_FILE",
                        TheDriver.CCPrintStatReportFilename);
  TheDriver.CCPrintInternalStats =
      checkEnvVar<bool>("CC_PRINT_INTERNAL_STAT", "CC_PRINT_INTERNAL_STAT_FILE",
                        TheDriver.CCPrintInternalStatReportFilename);

  return true;
}

static void FixupDiagPrefixExeName(TextDiagnosticPrinter *DiagClient,
                                   const std::string &Path) {
  // If the clang binary happens to be named cl.exe for compatibility reasons,
  // use clang-cl.exe as the prefix to avoid confusion between clang and MSVC.
  StringRef ExeBasename(llvm::sys::path::stem(Path));
  if (ExeBasename.equals_insensitive("cl"))
    ExeBasename = "clang-cl";
  DiagClient->setPrefix(std::string(ExeBasename));
}

static void SetInstallDir(SmallVectorImpl<const char *> &argv,
                          Driver &TheDriver, bool CanonicalPrefixes) {
  // Attempt to find the original path used to invoke the driver, to determine
  // the installed path. We do this manually, because we want to support that
  // path being a symlink.
  SmallString<128> InstalledPath(argv[0]);

  // Do a PATH lookup, if there are no directory components.
  if (llvm::sys::path::filename(InstalledPath) == InstalledPath)
    if (llvm::ErrorOr<std::string> Tmp = llvm::sys::findProgramByName(
            llvm::sys::path::filename(InstalledPath.str())))
      InstalledPath = *Tmp;

  // FIXME: We don't actually canonicalize this, we just make it absolute.
  if (CanonicalPrefixes)
    llvm::sys::fs::make_absolute(InstalledPath);

  StringRef InstalledPathParent(llvm::sys::path::parent_path(InstalledPath));
  if (llvm::sys::fs::exists(InstalledPathParent))
    TheDriver.setInstalledDir(InstalledPathParent);
}

static int ExecuteCC1Tool(SmallVectorImpl<const char *> &ArgV,
                          const llvm::ToolContext &ToolContext) {
  // If we call the cc1 tool from the clangDriver library (through
  // Driver::CC1Main), we need to clean up the options usage count. The options
  // are currently global, and they might have been used previously by the
  // driver.
  llvm::cl::ResetAllOptionOccurrences();

  llvm::BumpPtrAllocator A;
  llvm::cl::ExpansionContext ECtx(A, llvm::cl::TokenizeGNUCommandLine);
  if (llvm::Error Err = ECtx.expandResponseFiles(ArgV)) {
    llvm::errs() << toString(std::move(Err)) << '\n';
    return 1;
  }
  StringRef Tool = ArgV[1];
  void *GetExecutablePathVP = (void *)(intptr_t)GetExecutablePath;
  if (Tool == "-cc1")
    return cc1_main(ArrayRef(ArgV).slice(1), ArgV[0], GetExecutablePathVP);
  if (Tool == "-cc1as")
    return cc1as_main(ArrayRef(ArgV).slice(2), ArgV[0], GetExecutablePathVP);
  if (Tool == "-cc1gen-reproducer")
    return cc1gen_reproducer_main(ArrayRef(ArgV).slice(2), ArgV[0],
                                  GetExecutablePathVP, ToolContext);
  // Reject unknown tools.
  llvm::errs() << "error: unknown integrated tool '" << Tool << "'. "
               << "Valid tools include '-cc1' and '-cc1as'.\n";
  return 1;
}



int real_clang_main(int Argc, char **Argv, const llvm::ToolContext &ToolContext) {
    noteBottomOfStack();
  llvm::InitLLVM X(Argc, Argv);
  llvm::setBugReportMsg("PLEASE submit a bug report to " BUG_REPORT_URL
                        " and include the crash backtrace, preprocessed "
                        "source, and associated run script.\n");
  SmallVector<const char *, 256> Args(Argv, Argv + Argc);

  if (llvm::sys::Process::FixupStandardFileDescriptors())
    return 1;

  llvm::InitializeAllTargets();

  llvm::BumpPtrAllocator A;
  llvm::StringSaver Saver(A);

  const char *ProgName =
      ToolContext.NeedsPrependArg ? ToolContext.PrependArg : ToolContext.Path;

  bool ClangCLMode =
      IsClangCL(getDriverMode(ProgName, llvm::ArrayRef(Args).slice(1)));

  if (llvm::Error Err = expandResponseFiles(Args, ClangCLMode, A)) {
    llvm::errs() << toString(std::move(Err)) << '\n';
    return 1;
  }

  // Handle -cc1 integrated tools.
  if (Args.size() >= 2 && StringRef(Args[1]).startswith("-cc1"))
    return ExecuteCC1Tool(Args, ToolContext);

  // Handle options that need handling before the real command line parsing in
  // Driver::BuildCompilation()
  bool CanonicalPrefixes = true;
  for (int i = 1, size = Args.size(); i < size; ++i) {
    // Skip end-of-line response file markers
    if (Args[i] == nullptr)
      continue;
    if (StringRef(Args[i]) == "-canonical-prefixes")
      CanonicalPrefixes = true;
    else if (StringRef(Args[i]) == "-no-canonical-prefixes")
      CanonicalPrefixes = false;
  }

  // Handle CL and _CL_ which permits additional command line options to be
  // prepended or appended.
  if (ClangCLMode) {
    // Arguments in "CL" are prepended.
    std::optional<std::string> OptCL = llvm::sys::Process::GetEnv("CL");
    if (OptCL) {
      SmallVector<const char *, 8> PrependedOpts;
      getCLEnvVarOptions(*OptCL, Saver, PrependedOpts);

      // Insert right after the program name to prepend to the argument list.
      Args.insert(Args.begin() + 1, PrependedOpts.begin(), PrependedOpts.end());
    }
    // Arguments in "_CL_" are appended.
    std::optional<std::string> Opt_CL_ = llvm::sys::Process::GetEnv("_CL_");
    if (Opt_CL_) {
      SmallVector<const char *, 8> AppendedOpts;
      getCLEnvVarOptions(*Opt_CL_, Saver, AppendedOpts);

      // Insert at the end of the argument list to append.
      Args.append(AppendedOpts.begin(), AppendedOpts.end());
    }
  }

  std::set<std::string> SavedStrings;
  // Handle CCC_OVERRIDE_OPTIONS, used for editing a command line behind the
  // scenes.
  if (const char *OverrideStr = ::getenv("CCC_OVERRIDE_OPTIONS")) {
    // FIXME: Driver shouldn't take extra initial argument.
    ApplyQAOverride(Args, OverrideStr, SavedStrings);
  }

  std::string Path = GetExecutablePath(ToolContext.Path, CanonicalPrefixes);

  // Whether the cc1 tool should be called inside the current process, or if we
  // should spawn a new clang subprocess (old behavior).
  // Not having an additional process saves some execution time of Windows,
  // and makes debugging and profiling easier.
  bool UseNewCC1Process = CLANG_SPAWN_CC1;
  for (const char *Arg : Args)
    UseNewCC1Process = llvm::StringSwitch<bool>(Arg)
                           .Case("-fno-integrated-cc1", true)
                           .Case("-fintegrated-cc1", false)
                           .Default(UseNewCC1Process);

  IntrusiveRefCntPtr<DiagnosticOptions> DiagOpts =
      CreateAndPopulateDiagOpts(Args);

  TextDiagnosticPrinter *DiagClient
    = new TextDiagnosticPrinter(llvm::errs(), &*DiagOpts);
  FixupDiagPrefixExeName(DiagClient, ProgName);

  IntrusiveRefCntPtr<DiagnosticIDs> DiagID(new DiagnosticIDs());

  DiagnosticsEngine Diags(DiagID, &*DiagOpts, DiagClient);

  if (!DiagOpts->DiagnosticSerializationFile.empty()) {
    auto SerializedConsumer =
        clang::serialized_diags::create(DiagOpts->DiagnosticSerializationFile,
                                        &*DiagOpts, /*MergeChildRecords=*/true);
    Diags.setClient(new ChainedDiagnosticConsumer(
        Diags.takeClient(), std::move(SerializedConsumer)));
  }

  ProcessWarningOptions(Diags, *DiagOpts, /*ReportDiags=*/false);

  Driver TheDriver(Path, llvm::sys::getDefaultTargetTriple(), Diags);
  SetInstallDir(Args, TheDriver, CanonicalPrefixes);
  auto TargetAndMode = ToolChain::getTargetAndModeFromProgramName(ProgName);
  TheDriver.setTargetAndMode(TargetAndMode);
  // If -canonical-prefixes is set, GetExecutablePath will have resolved Path
  // to the llvm driver binary, not clang. In this case, we need to use
  // PrependArg which should be clang-*. Checking just CanonicalPrefixes is
  // safe even in the normal case because PrependArg will be null so
  // setPrependArg will be a no-op.
  if (ToolContext.NeedsPrependArg || CanonicalPrefixes)
    TheDriver.setPrependArg(ToolContext.PrependArg);

  insertTargetAndModeArgs(TargetAndMode, Args, SavedStrings);

  if (!SetBackdoorDriverOutputsFromEnvVars(TheDriver))
    return 1;

  if (!UseNewCC1Process) {
    TheDriver.CC1Main = [ToolContext](SmallVectorImpl<const char *> &ArgV) {
      return ExecuteCC1Tool(ArgV, ToolContext);
    };
    // Ensure the CC1Command actually catches cc1 crashes
    llvm::CrashRecoveryContext::Enable();
  }

  std::unique_ptr<Compilation> C(TheDriver.BuildCompilation(Args));

  Driver::ReproLevel ReproLevel = Driver::ReproLevel::OnCrash;
  if (Arg *A = C->getArgs().getLastArg(options::OPT_gen_reproducer_eq)) {
    auto Level =
        llvm::StringSwitch<std::optional<Driver::ReproLevel>>(A->getValue())
            .Case("off", Driver::ReproLevel::Off)
            .Case("crash", Driver::ReproLevel::OnCrash)
            .Case("error", Driver::ReproLevel::OnError)
            .Case("always", Driver::ReproLevel::Always)
            .Default(std::nullopt);
    if (!Level) {
      llvm::errs() << "Unknown value for " << A->getSpelling() << ": '"
                   << A->getValue() << "'\n";
      return 1;
    }
    ReproLevel = *Level;
  }
  if (!!::getenv("FORCE_CLANG_DIAGNOSTICS_CRASH"))
    ReproLevel = Driver::ReproLevel::Always;

  int Res = 1;
  bool IsCrash = false;
  Driver::CommandStatus CommandStatus = Driver::CommandStatus::Ok;
  // Pretend the first command failed if ReproStatus is Always.
  const Command *FailingCommand = nullptr;
  if (!C->getJobs().empty())
    FailingCommand = &*C->getJobs().begin();
  if (C && !C->containsError()) {
    SmallVector<std::pair<int, const Command *>, 4> FailingCommands;
    Res = TheDriver.ExecuteCompilation(*C, FailingCommands);

    for (const auto &P : FailingCommands) {
      int CommandRes = P.first;
      FailingCommand = P.second;
      if (!Res)
        Res = CommandRes;

      // If result status is < 0, then the driver command signalled an error.
      // If result status is 70, then the driver command reported a fatal error.
      // On Windows, abort will return an exit code of 3.  In these cases,
      // generate additional diagnostic information if possible.
      IsCrash = CommandRes < 0 || CommandRes == 70;
#ifdef _WIN32
      IsCrash |= CommandRes == 3;
#endif
#if LLVM_ON_UNIX
      // When running in integrated-cc1 mode, the CrashRecoveryContext returns
      // the same codes as if the program crashed. See section "Exit Status for
      // Commands":
      // https://pubs.opengroup.org/onlinepubs/9699919799/xrat/V4_xcu_chap02.html
      IsCrash |= CommandRes > 128;
#endif
      CommandStatus =
          IsCrash ? Driver::CommandStatus::Crash : Driver::CommandStatus::Error;
      if (IsCrash)
        break;
    }
  }

  // Print the bug report message that would be printed if we did actually
  // crash, but only if we're crashing due to FORCE_CLANG_DIAGNOSTICS_CRASH.
  if (::getenv("FORCE_CLANG_DIAGNOSTICS_CRASH"))
    llvm::dbgs() << llvm::getBugReportMsg();
  if (FailingCommand != nullptr &&
    TheDriver.maybeGenerateCompilationDiagnostics(CommandStatus, ReproLevel,
                                                  *C, *FailingCommand))
    Res = 1;

  Diags.getClient()->finish();

  if (!UseNewCC1Process && IsCrash) {
    // When crashing in -fintegrated-cc1 mode, bury the timer pointers, because
    // the internal linked list might point to already released stack frames.
    llvm::BuryPointer(llvm::TimerGroup::aquireDefaultGroup());
  } else {
    // If any timers were active but haven't been destroyed yet, print their
    // results now.  This happens in -disable-free mode.
    llvm::TimerGroup::printAll(llvm::errs());
    llvm::TimerGroup::clearAll();
  }

#ifdef _WIN32
  // Exit status should not be negative on Win32, unless abnormal termination.
  // Once abnormal termination was caught, negative status should not be
  // propagated.
  if (Res < 0)
    Res = 1;
#endif

  // If we have multiple failing commands, we return the result of the first
  // failing command.
  return Res;
}

int clang_main(int argc, char **argv, const llvm::ToolContext &ToolContext) {
  if(argc > 1 && strcmp(argv[1], fork_server::bob_argv) == 0) {
    // start forkserver
    start_forkserver(argc, argv, ToolContext);
  } else {
    return real_clang_main(argc, argv, ToolContext);
  }
}

