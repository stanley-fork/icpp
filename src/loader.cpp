/* Interpreting C++, executing the source and executable like a script */
/* By Jesse Liu < neoliu2011@gmail.com >, 2024 */
/* Copyright (c) vpand.com 2024. This file is released under LGPL2.
   See LICENSE in root directory for more details
*/

#include "loader.h"
#include "arch.h"
#include "exec.h"
#include "log.h"
#include "object.h"
#include "platform.h"
#include "runcfg.h"
#include "runtime.h"
#include <cstdio>
#include <iostream>
#include <llvm/Config/config.h>
#include <locale>
#include <map>
#include <mutex>
#include <stdio.h>
#include <thread>
#include <unordered_map>

extern "C" {
#if __linux__
void __divti3(void);
void __modti3(void);
void __udivti3(void);
void __umodti3(void);
void __unordtf2(void);
#if ARCH_ARM64
void __aarch64_ldadd8_acq_rel(void);
void __aarch64_ldadd8_relax(void);
void __aarch64_ldadd4_acq_rel(void);
void __aarch64_ldadd4_relax(void);
#endif
#elif ON_WINDOWS
void _CxxThrowException(void);

/*
A thread unsafe simulation of msvc tls initialization
*/
// copied from crt\src\vcruntime\thread_safe_statics.cpp
static int const uninitialized = 0;
static int const being_initialized = -1;
static int const epoch_start = INT_MIN;
static void icpp_Init_thread_header(int *const pOnce) {
  *pOnce = being_initialized;
}
static void icpp_Init_thread_footer(int *const pOnce) { *pOnce = epoch_start; }
static void icpp_Init_thread_abort(int *const pOnce) { *pOnce = uninitialized; }
#endif
}

#if ON_WINDOWS
extern uint64_t __security_cookie;
#endif

