/* Interpreting C++, executing the source and executable like a script */
/* By Jesse Liu < neoliu2011@gmail.com >, 2024 */
/* This file is released under LGPL2.
   See LICENSE in root directory for more details
*/

#include "exec.h"
#include "icpp.h"
#include "utils.h"
#include <filesystem>
#include <format>
#include <span>

using namespace std::literals;
namespace fs = std::filesystem;

// icpp/clang driver entry, it acts as a clang compiler when argv contains -c/-o
extern "C" int main(int argc, const char **argv);

// implement in llvm-project/clang/tools/driver/driver.cpp
extern std::string GetExecutablePath(const char *Argv0, bool CanonicalPrefixes);

static void print_version() {
  std::cout
      << "ICPP " << icpp::version_string()
      << " based on Unicorn and Clang/LLVM." << std::endl
      << "Interpreting C++, executing the source and executable like a script."
      << std::endl
      << "Project website: https://github.com/vpand/icpp/" << std::endl
      << "Sponsor website: https://vpand.com/" << std::endl
      << std::endl;
}

static void print_help() {
  std::cout
      << "OVERVIEW: ICPP " << icpp::version_string()
      << " based on Unicorn and Clang/LLVM." << std::endl
      << "  Interpreting C++, executing the source and executable like a "
         "script."
      << std::endl
      << std::endl
      << "USAGE: icpp [options] file0 [file1 ...] [-- args]" << std::endl
      << "OPTIONS:" << std::endl
      << "  -v, -version: print icpp version." << std::endl
      << "  --version: print icpp and clang version." << std::endl
      << "  -h, -help: print icpp help list." << std::endl
      << "  --help: print icpp and clang help list." << std::endl
      << "  -O0, -O1, -O2, -O3, -Os, -Oz: optimization level passed to "
         "clang, default to -O2."
      << std::endl
      << "  -I/path/to/include: header include directory passed to clang."
      << std::endl
      << "  -L/path/to/library: library search directory passed to icpp "
         "interpreting engine."
      << std::endl
      << "  -lname: full name of the dependent library file passed to icpp "
         "interpreting engine, e.g.: liba.dylib, liba.so, a.dll."
      << std::endl
      << "  -F/path/to/framework: framework search directory passed to icpp "
         "interpreting engine."
      << std::endl
      << "  -fname: framework name of the dependent library file passed to "
         "icpp "
         "interpreting engine."
      << std::endl
      << "  -p/path/to/json: professional json configuration file for "
         "trace/profile/plugin/etc.."
      << std::endl
      << "FILES: input file can be C++ source code(.c/.cc/.cpp/.cxx), "
         "MachO/ELF/PE executable."
      << std::endl
      << "ARGS: arguments passed to the main entry function of the input files."
      << std::endl
      << std::endl
      << "e.g.:" << std::endl
      << "  icpp helloworld.cc" << std::endl
      << R"x(  icpp helloworld.cc -- Hello World (i.e.: argc=3, argv[]={"helloworld.cc", "Hello", "World"}))x"
      << std::endl
      << "  icpp -O3 helloworld.cc" << std::endl
      << "  icpp -O0 -p/path/to/profile.json helloworld.cc" << std::endl
      << "  icpp -p/path/to/trace.json helloworld.exe" << std::endl
      << "  icpp -I/qt/include -L/qt/lib -llibQtCore.so hellowrold.cc"
      << std::endl
      << "  icpp -I/qt/include -L/qt/lib -lQtCore.dll hellowrold.cc"
      << std::endl
      << "  icpp -I/qt/include -F/qt/framework -fQtCore hellowrold.cc"
      << std::endl
      << std::endl;
}

static fs::path compile_source(const char *Argv0, std::string_view path,
                               const char *opt,
                               const std::vector<const char *> &incdirs) {
  // construct a temporary output object file path
  auto opath = fs::temp_directory_path() / icpp::rand_filename(8, ".o");

  // construct a full path which the last element must be "clang" to make clang
  // driver happy, otherwise it can't compile source to object, it seems that
  // clang driver depends on clang name to do the right compilation logic
  auto exepath = GetExecutablePath(Argv0, true);
  // this full path ends with "clang", it's exactly the format that clang driver
  // wants
  auto program = fs::path(exepath).parent_path() / ".." / "lib" / "clang";

  std::vector<const char *> args;
  args.push_back(program.c_str());
  // make clang driver to use our fake clang path as the executable path
  args.push_back("-no-canonical-prefixes");
  args.push_back("-std=gnu++23");
  args.push_back(opt);
  args.push_back("-c");
  args.push_back(path.data());
  args.push_back("-o");
  args.push_back(opath.c_str());

  // add user specified include directories
  for (auto i : incdirs) {
    args.push_back(i);
  }

#if __APPLE__
#define MACOSX_SDK                                                             \
  "/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/"      \
  "Developer/SDKs/MacOSX.sdk"
  args.push_back("-isysroot");
  args.push_back(MACOSX_SDK);
#elif __linux__
#error Un-implement the Linux platform currently.
#elif _WIN32
#error Un-implement the Windows platform currently.
#else
#error Unknown compiling platform.
#endif

  // main will invoke clang_main to generate the object file with the default
  // host triple
  main(static_cast<int>(args.size()), &args[0]);
  return opath;
}

