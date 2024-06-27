/* Interpreting C++, executing the source and executable like a script */
/* By Jesse Liu < neoliu2011@gmail.com >, 2024 */
/* Copyright (c) vpand.com 2024. This file is released under LGPL2.
   See LICENSE in root directory for more details
*/

#include "object.h"
#include "icpp.h"
#include "loader.h"
#include "utils.h"
#include <boost/beast.hpp>
#include <fstream>
#include <icppiobj.pb.h>
#include <iostream>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Support/MemoryBuffer.h>

namespace icpp {

Object::Object(std::string_view srcpath, std::string_view path)
    : srcpath_(srcpath), path_(path) {}

const char *Object::triple() {
  switch (type_) {
  case ELF_Reloc:
  case ELF_Exe:
    switch (arch_) {
    case AArch64:
      return "aarch64-none-linux-android";
    case X86_64:
      return "x86_64-none-linux-android";
    default:
      return "";
    }
  case MachO_Reloc:
  case MachO_Exe:
    switch (arch_) {
    case AArch64:
      return "arm64-apple-macosx";
    case X86_64:
      return "x86_64-apple-macosx";
    default:
      return "";
    }
  case COFF_Reloc:
  case COFF_Exe:
    switch (arch_) {
    case X86_64:
      return "x86_64-pc-windows-msvc";
    default:
      return "";
    }
  default:
    return "";
  }
}

void Object::createObject(ObjectType type) {
  // herein we pass IsVolatile as true to disable llvm to mmap this file
  // because some data sections may be modified at runtime
  auto errBuff = llvm::MemoryBuffer::getFile(path_, false, true, true);
  if (!errBuff) {
    std::cout << "Failed to read '" << path_
              << "': " << errBuff.getError().message() << std::endl;
    return;
  }
  fbuf_ = std::move(errBuff.get());
  auto buffRef = llvm::MemoryBufferRef(*fbuf_);
  auto expObj = CObjectFile::createObjectFile(buffRef);
  if (expObj) {
    type_ = type;
    ofile_ = std::move(expObj.get());
    switch (ofile_->getArch()) {
    case llvm::Triple::aarch64:
      arch_ = AArch64;
      break;
    case llvm::Triple::x86_64:
      arch_ = X86_64;
      break;
    default:
      arch_ = Unsupported;
      break;
    }
    parseSections();
    parseSymbols();
    decodeInsns();
  } else {
    std::cout << "Failed to create llvm object: "
              << llvm::toString(std::move(expObj.takeError())) << std::endl;
  }
}

void Object::parseSymbols() {
  using SymbolRef = llvm::object::SymbolRef;
  for (auto sym : ofile_->symbols()) {
    auto expType = sym.getType();
    if (!expType)
      continue;
    std::unordered_map<std::string, const void *> *caches = nullptr;
    switch (expType.get()) {
    case SymbolRef::ST_Data:
      caches = &datas_;
      break;
    case SymbolRef::ST_Function:
      caches = &funcs_;
      break;
    default:
      break;
    }
    if (!caches)
      continue;
    auto expFlags = sym.getFlags();
    if (!expFlags)
      continue;
    auto flags = expFlags.get();
    if ((flags & SymbolRef::SF_Undefined) || (flags & SymbolRef::SF_Common) ||
        (flags & SymbolRef::SF_Indirect) ||
        (flags & SymbolRef::SF_FormatSpecific)) {
      continue;
    }
    auto expSect = sym.getSection();
    auto expAddr = sym.getAddress();
    auto expName = sym.getName();
    if (!expSect || !expAddr || !expName)
      continue;
    auto sect = expSect.get();
    auto expSContent = sect->getContents();
    if (!expSContent)
      continue;
    auto saddr = sect->getAddress();
    auto sbuff = expSContent.get();
    auto snameExp = sect->getName();
    if (!snameExp)
      continue;
    auto addr = expAddr.get();
    auto name = expName.get();
    auto buff = sbuff.data() + addr - saddr;
    for (auto &ds : dynsects_) {
      if (snameExp.get() == ds.name) {
        // dynamically allocated section
        buff = ds.buffer.data() + addr - saddr;
        break;
      }
    }
    // ignore the internal temporary symbols
    if (name.starts_with("ltmp") || name.starts_with("l_."))
      continue;
    caches->insert({name.data(), buff});
    if (0) {
      log_print(Develop, "Cached symbol {}.{:x}.{}.", name.data(), addr,
                static_cast<const void *>(buff));
    }
  }
}

std::string_view Object::textSectName() {
  std::string_view textname(".text");
  if (ofile_->isMachO()) {
    textname = "__text";
  }
  return textname;
}

void Object::parseSections() {
  auto textname = textSectName();
  textsecti_ = 0;
  for (auto &s : ofile_->sections()) {
    auto expName = s.getName();
    if (!expName) {
      if (!textsz_)
        textsecti_++;
      continue;
    }

    auto name = expName.get();
    if (textname == name.data()) {
      auto expContent = s.getContents();
      if (!expContent) {
        log_print(Runtime, "Empty object file, there's no {} section.",
                  textname);
        break;
      }
      textsz_ = s.getSize();
      textrva_ = s.getAddress();
      textvm_ = reinterpret_cast<uint64_t>(expContent->data());
      log_print(Develop, "Text rva={:x}, vm={:x}.", textrva_, textvm_);
    } else if (name.ends_with("bss") || name.ends_with("common")) {
      dynsects_.push_back(
          {name.data(), s.getAddress(), std::string(s.getSize(), 0)});
    }

    if (!textsz_)
      textsecti_++;
  }
}

uc_arch Object::ucArch() {
  switch (arch_) {
  case AArch64:
    return UC_ARCH_ARM64;
  case X86_64:
    return UC_ARCH_X86;
  default:
    return UC_ARCH_MAX; // unsupported
  }
}

uc_mode Object::ucMode() {
  switch (arch_) {
  case X86_64:
    return UC_MODE_64;
  default:
    return UC_MODE_LITTLE_ENDIAN;
  }
}

const void *Object::mainEntry() {
  auto found = funcs_.find("_main");
  if (found == funcs_.end()) {
    found = funcs_.find("main");
  }
  if (found == funcs_.end()) {
    return nullptr;
  }
  return found->second;
}

const InsnInfo *Object::insnInfo(uint64_t vm) {
  auto rva = static_cast<uint32_t>(vm2rva(vm));
  // find insninfo related to this vm address
  auto found =
      std::lower_bound(iinfs_.begin(), iinfs_.end(), InsnInfo{.rva = rva});
  if (found == iinfs_.end()) {
    log_print(Runtime, "Failed to find instruction information of rva {:x}.",
              rva);
    abort();
  }
  return &*found;
}

std::string Object::generateCache() {
  namespace iobj = com::vpand::icppiobj;
  namespace base64 = boost::beast::detail::base64;

  // construct the iobj file
  iobj::InterpObject iobject;
  iobject.set_magic(iobj_magic);
  iobject.set_version(version_value().value);
  iobject.set_arch(static_cast<iobj::ArchType>(arch_));
  iobject.set_otype(static_cast<iobj::ObjectType>(type_));

  auto iins = iobject.mutable_instinfos();
  for (auto &i : iinfs_) {
    iins->Add(*reinterpret_cast<uint64_t *>(&i));
  }
  auto imetas = iobject.mutable_instmetas();
  for (auto &m : idecinfs_) {
    // as we use string as protobuf's map key, so we have to encode it as a real
    // string, otherwise its serialization will warn
    auto keysz = base64::encoded_size(m.first.length());
    std::string tmpkey(keysz, '\0');
    keysz = base64::encode(
        reinterpret_cast<void *>(const_cast<char *>(tmpkey.data())),
        m.first.data(), m.first.length());
    imetas->insert({std::string(tmpkey.data(), keysz), m.second});
  }
  auto irefs = iobject.mutable_irefsyms();
  Loader::locateModule("", true); // update loader's module list
  for (auto &r : irelocs_) {
    auto mod = cover(reinterpret_cast<uint64_t>(r.target))
                   ? ""
                   : Loader::locateModule(r.target);
    if (mod.size()) {
      auto found = irefs->find(mod);
      if (found == irefs->end()) {
        found = irefs->insert({mod.data(), iobj::SymbolList()}).first;
      }
      found->second.mutable_names()->Add(r.name.data());
    } else {
      mod = "self";
      auto found = irefs->find(mod);
      if (found == irefs->end()) {
        found = irefs->insert({mod.data(), iobj::SymbolList()}).first;
      }
      found->second.mutable_rvas()->Add(reinterpret_cast<uint64_t>(r.target) -
                                        textvm_);
    }
  }
  iobject.set_objbuf(
      std::string(fbuf_.get()->getBufferStart(), fbuf_.get()->getBufferSize()));

  // save to io file
  auto srcpath = fs::path(srcpath_);
  auto cachepath =
      (srcpath.parent_path() / (srcpath.stem().string() + ".io")).string();
  std::ofstream fout(cachepath, std::ios::binary);
  if (fout.is_open()) {
    iobject.SerializeToOstream(&fout);
    log_print(Develop, "Cached the interpretable object {}: ", cachepath);
  } else {
    log_print(Runtime, "Failed to create interpretable object {}: {}.",
              cachepath, std::strerror(errno));
  }
  return cachepath;
}

Object::~Object() {
  auto filebuff = fbuf_->getBuffer();
  if (*reinterpret_cast<const uint32_t *>(filebuff.data()) == iobj_magic) {
    // it's already an iobj file
    return;
  }
  // generate iobj file
  generateCache();
}

MachOObject::MachOObject(std::string_view srcpath, std::string_view path)
    : Object(srcpath, path) {}

MachOObject::~MachOObject() {}

MachORelocObject::MachORelocObject(std::string_view srcpath,
                                   std::string_view path)
    : MachOObject(srcpath, path) {
  createObject(MachO_Reloc);
}

MachORelocObject::~MachORelocObject() {}

MachOExeObject::MachOExeObject(std::string_view srcpath, std::string_view path)
    : MachOObject(srcpath, path) {
  createObject(MachO_Exe);
}

MachOExeObject::~MachOExeObject() {}

ELFObject::ELFObject(std::string_view srcpath, std::string_view path)
    : Object(srcpath, path) {}

ELFObject::~ELFObject() {}

ELFRelocObject::ELFRelocObject(std::string_view srcpath, std::string_view path)
    : ELFObject(srcpath, path) {
  createObject(ELF_Reloc);
}

ELFRelocObject::~ELFRelocObject() {}

ELFExeObject::ELFExeObject(std::string_view srcpath, std::string_view path)
    : ELFObject(srcpath, path) {
  createObject(ELF_Exe);
}

ELFExeObject::~ELFExeObject() {}

COFFObject::COFFObject(std::string_view srcpath, std::string_view path)
    : Object(srcpath, path) {}

COFFObject::~COFFObject() {}

COFFRelocObject::COFFRelocObject(std::string_view srcpath,
                                 std::string_view path)
    : COFFObject(srcpath, path) {
  createObject(COFF_Reloc);
}

COFFRelocObject::~COFFRelocObject() {}

COFFExeObject::COFFExeObject(std::string_view srcpath, std::string_view path)
    : COFFObject(srcpath, path) {
  createObject(COFF_Exe);
}

COFFExeObject::~COFFExeObject() {}

InterpObject::InterpObject(std::string_view srcpath, std::string_view path)
    : Object(srcpath, path) {
  namespace iobj = com::vpand::icppiobj;
  namespace base64 = boost::beast::detail::base64;

  // herein we pass IsVolatile as true to disable llvm to mmap this file
  // because some data sections may be modified at runtime
  auto errBuff = llvm::MemoryBuffer::getFile(path_, false, true, true);
  if (!errBuff) {
    std::cout << "Failed to read '" << path_
              << "': " << errBuff.getError().message() << std::endl;
    return;
  }
  if (*reinterpret_cast<const uint32_t *>(errBuff.get()->getBufferStart()) !=
      iobj_magic) {
    // it isn't a icpp interpretable object file
    return;
  }
  if (*reinterpret_cast<const uint32_t *>(errBuff.get()->getBufferStart() +
                                          4) != version_value().value) {
    log_print(Runtime,
              "The file {} does be an icpp interpretable object, but its "
              "version doesn't match this icpp (expected {}).",
              path_, version_string());
    return;
  }

  // construct the object instance
  iobj::InterpObject iobject;
  if (!iobject.ParseFromArray(errBuff.get()->getBufferStart(),
                              errBuff.get()->getBufferSize())) {
    log_print(Runtime, "Can't load the file {}, it's corrupted.", path_);
    return;
  }
  fbuf_ = std::move(errBuff.get());
  auto obuf = iobject.objbuf();
  auto buffRef =
      llvm::MemoryBufferRef(llvm::StringRef(obuf), llvm::StringRef(path));
  auto expObj = CObjectFile::createObjectFile(buffRef);
  if (!expObj) {
    std::cout << "Failed to create llvm object: "
              << llvm::toString(std::move(expObj.takeError())) << std::endl;
    return;
  }
  ofile_ = std::move(expObj.get());
  arch_ = static_cast<ArchType>(iobject.arch());
  type_ = static_cast<ObjectType>(iobject.otype());

  // parse from original object
  parseSections();
  parseSymbols();

  auto iins = iobject.instinfos();
  for (auto &i : iins) {
    iinfs_.push_back(*reinterpret_cast<InsnInfo *>(&i));
  }
  auto imetas = iobject.instmetas();
  for (auto &m : imetas) {
    std::string tmpkey(base64::decoded_size(m.first.length()), '\0');
    auto decret =
        base64::decode(tmpkey.data(), m.first.data(), m.first.length());
    idecinfs_.insert({std::string(tmpkey.data(), decret.first), m.second});
  }
  auto irefs = iobject.irefsyms();
  for (auto &r : irefs) {
    if (r.second.rvas().size()) {
      for (auto rva : r.second.rvas()) {
        irelocs_.push_back(
            RelocInfo{"self", reinterpret_cast<void *>(rva + textrva_)});
      }
      continue;
    }
    // dependent module
    Loader loader(r.first);
    if (loader.valid()) {
      for (auto &s : r.second.names()) {
        auto target = loader.locate(s);
        if (!target)
          target = Loader::locateSymbol(s, false);
        if (!target) {
          log_print(Runtime, "Can't resolve dependent symbol {}.", s);
          std::exit(-1);
        }
        irelocs_.push_back(RelocInfo{s, target});
      }
    } else {
      log_print(Runtime, "Can't load dependent module {}.", r.first);
      std::exit(-1);
    }
  }
}

InterpObject::~InterpObject() {}

} // namespace icpp