namespace icpp {

// some simulated system global variables
#if ON_WINDOWS
static const int _tls_index = 0;
static const int _init_thread_epoch = epoch_start;
#else
static uint64_t __dso_handle = 0;
#endif

static const void *global_locals[] = {
#if ON_WINDOWS
    &_tls_index,
    &_init_thread_epoch,
#else
    &__dso_handle,
#endif
};

libcpp_thread_create_t libcpp_thread_create = nullptr;

static void nop_function(void) {}

struct ModuleLoader {
  ModuleLoader() : mainid_(std::this_thread::get_id()) {
    // these symbols are extern in object but finally linked in exe/lib,
    // or
    // they have a different signature between the host and clang libc++.
    //
    // herein simulates this behaviour or redirects it to the pre-cached
    // implementation
#if __linux__
    syms_.insert({"__dso_handle", &__dso_handle});
    syms_.insert({"__divti3", reinterpret_cast<const void *>(&__divti3)});
    syms_.insert({"__modti3", reinterpret_cast<const void *>(&__modti3)});
    syms_.insert({"__udivti3", reinterpret_cast<const void *>(&__udivti3)});
    syms_.insert({"__umodti3", reinterpret_cast<const void *>(&__umodti3)});
    syms_.insert({"__unordtf2", reinterpret_cast<const void *>(&__unordtf2)});
#if ARCH_ARM64
    syms_.insert({"__aarch64_ldadd8_acq_rel",
                  reinterpret_cast<const void *>(&__aarch64_ldadd8_acq_rel)});
    syms_.insert({"__aarch64_ldadd8_relax",
                  reinterpret_cast<const void *>(&__aarch64_ldadd8_relax)});
    syms_.insert({"__aarch64_ldadd4_acq_rel",
                  reinterpret_cast<const void *>(&__aarch64_ldadd4_acq_rel)});
    syms_.insert({"__aarch64_ldadd4_relax",
                  reinterpret_cast<const void *>(&__aarch64_ldadd4_relax)});
#endif
#elif ON_WINDOWS
    // clang libc++: operator delete(void *,unsigned __int64)
    // msvc: operator delete(void *)
    // redirect new/delete to malloc/free
    auto ucrt = ::LoadLibraryA("ucrtbase.dll");
    syms_.insert({"??2@YAPEAX_K@Z", reinterpret_cast<const void *>(
                                        ::GetProcAddress(ucrt, "malloc"))});
    syms_.insert({"??3@YAXPEAX_K@Z", reinterpret_cast<const void *>(
                                         ::GetProcAddress(ucrt, "free"))});
    // rtti type info vtable
    auto rtti = &typeid(char *);
    syms_.insert({"??_7type_info@@6B@", *(const void **)(rtti)});
    // throw exception
    syms_.insert({"_CxxThrowException",
                  reinterpret_cast<const void *>(&_CxxThrowException)});
    syms_.insert({"__security_cookie",
                  reinterpret_cast<const void *>(&__security_cookie)});
    syms_.insert({"__security_check_cookie",
                  reinterpret_cast<const void *>(&nop_function)});
    syms_.insert({"_tls_index", &_tls_index});
    syms_.insert({"_Init_thread_epoch", &_init_thread_epoch});
    syms_.insert({"_Init_thread_header",
                  reinterpret_cast<const void *>(&icpp_Init_thread_header)});
    syms_.insert({"_Init_thread_footer",
                  reinterpret_cast<const void *>(&icpp_Init_thread_footer)});
    syms_.insert({"_Init_thread_abort",
                  reinterpret_cast<const void *>(&icpp_Init_thread_abort)});
#endif

#if __APPLE__
    syms_.insert({"___dso_handle", &__dso_handle});
    // currently, the clang cpp module initializer is a nop function,
    // and we will skip to call it in ctor caller
    syms_.insert({"__ZGIW3std", reinterpret_cast<const void *>(&nop_function)});
#else
    syms_.insert({"_ZGIW3std", reinterpret_cast<const void *>(&nop_function)});
#endif

    // load c++ runtime library
    auto libpath =
        fs::absolute(RunConfig::inst()->program).parent_path() / "../lib";
#if ON_WINDOWS
    // set boost dependency
    ::AddDllDirectory(libpath.wstring().data());
    ::LoadLibraryA("Shell32.dll");

    auto mcxx = loadLibrary((libpath / "c++" LLVM_PLUGIN_EXT).string());

    libcpp_thread_create = (libcpp_thread_create_t)(resolve(
        mcxx, "?__libcpp_thread_create@__1@std@@YAHPEAPEAXP6APEAXPEAX@Z1@Z",
        false));
#else
    syms_.insert(
        {"___cxa_atexit", reinterpret_cast<const void *>(&__cxa_atexit)});
    syms_.insert(
        {"___cxa_throw", reinterpret_cast<const void *>(&__cxa_throw)});
    syms_.insert({"___stack_chk_fail",
                  reinterpret_cast<const void *>(&__stack_chk_fail)});

#if ANDROID
    loadLibrary((libpath / "libc++" LLVM_PLUGIN_EXT).string());
    loadLibrary((libpath / "libc++abi" LLVM_PLUGIN_EXT).string());
    loadLibrary((libpath / "libunwind" LLVM_PLUGIN_EXT).string());
#elif __APPLE__
    loadLibrary((libpath / "libc++.1" LLVM_PLUGIN_EXT).string());
    loadLibrary((libpath / "libc++abi.1" LLVM_PLUGIN_EXT).string());
    loadLibrary((libpath / "libunwind.1" LLVM_PLUGIN_EXT).string());
#else
    loadLibrary((libpath / "libc++" LLVM_PLUGIN_EXT ".1").string());
    loadLibrary((libpath / "libc++abi" LLVM_PLUGIN_EXT ".1").string());
    loadLibrary((libpath / "libunwind" LLVM_PLUGIN_EXT ".1").string());
#endif
#endif

    // initialize the symbol hashes for the third-party modules lazy loading
    if (fs::exists(RuntimeLib::inst().repo(false)))
      RuntimeLib::inst().initHashes();

    // cache the apis
    auto regexInit = &api::regex::init;
    auto regexDeinit = &api::regex::deinit;
    auto regexSearch = &api::regex::search;
#if ON_WINDOWS
    syms_.insert({"?home_directory@icpp@@YA?AV?$basic_string_view@DU?$char_"
                  "traits@D@__1@std@@@__1@std@@XZ",
                  api::home_directory});
    syms_.insert({"?version@icpp@@YA?AV?$basic_string_view@DU?$char_traits@D@__"
                  "1@std@@@__1@std@@XZ",
                  api::version});
    syms_.insert({"?program@icpp@@YA?AV?$basic_string_view@DU?$char_traits@D@__"
                  "1@std@@@__1@std@@XZ",
                  api::program});
    syms_.insert({"?exec_expression@icpp@@YAHV?$basic_string_view@DU?$char_"
                  "traits@D@__1@std@@@__1@std@@@Z",
                  api::exec_expression});
    syms_.insert({"?exec_string@icpp@@YAHV?$basic_string_view@DU?$char_traits@"
                  "D@__1@std@@@__1@std@@HPEAPEBD@Z",
                  api::exec_string});
    syms_.insert({"?exec_source@icpp@@YAHV?$basic_string_view@DU?$char_traits@"
                  "D@__1@std@@@__1@std@@HPEAPEBD@Z",
                  api::exec_source});
    syms_.insert({"?exec_module@icpp@@YAHV?$basic_string_view@DU?$char_traits@"
                  "D@__1@std@@@__1@std@@HPEAPEBD@Z",
                  api::exec_module});
    syms_.insert({"?result_gets@icpp@@YA?AV?$basic_string_view@DU?$char_traits@"
                  "D@__1@std@@@__1@std@@XZ",
                  api::result_gets});
    syms_.insert({"?result_get@icpp@@YAJXZ", api::result_get});
    syms_.insert({"?is_cpp_source@icpp@@YA_NV?$basic_string_view@DU?$char_"
                  "traits@D@__1@std@@@__1@std@@@Z",
                  api::is_cpp_source});
    syms_.insert({"?rand_string@icpp@@YA?AV?$basic_string_view@DU?$char_traits@"
                  "D@__1@std@@@__1@std@@PEADH@Z",
                  api::rand_string});
    syms_.insert({"?rand_value@icpp@@YAHXZ", api::rand_value});
    syms_.insert({"?load_library@icpp@@YAPEAXV?$basic_string_view@DU?$char_"
                  "traits@D@__1@std@@@__1@std@@@Z",
                  api::load_library});
    syms_.insert({"?unload_library@icpp@@YAPEAXPEAX@Z", api::unload_library});
    syms_.insert({"?resolve_symbol@icpp@@YAPEAXV?$basic_string_view@DU?$char_"
                  "traits@D@__1@std@@@__1@std@@PEAX@Z",
                  api::resolve_symbol});
    syms_.insert(
        {"?iterate_modules@icpp@@YAXAEBV?$function@$$A6A_N_KV?$basic_string_"
         "view@DU?$char_traits@D@__1@std@@@__1@std@@@Z@__1@std@@@Z",
         api::iterate_modules});
    syms_.insert({"?result_set@icpp@@YAXJ@Z", api::result_set});
    syms_.insert({"?result_set@icpp@@YAXAEBV?$basic_string_view@DU?$char_"
                  "traits@D@__1@std@@@__1@std@@@Z",
                  api::result_sets});
    syms_.insert({"?init@regex@icpp@@AEAAXV?$basic_string_view@DU?$char_traits@"
                  "D@__1@std@@@__1@std@@H@Z",
                  *(const void **)(&regexInit)});
    syms_.insert(
        {"?deinit@regex@icpp@@AEAAXXZ", *(const void **)(&regexDeinit)});
    syms_.insert({"?search@regex@icpp@@QEBA_NV?$basic_string_view@DU?$char_"
                  "traits@D@__1@std@@@__1@std@@@Z",
                  *(const void **)(&regexSearch)});
#else
#if __APPLE__
#define apisym(n) #n
#else
#define apisym(n) (const char *)#n + 1
#endif
    syms_.insert({apisym(__ZN4icpp7programEv),
                  reinterpret_cast<const void *>(&api::program)});
    syms_.insert({apisym(__ZN4icpp7versionEv),
                  reinterpret_cast<const void *>(&api::version)});
    syms_.insert({apisym(__ZN4icpp14home_directoryEv),
                  reinterpret_cast<const void *>(&api::home_directory)});
    syms_.insert(
        {apisym(
             __ZN4icpp15exec_expressionENSt3__117basic_string_viewIcNS0_11char_traitsIcEEEE),
         reinterpret_cast<const void *>(&api::exec_expression)});
    syms_.insert(
        {apisym(
             __ZN4icpp11exec_stringENSt3__117basic_string_viewIcNS0_11char_traitsIcEEEEiPPKc),
         reinterpret_cast<const void *>(&api::exec_string)});
    syms_.insert(
        {apisym(
             __ZN4icpp11exec_sourceENSt3__117basic_string_viewIcNS0_11char_traitsIcEEEEiPPKc),
         reinterpret_cast<const void *>(&api::exec_source)});
    syms_.insert(
        {apisym(
             __ZN4icpp11exec_moduleENSt3__117basic_string_viewIcNS0_11char_traitsIcEEEEiPPKc),
         reinterpret_cast<const void *>(&api::exec_module)});
    syms_.insert({apisym(__ZN4icpp10result_getEv),
                  reinterpret_cast<const void *>(&api::result_get)});
    syms_.insert({apisym(__ZN4icpp11result_getsEv),
                  reinterpret_cast<const void *>(&api::result_gets)});
    syms_.insert(
        {apisym(
             __ZN4icpp13is_cpp_sourceENSt3__117basic_string_viewIcNS0_11char_traitsIcEEEE),
         reinterpret_cast<const void *>(&api::is_cpp_source)});
    syms_.insert({apisym(__ZN4icpp10rand_valueEv),
                  reinterpret_cast<const void *>(&api::rand_value)});
    syms_.insert({apisym(__ZN4icpp11rand_stringEPci),
                  reinterpret_cast<const void *>(&api::rand_string)});
    syms_.insert(
        {apisym(
             __ZN4icpp12load_libraryENSt3__117basic_string_viewIcNS0_11char_traitsIcEEEE),
         reinterpret_cast<const void *>(&api::load_library)});
    syms_.insert({apisym(__ZN4icpp14unload_libraryEPv),
                  reinterpret_cast<const void *>(&api::unload_library)});
    syms_.insert(
        {apisym(
             __ZN4icpp14resolve_symbolENSt3__117basic_string_viewIcNS0_11char_traitsIcEEEEPv),
         reinterpret_cast<const void *>(&api::resolve_symbol)});
    syms_.insert(
        {apisym(
             __ZN4icpp15iterate_modulesERKNSt3__18functionIFbyNS0_17basic_string_viewIcNS0_11char_traitsIcEEEEEEE),
         reinterpret_cast<const void *>(&api::iterate_modules)});
    syms_.insert(
        {apisym(
             __ZN4icpp15iterate_modulesERKNSt3__18functionIFbmNS0_17basic_string_viewIcNS0_11char_traitsIcEEEEEEE),
         reinterpret_cast<const void *>(&api::iterate_modules)});
    syms_.insert({apisym(__ZN4icpp10result_setEl),
                  reinterpret_cast<const void *>(&api::result_set)});
    syms_.insert(
        {apisym(
             __ZN4icpp10result_setERKNSt3__117basic_string_viewIcNS0_11char_traitsIcEEEE),
         reinterpret_cast<const void *>(&api::result_sets)});
    syms_.insert(
        {apisym(
             __ZN4icpp5regex4initENSt3__117basic_string_viewIcNS1_11char_traitsIcEEEEi),
         *(const void **)(&regexInit)});
    syms_.insert(
        {apisym(__ZN4icpp5regex6deinitEv), *(const void **)(&regexDeinit)});
    syms_.insert(
        {apisym(
             __ZNK4icpp5regex6searchENSt3__117basic_string_viewIcNS1_11char_traitsIcEEEE),
         *(const void **)(&regexSearch)});
#endif
  }