static std::vector<std::string>
get_dependencies(const std::vector<const char *> &libdirs,
                 const std::vector<const char *> &libs,
                 const std::vector<const char *> &framedirs,
                 const std::vector<const char *> &frameworks) {
  std::vector<std::string> deps;
  for (auto l : libdirs) {
    for (auto n : libs) {
      auto lib = fs::path(l) / n;
      if (fs::exists(lib)) {
        deps.push_back(lib);
        break;
      }
    }
  }
  for (auto f : framedirs) {
    for (auto n : frameworks) {
      auto frame = fs::path(f) / std::format("{}.framework", n) / n;
      if (fs::exists(frame)) {
        deps.push_back(frame);
        break;
      }
    }
  }
  return deps;
}

int icpp_main(int argc, char **argv) {
  // optimization level passed to clang
  const char *icpp_option_opt = "-O2";

  // include directory passed to clang
  std::vector<const char *> icpp_option_incdirs;

  // library directory passed to exec engine
  std::vector<const char *> icpp_option_libdirs;

  // library name passed to exec engine for dependent runtime symbol lookup
  std::vector<const char *> icpp_option_libs;

  // framework directory passed to exec engine
  std::vector<const char *> icpp_option_framedirs;

  // framework name passed to exec engine for dependent runtime symbol lookup
  std::vector<const char *> icpp_option_frameworks;

  // professional json configuration file for trace/profile/plugin
  const char *icpp_option_procfg = "";

  // mark the double dash index, all the args after idoubledash will be passed
  // to the input file
  int idoubledash = argc;
  for (int i = 0; i < argc; i++) {
    if (std::string_view(argv[i]) == "--"sv) {
      idoubledash = i;
      break;
    }
  }

  // skip argv[0] and argv[idoubledash, ...]
  auto args = std::span{argv + 1, static_cast<std::size_t>(idoubledash - 1)};

  // parse the command line arguments for icpp options
  for (auto p : args) {
    auto sp = std::string_view(p);
    if (sp == "-v"sv || sp == "-version"sv) {
      print_version();
      return 0; // return to main to exit this program
    }
    if (sp == "--version"sv) {
      print_version();
      return 1; // continuing let clang print its version
    }
    if (sp == "-h"sv || sp == "-help"sv) {
      print_help();
      return 0;
    }
    if (sp == "--help"sv) {
      print_help();
      return 1; // continuing let clang print its help list
    }
    if (sp == "-c"sv || sp == "-o"sv) {
      return 1; // continuing let clang do the compilation task
    }
    if (sp.starts_with("-I")) {
      // forward to clang
      icpp_option_incdirs.push_back(sp.data());
    } else if (sp.starts_with("-L")) {
      icpp_option_libdirs.push_back(sp.data() + 2);
    } else if (sp.starts_with("-l")) {
      icpp_option_libs.push_back(sp.data() + 2);
    } else if (sp.starts_with("-F")) {
      icpp_option_framedirs.push_back(sp.data() + 2);
    } else if (sp.starts_with("-f")) {
      icpp_option_frameworks.push_back(sp.data() + 2);
    } else if (sp.starts_with("-p")) {
      icpp_option_procfg = sp.data() + 2;
    }
  }

  auto deps = get_dependencies(icpp_option_libdirs, icpp_option_libs,
                               icpp_option_framedirs, icpp_option_frameworks);
  // interpret the input Source-C++ or
  // Executable-MachO/Executable-ELF/Executable-PE files
  for (auto p : args) {
    auto sp = std::string_view(p);
    if (sp[0] == '-')
      continue;
    if (!fs::exists(fs::path(sp))) {
      std::cout << "Input file '" << sp << "' doesn't exist." << std::endl;
      continue;
    }
    if (icpp::is_cpp_source(sp)) {
      // compile the input source to be as the running host object file(.o,
      // .obj)
      auto opath =
          compile_source(argv[0], sp, icpp_option_opt, icpp_option_incdirs);
      if (fs::exists(opath)) {
        icpp::exec_main(opath.c_str(), deps, icpp_option_procfg,
                        idoubledash - argc, &argv[idoubledash + 1]);
        fs::remove(opath);
      } else {
        // if failed to compile the input source, clang has already printed the
        // errors
      }
    } else {
      // pass sp as an executable file
      icpp::exec_main(sp, deps, icpp_option_procfg, idoubledash - argc,
                      &argv[idoubledash + 1]);
    }
  }
  return 0;
}
