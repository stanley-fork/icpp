/* Interpreting C++, executing the source and executable like a script */
/* By Jesse Liu < neoliu2011@gmail.com >, 2024 */
/* Copyright (c) vpand.com 2024. This file is released under LGPL2.
   See LICENSE in root directory for more details
*/

#include "runtime.h"
#include "exec.h"
#include "icpp.h"
#include "loader.h"
#include "platform.h"
#include "runcfg.h"
#include "utils.h"
#include "llvm/Support/MemoryBuffer.h"
#include <isymhash.pb.h>

namespace icpp {

RuntimeLib &RuntimeLib::inst() {
  static RuntimeLib rt;
  return rt;
}

RuntimeLib::RuntimeLib() {}

RuntimeLib::~RuntimeLib() {}

fs::path RuntimeLib::repo(bool force) {
  auto home = fs::path(home_directory()) / repoName;
  return force ? must_exist(home) : home;
}

fs::path RuntimeLib::includeFull() {
  return must_exist(repo() / includeRelative());
}

fs::path RuntimeLib::libFull() { return must_exist(repo() / libRelative()); }

void RuntimeLib::initHashes() {
  if (hashes_.size())
    return;

  for (auto &entry : fs::directory_iterator(libFull())) {
    if (entry.is_directory()) {
      auto hashfile = entry.path() / hashFile;
      auto expBuff = llvm::MemoryBuffer::getFile(hashfile.string());
      if (!expBuff)
        continue; // symbol.hash file is missing, ignore this module

      auto buffer = expBuff.get().get();
      auto newit = hashes_
                       .insert({entry.path().filename().string(),
                                std::make_unique<vpimod::SymbolHash>()})
                       .first;
      if (!newit->second->ParseFromArray(buffer->getBufferStart(),
                                         buffer->getBufferSize())) {
        log_print(Runtime, "Failed to parse {}.", hashfile.string());
        continue;
      }
      log_print(Develop, "Loaded symbol hashes from {}.", newit->first);
    }
  }
}

#if ON_WINDOWS
static constexpr const char *symbol_name(std::string_view raw) {
  return raw.data() + (raw.starts_with("__imp_") ? 6 : 0);
}
#else
#define symbol_name(raw) (raw.data() + 0)
#endif

fs::path RuntimeLib::find(std::string_view symbol) {
  auto hash =
      static_cast<uint32_t>(std::hash<std::string_view>{}(symbol_name(symbol)));
  // foreach module
  for (auto &mh : hashes_) {
    // foreach object/library
    for (auto lh : mh.second->hashes()) {
      auto hashbuff = reinterpret_cast<const uint32_t *>(&lh.second[0]);
      if (std::binary_search(hashbuff,
                             hashbuff + lh.second.size() / sizeof(hashbuff[0]),
                             hash)) {
        return libFull(mh.first) / lh.first;
      }
    }
  }
  return "";
}

std::vector<std::string_view> RuntimeLib::modules() {
  // initialize the symbol hashes for the third-party modules lazy loading
  if (fs::exists(repo(false)))
    initHashes();

  std::vector<std::string_view> ms;
  for (auto &mh : hashes_)
    ms.push_back(mh.first);
  return ms;
}

namespace api {

// the icpp interpreter version
std::string_view version() { return icpp::version_string(); }

// the icpp main program argv[0] path
std::string_view program() { return icpp::RunConfig::inst()->program; }

// the current user home directory, e.g.: ~, C:/Users/icpp
std::string_view home_directory() { return icpp::home_directory(); }

// execute a c++ expression
int exec_expression(std::string_view expr) {
  return icpp::exec_string(icpp::RunConfig::inst()->program, expr);
}

// execute a c++ source from string
int exec_string(std::string_view code, int argc, const char **argv) {
  return icpp::exec_string(icpp::RunConfig::inst()->program, code, true, argc,
                           argv);
}

// execute a c++ source file
int exec_source(std::string_view path, int argc, const char **argv) {
  return icpp::exec_source(icpp::RunConfig::inst()->program, path, argc, argv);
}

// execute an icpp module installed by imod
int exec_module(std::string_view module, int argc, const char **argv) {
  auto omain = icpp::RuntimeLib::inst().libFull(module) / "main.o";
  if (fs::exists(omain)) {
    std::vector<std::string> deps;
    int iargc = 1;
    auto mname = module.data();
    const char **iarg = &mname;
    if (argc) {
      iargc = argc;
      iarg = argv;
    }
    bool validcache;
    return icpp::exec_main(omain.string(), deps, omain.string(), iargc,
                           const_cast<char **>(iarg), validcache);
  }
  icpp::log_print(
      Runtime, "The module '{}' doesn't contain a main.o entry file.", module);
  return -1;
}

// result setter/getter for main script and its sub script
// which is executed by exec_* api
/*
e.g.:
  icpp::exec_expression("result_set(520)");
  icpp::prints("Result: {}", result_get());
*/
static long result_i = 0;
static std::string result_s;

void result_set(long result) { result_i = result; }

void result_sets(const std::string_view &result) { result_s = result; }

long result_get() { return result_i; }

std::string_view result_gets() { return result_s; }

// load a native library
void *load_library(std::string_view path) {
  return const_cast<void *>(icpp::load_library(path));
}

// unload a native library
void *unload_library(void *handle) {
  icpp::log_print(Runtime,
                  "Doesn't support unloading native library currently.");
  return handle;
}

// lookup a native symbol
// default search in the whole program
void *resolve_symbol(std::string_view name, void *handle) {
  return const_cast<void *>(icpp::find_symbol(handle, name));
}

// iterate all the native modules in this running process,
// return true to break iterating
void iterate_modules(
    const std::function<bool(uint64_t base, std::string_view path)> &callback) {
  icpp::iterate_modules(callback);
}

// check whether the given path ends with a c++ source file extension or not
bool is_cpp_source(std::string_view path) { return icpp::is_cpp_source(path); }

// random value or string generator
int rand_value() { return icpp::rand_value(); }

std::string_view rand_string(char *buff, int length) {
  auto str = icpp::rand_string(length);
  std::memcpy(buff, str.data(), length);
  return {buff, static_cast<size_t>(length)};
}

void regex::init(std::string_view pattern, int flags) {
  auto rflags = static_cast<std::regex_constants::syntax_option_type>(flags);
  context_ = new std::regex(pattern.data(), rflags);
}

void regex::deinit() {
  auto preg = static_cast<std::regex *>(context_);
  delete preg;
}

// return true if str matches the initial pattern
bool regex::search(std::string_view str) const {
  auto preg = static_cast<std::regex *>(context_);
  return std::regex_search(str.data(), str.data() + str.size(), *preg);
}

} // namespace api

} // namespace icpp