  ~ModuleLoader() {}

  void cacheAndClean(int exitcode) {
    // generate the iobject module cache if everything went well
    if (!exitcode) {
      for (auto io : imods_) {
        if (io->isCache())
          continue;
        io->generateCache();
      }
    }
    imods_.clear();
  }

  void cacheSymbol(std::string_view name, const void *impl) {
    syms_.insert({name.data(), impl});
  }

  bool isMain() { return mainid_ == std::this_thread::get_id(); }

  struct LockGuard {
    LockGuard(ModuleLoader *p, std::recursive_mutex &m)
        : parent_(p), mutex_(m) {
      if (!parent_->isMain())
        mutex_.lock();
    }

    ~LockGuard() {
      if (!parent_->isMain())
        mutex_.unlock();
    }

    ModuleLoader *parent_;
    std::recursive_mutex &mutex_;
  };

  const void *loadLibrary(std::string_view path);
  const void *resolve(const void *handle, std::string_view name, bool data);
  const void *resolve(std::string_view name, bool data);
  std::string find(const void *addr, bool update);

  // it'll be invoked by execute engine when loaded a iobject module
  void cacheObject(std::shared_ptr<Object> imod);

  bool executable(uint64_t vm, Object **iobject) {
    for (auto m : imods_) {
      if (m->executable(vm, nullptr)) {
        iobject[0] = m.get();
        return true;
      }
    }
    return false;
  }

