#pragma once
#include <string>
namespace acecode::cli {
void configure_process_environment();
std::string get_executable_dir_from_argv(int argc, char* argv[]);
}