  bool belong(uint64_t vm) {
    for (auto m : imods_) {
      if (m->belong(vm))
        return true;
    }
    return false;
  }

private:
  const void *resolveInCache(std::string_view name, bool data);
  const void *lookup(std::string_view name, bool data);
#if __linux__
  friend int iter_so_callback(dl_phdr_info *info, size_t size, void *data);
#endif

  std::thread::id mainid_;
  std::recursive_mutex mutex_;

  // cached symbols
  std::unordered_map<std::string, const void *> syms_;

  // native modules
  std::map<uint64_t, std::string> mods_;
  std::vector<std::map<uint64_t, std::string>::iterator> modits_;

  // native module handles
  std::map<std::string, const void *> mhandles_;
  std::vector<std::map<std::string, const void *>::iterator> mhandleits_;

  // iobject modules
  std::vector<std::shared_ptr<Object>> imods_;
};

// the module/object loader
static std::unique_ptr<ModuleLoader> moloader;

const void *ModuleLoader::loadLibrary(std::string_view path) {
  LockGuard lock(this, mutex_);
  auto found = mhandles_.find(path.data());
  if (found == mhandles_.end()) {
    bool iobj = path.ends_with(obj_ext) || path.ends_with(iobj_ext);
    auto addr = iobj ? nullptr : load_library(path.data());
    if (!addr) {
      if (iobj) {
        // check the already loaded/cached iobject module
        auto found = mhandles_.find(path.data());
        if (found != mhandles_.end()) {
          return found->second;
        }

        bool validcache;
        auto object = create_object("", path, validcache);
        if (object && object->valid()) {
          // initialize this iobject module, call its construction functions,
          // it'll call the Loader::cacheObject after executing the ctors
          init_library(object);
          imods_.push_back(object);
          addr = object.get();
        }
      }
      if (!addr) {
        log_print(Runtime, "Failed to load library: {}", path.data());
        return nullptr;
      }
    }
    if (addr)
      log_print(Develop, "Loaded module {}.", path.data());
    found = mhandles_.insert({path.data(), addr}).first;
    mhandleits_.push_back(found);
  }
  return found->second;
}

const void *ModuleLoader::resolveInCache(std::string_view name, bool data) {
  auto found = syms_.find(name.data());
  if (found == syms_.end())
    return nullptr;
  for (auto loc : global_locals) {
    // return the simulated global locals directly
    if (found->second == loc)
      return loc;
  }
  // return a second level pointer if applying data type symbol
  return data ? &found->second : found->second;
}

const void *ModuleLoader::resolve(const void *handle, std::string_view name,
                                  bool data) {
  LockGuard lock(this, mutex_);

  const void *target = resolveInCache(name, data);
  if (target)
    return target;

  // check it in iobject modules
  for (auto io : imods_) {
    if (handle != io.get())
      continue;
    auto t = io->locateSymbol(name);
    if (t) {
      target = t;
      break;
    }
  }

  // check it in native modules
  if (!target) {
    target = find_symbol(const_cast<void *>(handle), name);
  }

  if (!target)
    return nullptr;
  auto newit = syms_.insert({name.data(), target}).first;
  return data ? &newit->second : newit->second;
}

const void *ModuleLoader::resolve(std::string_view name, bool data) {
  LockGuard lock(this, mutex_);

  const void *target = resolveInCache(name, data);
  return target ? target : lookup(name, data);
}

const void *ModuleLoader::lookup(std::string_view name, bool data) {
  // load boost libraries lazily
  static bool boost = false;
  if (!boost && name.find("boost") != std::string_view::npos) {
    boost = true;

    // load these libs in the end, otherwise failed loading
    std::vector<std::string> lazylibs;
    for (auto &entry : fs::recursive_directory_iterator(
             fs::absolute(RunConfig::inst()->program).parent_path() / ".." /
             "lib" / "boost")) {
      auto libpath = entry.path();
      auto name = libpath.filename().string();
      if (entry.is_regular_file() && !entry.is_symlink() &&
          name.find(LLVM_PLUGIN_EXT) != std::string::npos) {
#if __linux__
        if (name.find("boost_log") != std::string::npos ||
            name.find("boost_locale") != std::string::npos ||
            name.find("boost_fiber_numa") != std::string::npos) {
          lazylibs.push_back(libpath.string());
          continue;
        }
#endif
        loadLibrary(libpath.string());
      }
    }
    for (auto &p : lazylibs)
      loadLibrary(p);
  }

  const void *target = nullptr;

  // check it in iobject modules
  for (auto io : imods_) {
    if ((target = io->locateSymbol(name)))
      break;
  }

  // check it in loaded modules
  if (!target) {
    for (auto &m : mhandleits_) {
      if ((target = find_symbol(m->second, name)))
        break;
    }
  }

  // check it in native system modules
  if (!target)
    target = find_symbol(nullptr, name);

  if (!target) {
    // the final chance to resolve this symbol
    auto path = RuntimeLib::inst().find(name);
    if (!path.empty()) {
      auto handle = loadLibrary(path.string());
      target = resolve(handle, name, data);
    }
    // Oops...
    if (!target) {
      log_print(Runtime,
                "Fatal error, failed to resolve symbol {}, redirect to abort.",
                name.data());
      target = reinterpret_cast<const void *>(&abort);
    }
  }

  // cache it
  auto newit = syms_.insert({name.data(), target}).first;
  return data ? &newit->second : newit->second;
}

std::string ModuleLoader::find(const void *addr, bool update) {
  if (mods_.size() == 0 || update) {
    LockGuard lock(this, mutex_);
    iterate_modules([](uint64_t base, std::string_view path) {
      moloader->mods_.insert({base, path.data()});
      return false;
    });
    // reset module iterators
    modits_.clear();
    for (auto it = mods_.begin(); it != mods_.end(); it++) {
      modits_.push_back(it);
    }
  }
  // check it in iobject module
  for (auto m : imods_) {
    if (m->belong(reinterpret_cast<uint64_t>(addr))) {
      return m->cachePath();
    }
  }
  // binary search for the module
  long low = 0;
  long high = modits_.size() - 1;
  auto target = reinterpret_cast<uint64_t>(addr);
  while (low <= high) {
    auto mid = (low + high) / 2;
    if (mid + 1 == modits_.size()) {
      return modits_[mid]->second.data();
    }
    auto base0 = modits_[mid]->first;
    auto base1 = modits_[mid + 1]->first;
    if (base0 <= target && target < base1) {
      // if target is between base0 and base1, we think it belongs to base0
      return modits_[mid]->second.data();
    }
    if (base0 > target) {
      // back forward
      high = mid - 1;
    } else {
      // go forward
      low = mid + 1;
    }
  }
  return "";
}

void ModuleLoader::cacheObject(std::shared_ptr<Object> imod) {
  if (mhandles_.find(imod->path().data()) != mhandles_.end())
    return;
  imods_.push_back(imod);
  mhandleits_.push_back(
      mhandles_
          .insert({imod->path().data(), reinterpret_cast<void *>(imod.get())})
          .first);
}

void Loader::initialize() {
  if (!moloader)
    moloader = std::make_unique<ModuleLoader>();
}

void Loader::deinitialize(int exitcode) {
  if (!moloader)
    return;
  moloader->cacheAndClean(exitcode);
}

Loader::Loader(Object *object, const std::vector<std::string> &deps)
    : object_(object) {
  for (auto &m : deps) {
    moloader->loadLibrary(m);
  }
}

Loader::Loader(std::string_view module) {
  handle_ = moloader->loadLibrary(module);
}

Loader::~Loader() {}

bool Loader::valid() { return object_ || handle_; }

const void *Loader::locate(std::string_view name, bool data) {
  return moloader->resolve(handle_, name, data);
}

const void *Loader::locateSymbol(std::string_view name, bool data) {
  return moloader->resolve(name, data);
}

std::string Loader::locateModule(const void *addr, bool update) {
  return moloader->find(addr, update);
}

void Loader::cacheObject(std::shared_ptr<Object> imod) {
  moloader->cacheObject(imod);
}

void Loader::cacheSymbol(std::string_view name, const void *impl) {
  moloader->cacheSymbol(name, impl);
}

bool Loader::executable(uint64_t vm, Object **iobject) {
  return moloader->executable(vm, iobject);
}

bool Loader::belong(uint64_t vm) { return moloader->belong(vm); }

bool Loader::globalLocal(uint64_t vm) {
  for (auto loc : global_locals) {
    if (vm == reinterpret_cast<uint64_t>(loc))
      return true;
  }
  return false;
}

const int *Loader::simulateTlsEpoch() {
#if ON_WINDOWS
  return &_init_thread_epoch;
#else
  return nullptr;
#endif
}

} // namespace icpp
