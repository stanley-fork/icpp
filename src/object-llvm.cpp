/* Interpreting C++, executing the source and executable like a script */
/* By Jesse Liu < neoliu2011@gmail.com >, 2024 */
/* Copyright (c) vpand.com 2024. This file is released under LGPL2.
   See LICENSE in root directory for more details
*/

// The disassembler related stuff is modified from llvm-objdump.

//===-- llvm-objdump.cpp - Object file dumping utility for llvm -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This program is a utility that works like binutils "objdump", that is, it
// dumps out a plethora of information about an object file depending on the
// flags.
//
// The flags and output of this program should be near identical to those of
// binutils objdump.
//
//===----------------------------------------------------------------------===//

#include "SourcePrinter.h"
#include "arch.h"
#include "llvm-objdump.h"
#include "loader.h"
#include "log.h"
#include "object.h"
#include "runcfg.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SetOperations.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/Twine.h"
#include "llvm/BinaryFormat/COFF.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/BinaryFormat/MachO.h"
#include "llvm/BinaryFormat/Wasm.h"
#include "llvm/DebugInfo/BTF/BTFParser.h"
#include "llvm/DebugInfo/DWARF/DWARFContext.h"
#include "llvm/DebugInfo/Symbolize/SymbolizableModule.h"
#include "llvm/DebugInfo/Symbolize/Symbolize.h"
#include "llvm/Debuginfod/BuildIDFetcher.h"
#include "llvm/Debuginfod/Debuginfod.h"
#include "llvm/Debuginfod/HTTPClient.h"
#include "llvm/Demangle/Demangle.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCDisassembler/MCDisassembler.h"
#include "llvm/MC/MCDisassembler/MCRelocationInfo.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstPrinter.h"
#include "llvm/MC/MCInstrAnalysis.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCObjectFileInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCTargetOptions.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Object/Archive.h"
#include "llvm/Object/BuildID.h"
#include "llvm/Object/COFF.h"
#include "llvm/Object/COFFImportFile.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Object/ELFTypes.h"
#include "llvm/Object/FaultMapParser.h"
#include "llvm/Object/MachO.h"
#include "llvm/Object/MachOUniversal.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Object/OffloadBinary.h"
#include "llvm/Object/Wasm.h"
#include "llvm/Option/Arg.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Option/Option.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/GraphWriter.h"
#include "llvm/Support/LLVMDriver.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/StringSaver.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <optional>
#include <set>
#include <system_error>
#include <unicorn/unicorn.h>
#include <unordered_map>
#include <utility>

#if ICPP_CROSS_GADGET
#define ICPP_HAS_AARCH64 ARCH_ARM64
#define ICPP_HAS_X64 ARCH_X64
#else
#define ICPP_HAS_AARCH64 1
#define ICPP_HAS_X64 1
#endif

using namespace llvm;
using namespace llvm::object;
using namespace llvm::objdump;
using namespace llvm::opt;

constexpr const char *ToolName = "icpp";

bool objdump::ArchiveHeaders = false;
bool objdump::Demangle = true;
bool objdump::Disassemble = true;
bool objdump::DisassembleAll = false;
bool objdump::SymbolDescription = true;
bool objdump::TracebackTable = true;
bool objdump::SectionContents = false;
bool objdump::PrintLines = true;
bool objdump::ShowRawInsn = true;
bool objdump::LeadingAddr = true;
bool objdump::Relocations = true;
bool objdump::PrintImmHex = true;
bool objdump::PrivateHeaders = true;
bool objdump::SectionHeaders = true;
bool objdump::PrintSource = true;
bool objdump::SymbolTable = true;
bool objdump::UnwindInfo = true;
std::string objdump::Prefix;
uint32_t objdump::PrefixStrip;
int objdump::DbgIndent = 52;
DebugVarsFormat objdump::DbgVariables = DVDisabled;

void objdump::reportWarning(const Twine &Message, StringRef File) {
  // Output order between errs() and outs() matters especially for archive
  // files where the output is per member object.
  outs().flush();
  WithColor::warning(errs(), ToolName)
      << "'" << File << "': " << Message << "\n";
}

[[noreturn]] void objdump::reportError(StringRef File, const Twine &Message) {
  outs().flush();
  WithColor::error(errs(), ToolName) << "'" << File << "': " << Message << "\n";
  exit(1);
}

[[noreturn]] void objdump::reportError(Error E, StringRef FileName,
                                       StringRef ArchiveName,
                                       StringRef ArchitectureName) {
  assert(E);
  outs().flush();
  WithColor::error(errs(), ToolName);
  if (ArchiveName != "")
    errs() << ArchiveName << "(" << FileName << ")";
  else
    errs() << "'" << FileName << "'";
  if (!ArchitectureName.empty())
    errs() << " (for architecture " << ArchitectureName << ")";
  errs() << ": ";
  logAllUnhandledErrors(std::move(E), errs());
  exit(1);
}

/// Get the column at which we want to start printing the instruction
/// disassembly, taking into account anything which appears to the left of it.
unsigned objdump::getInstStartColumn(const MCSubtargetInfo &STI) {
  return !ShowRawInsn ? 16 : STI.getTargetTriple().isX86() ? 40 : 24;
}

namespace icpp {

static const char *archName(const ObjectFile *Obj) {
  switch (Obj->getArch()) {
  case Triple::aarch64:
    return "aarch64";
  case Triple::x86_64:
    return "x86-64";
  default:
    return "";
  }
}

static const Target *getTarget(const ObjectFile *Obj, std::string &TripleName) {
  static bool init_llvm = false;
  if (!init_llvm) {
    init_llvm = true;
    // Initialize All target infos
#define init_target(name)                                                      \
  LLVMInitialize##name##Target();                                              \
  LLVMInitialize##name##TargetMC();                                            \
  LLVMInitialize##name##TargetInfo();                                          \
  LLVMInitialize##name##AsmPrinter();                                          \
  LLVMInitialize##name##AsmParser();                                           \
  LLVMInitialize##name##Disassembler();
#if ICPP_HAS_AARCH64
    init_target(AArch64);
#endif
#if ICPP_HAS_X64
    init_target(X86);
#endif
  }

  // Figure out the target triple.
  Triple TheTriple("unknown-unknown-unknown");
  if (TripleName.empty()) {
    TheTriple = Obj->makeTriple();
  } else {
    TheTriple.setTriple(Triple::normalize(TripleName));
    auto Arch = Obj->getArch();
    if (Arch == Triple::arm || Arch == Triple::armeb)
      Obj->setARMSubArch(TheTriple);
  }

  // Get the target specific parser.
  std::string Error;
  const Target *TheTarget =
      TargetRegistry::lookupTarget(archName(Obj), TheTriple, Error);
  if (!TheTarget)
    reportError(Obj->getFileName(), "can't find target: " + Error);

  // Update the triple name and return the found target.
  TripleName = TheTriple.getTriple();
  return TheTarget;
}

class DisassemblerTarget {
public:
  const Target *TheTarget;
  std::unique_ptr<const MCSubtargetInfo> SubtargetInfo;
  std::shared_ptr<MCContext> Context;
  std::unique_ptr<MCDisassembler> DisAsm;
  std::shared_ptr<MCInstrAnalysis> InstrAnalysis;
  std::shared_ptr<MCInstPrinter> InstPrinter;

  DisassemblerTarget(const Target *TheTarget, ObjectFile &Obj,
                     StringRef TripleName, StringRef MCPU,
                     SubtargetFeatures &Features);
  DisassemblerTarget(DisassemblerTarget &Other, StringRef TripleName,
                     StringRef MCPU, SubtargetFeatures &Features);

private:
  MCTargetOptions Options;
  std::shared_ptr<const MCRegisterInfo> RegisterInfo;
  std::shared_ptr<const MCAsmInfo> AsmInfo;
  std::shared_ptr<const MCInstrInfo> InstrInfo;
  std::shared_ptr<MCObjectFileInfo> ObjectFileInfo;
};

DisassemblerTarget::DisassemblerTarget(const Target *TheTarget, ObjectFile &Obj,
                                       StringRef TripleName, StringRef MCPU,
                                       SubtargetFeatures &Features)
    : TheTarget(TheTarget),
      RegisterInfo(TheTarget->createMCRegInfo(TripleName)) {
  if (!RegisterInfo)
    reportError(Obj.getFileName(), "no register info for target " + TripleName);

  // Set up disassembler.
  AsmInfo.reset(TheTarget->createMCAsmInfo(*RegisterInfo, TripleName, Options));
  if (!AsmInfo)
    reportError(Obj.getFileName(), "no assembly info for target " + TripleName);

  SubtargetInfo.reset(
      TheTarget->createMCSubtargetInfo(TripleName, MCPU, Features.getString()));
  if (!SubtargetInfo)
    reportError(Obj.getFileName(),
                "no subtarget info for target " + TripleName);
  InstrInfo.reset(TheTarget->createMCInstrInfo());
  if (!InstrInfo)
    reportError(Obj.getFileName(),
                "no instruction info for target " + TripleName);
  Context =
      std::make_shared<MCContext>(Triple(TripleName), AsmInfo.get(),
                                  RegisterInfo.get(), SubtargetInfo.get());

  // FIXME: for now initialize MCObjectFileInfo with default values
  ObjectFileInfo.reset(
      TheTarget->createMCObjectFileInfo(*Context, /*PIC=*/false));
  Context->setObjectFileInfo(ObjectFileInfo.get());

  DisAsm.reset(TheTarget->createMCDisassembler(*SubtargetInfo, *Context));
  if (!DisAsm)
    reportError(Obj.getFileName(), "no disassembler for target " + TripleName);

  if (auto *ELFObj = dyn_cast<ELFObjectFileBase>(&Obj))
    DisAsm->setABIVersion(ELFObj->getEIdentABIVersion());

  InstrAnalysis.reset(TheTarget->createMCInstrAnalysis(InstrInfo.get()));

  int AsmPrinterVariant = AsmInfo->getAssemblerDialect();
  InstPrinter.reset(TheTarget->createMCInstPrinter(Triple(TripleName),
                                                   AsmPrinterVariant, *AsmInfo,
                                                   *InstrInfo, *RegisterInfo));
  if (!InstPrinter)
    reportError(Obj.getFileName(),
                "no instruction printer for target " + TripleName);
  InstPrinter->setPrintImmHex(PrintImmHex);
  InstPrinter->setPrintBranchImmAsAddress(true);
  InstPrinter->setMCInstrAnalysis(InstrAnalysis.get());
}

DisassemblerTarget::DisassemblerTarget(DisassemblerTarget &Other,
                                       StringRef TripleName, StringRef MCPU,
                                       SubtargetFeatures &Features)
    : TheTarget(Other.TheTarget),
      SubtargetInfo(TheTarget->createMCSubtargetInfo(TripleName, MCPU,
                                                     Features.getString())),
      Context(Other.Context),
      DisAsm(TheTarget->createMCDisassembler(*SubtargetInfo, *Context)),
      InstrAnalysis(Other.InstrAnalysis), InstPrinter(Other.InstPrinter),
      RegisterInfo(Other.RegisterInfo), AsmInfo(Other.AsmInfo),
      InstrInfo(Other.InstrInfo), ObjectFileInfo(Other.ObjectFileInfo) {}

void ObjectDisassembler::init(CObjectFile *Obj, std::string_view Triple) {
  std::string TripleName(Triple);
  const Target *TheTarget = getTarget(Obj, TripleName);
  std::string MCPU;
  std::vector<std::string> MAttrs;

  // Package up features to be passed to target/subtarget
  Expected<SubtargetFeatures> FeaturesValue = Obj->getFeatures();
  if (!FeaturesValue)
    reportError(FeaturesValue.takeError(), Obj->getFileName());
  SubtargetFeatures Features = *FeaturesValue;
  if (!MAttrs.empty()) {
    for (unsigned I = 0; I != MAttrs.size(); ++I)
      Features.AddFeature(MAttrs[I]);
  } else if (MCPU.empty() && Obj->getArch() == llvm::Triple::aarch64) {
    Features.AddFeature("+all");
  }

  if (MCPU.empty())
    MCPU = Obj->tryGetCPUName().value_or("").str();

  DT = new DisassemblerTarget(TheTarget, *Obj, TripleName, MCPU, Features);
  SP = new SourcePrinter(Obj, TheTarget->getName());
}

ObjectDisassembler::~ObjectDisassembler() {
  delete DT;
  delete SP;
}

std::string Object::sourceInfo(uint64_t vm) {
  uint64_t sindex = -1;
  uint64_t saddr = 0;
  for (auto &s : ofile_->sections()) {
    auto expContent = s.getContents();
    if (!expContent)
      continue;
    auto start = reinterpret_cast<uint64_t>(expContent->data());
    if (start <= vm && vm < start + s.getSize()) {
      sindex = s.getIndex();
      saddr = s.getAddress() + vm - start;
      break;
    }
  }
  if (sindex == -1)
    return "";

  std::string Output;
  raw_string_ostream OS(Output);
  formatted_raw_ostream FOS(OS);
  auto SectAddr = object::SectionedAddress{saddr, sindex};
  LiveVariablePrinter LVP(*odiser_.DT->Context->getRegisterInfo(),
                          *odiser_.DT->SubtargetInfo);
  odiser_.SP->printSourceLine(FOS, SectAddr, ofile_->getFileName(), LVP);
  FOS.flush();
  return Output;
}

#if ICPP_HAS_AARCH64

#define GET_REGINFO_ENUM
#define GET_INSTRINFO_ENUM
#include "llvm/../../lib/Target/AArch64/AArch64GenInstrInfo.inc"
#include "llvm/../../lib/Target/AArch64/AArch64GenRegisterInfo.inc"

static uint16_t llvm2ucRegisterAArch64(unsigned reg) {
  namespace INSN = llvm::AArch64;

  // x
  if (INSN::X0 <= reg && reg <= INSN::X28)
    return UC_ARM64_REG_X0 + reg - INSN::X0;
  if (INSN::FP == reg)
    return UC_ARM64_REG_FP;
  if (INSN::LR == reg)
    return UC_ARM64_REG_LR;
  if (INSN::SP == reg)
    return UC_ARM64_REG_SP;
  // w
  if (INSN::W0 <= reg && reg <= INSN::W30)
    return UC_ARM64_REG_W0 + reg - INSN::W0;
  // s
  if (INSN::S0 <= reg && reg <= INSN::S31)
    return UC_ARM64_REG_S0 + reg - INSN::S0;
  // d
  if (INSN::D0 <= reg && reg <= INSN::D31)
    return UC_ARM64_REG_D0 + reg - INSN::D0;
  // b
  if (INSN::B0 <= reg && reg <= INSN::B31)
    return UC_ARM64_REG_B0 + reg - INSN::B0;
  // h
  if (INSN::H0 <= reg && reg <= INSN::H31)
    return UC_ARM64_REG_H0 + reg - INSN::H0;
  // q
  if (INSN::Q0 <= reg && reg <= INSN::Q31)
    return UC_ARM64_REG_Q0 + reg - INSN::Q0;
  // zero
  if (INSN::WZR == reg)
    return UC_ARM64_REG_WZR;
  if (INSN::XZR == reg)
    return UC_ARM64_REG_XZR;

  log_print(Runtime, "Unknown llvm instruction register operand type: {}.",
            reg);
  abort();
}

static void parseInstAArch64(const MCInst &inst, uint64_t opcptr,
                             std::map<std::string, std::string> &decinfo,
                             InsnInfo &iinfo) {
  namespace INSN = llvm::AArch64;
  switch (inst.getOpcode()) {
  case INSN::BRK:
    iinfo.type = INSN_ABORT;
    break;
  case INSN::TBZW:
  case INSN::TBZX:
  case INSN::TBNZW:
  case INSN::TBNZX:
  case INSN::CBZW:
  case INSN::CBZX:
  case INSN::CBNZW:
  case INSN::CBNZX:
  case INSN::Bcc:
    iinfo.type = INSN_CONDJUMP;
    break;
  case INSN::RET:
    iinfo.type = INSN_ARM64_RETURN;
    break;
  case INSN::B:
    iinfo.type = INSN_ARM64_JUMP;
    break;
  case INSN::BR:
    iinfo.type = INSN_ARM64_JUMPREG;
    break;
  case INSN::BL:
    iinfo.type = INSN_ARM64_CALL;
    break;
  case INSN::BLR:
    iinfo.type = INSN_ARM64_CALLREG;
    break;
  case INSN::SVC:
    iinfo.type = INSN_ARM64_SYSCALL;
    break;
  case INSN::ADR:
    iinfo.type = INSN_ARM64_ADR;
    break;
  case INSN::ADRP:
    iinfo.type = INSN_ARM64_ADRP;
    break;
  case INSN::LDRSWl:
    iinfo.type = INSN_ARM64_LDRSWL;
    break;
  case INSN::LDRWl:
    iinfo.type = INSN_ARM64_LDRWL;
    break;
  case INSN::LDRXl:
    iinfo.type = INSN_ARM64_LDRXL;
    break;
  case INSN::LDRSl:
    iinfo.type = INSN_ARM64_LDRSL;
    break;
  case INSN::LDRDl:
    iinfo.type = INSN_ARM64_LDRDL;
    break;
  case INSN::LDRQl:
    iinfo.type = INSN_ARM64_LDRQL;
    break;
  default:
    iinfo.type = INSN_HARDWARE;
    break;
  }
}

#endif // end of ICPP_HAS_AARCH64

#if ICPP_HAS_X64

#define GET_REGINFO_ENUM
#define GET_INSTRINFO_ENUM
#include "llvm/../../lib/Target/X86/X86GenInstrInfo.inc"
#include "llvm/../../lib/Target/X86/X86GenRegisterInfo.inc"

static uint16_t llvm2ucRegisterX64(unsigned reg) {
  namespace INSN = llvm::X86;

  if (INSN::AH == reg)
    return UC_X86_REG_AH;
  if (INSN::AL == reg)
    return UC_X86_REG_AL;
  if (INSN::AX == reg)
    return UC_X86_REG_AX;
  if (INSN::BH == reg)
    return UC_X86_REG_BH;
  if (INSN::BL == reg)
    return UC_X86_REG_BL;
  if (INSN::BP == reg)
    return UC_X86_REG_BP;
  if (INSN::BPL == reg)
    return UC_X86_REG_BPL;
  if (INSN::BX == reg)
    return UC_X86_REG_BX;
  if (INSN::CH == reg)
    return UC_X86_REG_CH;
  if (INSN::CL == reg)
    return UC_X86_REG_CL;
  if (INSN::CS == reg)
    return UC_X86_REG_CS;
  if (INSN::CX == reg)
    return UC_X86_REG_CX;
  if (INSN::DH == reg)
    return UC_X86_REG_DH;
  if (INSN::DI == reg)
    return UC_X86_REG_DI;
  if (INSN::DIL == reg)
    return UC_X86_REG_DIL;
  if (INSN::DL == reg)
    return UC_X86_REG_DL;
  if (INSN::DS == reg)
    return UC_X86_REG_DS;
  if (INSN::DX == reg)
    return UC_X86_REG_DX;
  if (INSN::EAX == reg)
    return UC_X86_REG_EAX;
  if (INSN::EBP == reg)
    return UC_X86_REG_EBP;
  if (INSN::EBX == reg)
    return UC_X86_REG_EBX;
  if (INSN::ECX == reg)
    return UC_X86_REG_ECX;
  if (INSN::EDI == reg)
    return UC_X86_REG_EDI;
  if (INSN::EDX == reg)
    return UC_X86_REG_EDX;
  if (INSN::EFLAGS == reg)
    return UC_X86_REG_EFLAGS;
  if (INSN::EIP == reg)
    return UC_X86_REG_EIP;
  if (INSN::ES == reg)
    return UC_X86_REG_ES;
  if (INSN::ESI == reg)
    return UC_X86_REG_ESI;
  if (INSN::ESP == reg)
    return UC_X86_REG_ESP;
  if (INSN::FPSW == reg)
    return UC_X86_REG_FPSW;
  if (INSN::FS == reg)
    return UC_X86_REG_FS;
  if (INSN::GS == reg)
    return UC_X86_REG_GS;
  if (INSN::IP == reg)
    return UC_X86_REG_IP;
  if (INSN::RAX == reg)
    return UC_X86_REG_RAX;
  if (INSN::RBP == reg)
    return UC_X86_REG_RBP;
  if (INSN::RBX == reg)
    return UC_X86_REG_RBX;
  if (INSN::RCX == reg)
    return UC_X86_REG_RCX;
  if (INSN::RDI == reg)
    return UC_X86_REG_RDI;
  if (INSN::RDX == reg)
    return UC_X86_REG_RDX;
  if (INSN::RIP == reg)
    return UC_X86_REG_RIP;
  if (INSN::RSI == reg)
    return UC_X86_REG_RSI;
  if (INSN::RSP == reg)
    return UC_X86_REG_RSP;
  if (INSN::SI == reg)
    return UC_X86_REG_SI;
  if (INSN::SIL == reg)
    return UC_X86_REG_SIL;
  if (INSN::SP == reg)
    return UC_X86_REG_SP;
  if (INSN::SPL == reg)
    return UC_X86_REG_SPL;
  if (INSN::SS == reg)
    return UC_X86_REG_SS;
  if (INSN::MM0 <= reg && reg <= INSN::MM7)
    return UC_X86_REG_MM0 + reg - INSN::MM0;
  if (INSN::R8 <= reg && reg <= INSN::R15)
    return UC_X86_REG_R8 + reg - INSN::R8;
  if (INSN::ST0 <= reg && reg <= INSN::ST7)
    return UC_X86_REG_ST0 + reg - INSN::ST0;
  if (INSN::XMM0 <= reg && reg <= INSN::XMM15)
    return UC_X86_REG_XMM0 + reg - INSN::XMM0;
  if (INSN::XMM16 <= reg && reg <= INSN::XMM31)
    return UC_X86_REG_XMM16 + reg - INSN::XMM16;
  if (INSN::YMM0 <= reg && reg <= INSN::YMM15)
    return UC_X86_REG_YMM0 + reg - INSN::YMM0;
  if (INSN::YMM16 <= reg && reg <= INSN::YMM31)
    return UC_X86_REG_YMM16 + reg - INSN::YMM16;
  if (INSN::ZMM0 <= reg && reg <= INSN::ZMM31)
    return UC_X86_REG_ZMM0 + reg - INSN::ZMM0;
  if (INSN::R8B == reg)
    return UC_X86_REG_R8B;
  if (INSN::R9B == reg)
    return UC_X86_REG_R9B;
  if (INSN::R10B == reg)
    return UC_X86_REG_R10B;
  if (INSN::R11B == reg)
    return UC_X86_REG_R11B;
  if (INSN::R12B == reg)
    return UC_X86_REG_R12B;
  if (INSN::R13B == reg)
    return UC_X86_REG_R13B;
  if (INSN::R14B == reg)
    return UC_X86_REG_R14B;
  if (INSN::R15B == reg)
    return UC_X86_REG_R15B;
  if (INSN::R8D == reg)
    return UC_X86_REG_R8D;
  if (INSN::R9D == reg)
    return UC_X86_REG_R9D;
  if (INSN::R10D == reg)
    return UC_X86_REG_R10D;
  if (INSN::R11D == reg)
    return UC_X86_REG_R11D;
  if (INSN::R12D == reg)
    return UC_X86_REG_R12D;
  if (INSN::R13D == reg)
    return UC_X86_REG_R13D;
  if (INSN::R14D == reg)
    return UC_X86_REG_R14D;
  if (INSN::R15D == reg)
    return UC_X86_REG_R15D;
  if (INSN::R8W == reg)
    return UC_X86_REG_R8W;
  if (INSN::R9W == reg)
    return UC_X86_REG_R9W;
  if (INSN::R10W == reg)
    return UC_X86_REG_R10W;
  if (INSN::R11W == reg)
    return UC_X86_REG_R11W;
  if (INSN::R12W == reg)
    return UC_X86_REG_R12W;
  if (INSN::R13W == reg)
    return UC_X86_REG_R13W;
  if (INSN::R14W == reg)
    return UC_X86_REG_R14W;
  if (INSN::R15W == reg)
    return UC_X86_REG_R15W;
  if (INSN::EFLAGS == reg)
    return UC_X86_REG_RFLAGS;
  switch (reg) {
  case INSN::NoRegister:
  case INSN::EIZ:
  case INSN::RIZ:
    // reuse dr7 as a zero register converted from llvm register
    return UC_X86_REG_DR7;
  default:
    break;
  }

  log_print(Runtime, "Unknown llvm instruction register operand type: {}.",
            reg);
  abort();
}

static void parseInstX64(MCInst &inst, uint64_t opcptr,
                         std::map<std::string, std::string> &decinfo,
                         InsnInfo &iinfo) {
#define add_jcond_opr(cond)                                                    \
  if (iinfo.len > 5) {                                                         \
    inst.addOperand(MCOperand::createImm(CONDT_##cond));                       \
  }
#define set_jcond_opr(cond)                                                    \
  {                                                                            \
    opr.setImm(CONDT_##cond);                                                  \
    break;                                                                     \
  }

  namespace INSN = llvm::X86;
  switch (inst.getOpcode()) {
  case INSN::INT:
  case INSN::INT3:
  case INSN::INTO:
  case INSN::TRAP:
    iinfo.type = INSN_ABORT;
    break;
  case INSN::JCC_1:
  case INSN::JCC_2:
  case INSN::JCC_4: {
    iinfo.type = iinfo.len > 5 ? INSN_X64_JUMPCOND : INSN_CONDJUMP;
    if (iinfo.len > 5) {
      MCOperand &opr = inst.getOperand(1);
      // convert llvm condition imm to icpp named condition code,
      // see llvm/lib/Target/X86/MCTargetDesc/X86InstPrinterCommon.cpp
      switch (opr.getImm()) {
      case 0:
        set_jcond_opr(jo);
      case 1:
        set_jcond_opr(jno);
      case 2:
        set_jcond_opr(jb);
      case 3:
        set_jcond_opr(jae);
      case 4:
        set_jcond_opr(je);
      case 5:
        set_jcond_opr(jne);
      case 6:
        set_jcond_opr(jbe);
      case 7:
        set_jcond_opr(ja);
      case 8:
        set_jcond_opr(js);
      case 9:
        set_jcond_opr(jns);
      case 0xa:
        set_jcond_opr(jp);
      case 0xb:
        set_jcond_opr(jnp);
      case 0xc:
        set_jcond_opr(jl);
      case 0xd:
        set_jcond_opr(jge);
      case 0xe:
        set_jcond_opr(jle);
      case 0xf:
        set_jcond_opr(jg);
      default:
        log_print(Develop, "Out of condition code range: {}.", opr.getImm());
        break;
      }
    }
    break;
  }
  case INSN::JRCXZ:
    iinfo.type = iinfo.len > 5 ? INSN_X64_JUMPCOND : INSN_CONDJUMP;
    add_jcond_opr(jrcxz);
    break;
  case INSN::JECXZ:
    iinfo.type = iinfo.len > 5 ? INSN_X64_JUMPCOND : INSN_CONDJUMP;
    add_jcond_opr(jecxz);
    break;
  case INSN::RET:
  case INSN::RET16:
  case INSN::RET32:
  case INSN::RET64:
    iinfo.type = INSN_X64_RETURN;
    break;
  case INSN::SYSCALL:
    iinfo.type = INSN_X64_SYSCALL;
    break;
  case INSN::CALLpcrel16:
  case INSN::CALLpcrel32:
  case INSN::CALL64pcrel32:
    iinfo.type = INSN_X64_CALL;
    break;
  case INSN::CALL16m:
  case INSN::CALL32m:
  case INSN::CALL64m:
    iinfo.type = INSN_X64_CALLMEM;
    break;
  case INSN::CALL16r:
  case INSN::CALL32r:
  case INSN::CALL64r:
    iinfo.type = INSN_X64_CALLREG;
    break;
  case INSN::JMP_1:
  case INSN::JMP_2:
  case INSN::JMP_4:
    iinfo.type = INSN_X64_JUMP;
    break;
  case INSN::JMP16m:
  case INSN::JMP32m:
  case INSN::JMP64m:
    iinfo.type = INSN_X64_JUMPMEM;
    break;
  case INSN::JMP16r:
  case INSN::JMP32r:
  case INSN::JMP64r:
    iinfo.type = INSN_X64_JUMPREG;
    break;
  case INSN::MOV64rm:
    iinfo.type = INSN_X64_MOV64RM;
    break;
  case INSN::MOV32rm:
    iinfo.type = INSN_X64_MOV32RM;
    break;
  case INSN::MOV16rm:
    iinfo.type = INSN_X64_MOV16RM;
    break;
  case INSN::MOV8rm:
    iinfo.type = INSN_X64_MOV8RM;
    break;
  case INSN::MOV64mr:
    iinfo.type = INSN_X64_MOV64MR;
    break;
  case INSN::MOV32mr:
    iinfo.type = INSN_X64_MOV32MR;
    break;
  case INSN::MOV16mr:
    iinfo.type = INSN_X64_MOV16MR;
    break;
  case INSN::MOV8mr:
    iinfo.type = INSN_X64_MOV8MR;
    break;
  case INSN::MOV64mi32:
    iinfo.type = INSN_X64_MOV64MI32;
    break;
  case INSN::MOV32mi:
    iinfo.type = INSN_X64_MOV32MI;
    break;
  case INSN::MOV16mi:
    iinfo.type = INSN_X64_MOV16MI;
    break;
  case INSN::MOV8mi:
    iinfo.type = INSN_X64_MOV8MI;
    break;
  case INSN::LEA32r:
    iinfo.type = INSN_X64_LEA32;
    break;
  case INSN::LEA64r:
    iinfo.type = INSN_X64_LEA64;
    break;
  case INSN::MOVAPSrm:
    iinfo.type = INSN_X64_MOVAPSRM;
    break;
  case INSN::MOVAPSmr:
    iinfo.type = INSN_X64_MOVAPSMR;
    break;
  case INSN::MOVUPSrm:
    iinfo.type = INSN_X64_MOVUPSRM;
    break;
  case INSN::MOVUPSmr:
    iinfo.type = INSN_X64_MOVUPSMR;
    break;
  case INSN::MOVAPDrm:
    iinfo.type = INSN_X64_MOVAPDRM;
    break;
  case INSN::MOVAPDmr:
    iinfo.type = INSN_X64_MOVAPDMR;
    break;
  case INSN::MOVUPDrm:
    iinfo.type = INSN_X64_MOVUPDRM;
    break;
  case INSN::MOVUPDmr:
    iinfo.type = INSN_X64_MOVUPDMR;
    break;
  case INSN::CMP8mi:
    iinfo.type = INSN_X64_CMP8MI;
    break;
  case INSN::CMP8mi8:
    iinfo.type = INSN_X64_CMP8MI8;
    break;
  case INSN::CMP16mi:
    iinfo.type = INSN_X64_CMP16MI;
    break;
  case INSN::CMP16mi8:
    iinfo.type = INSN_X64_CMP16MI8;
    break;
  case INSN::CMP32mi:
    iinfo.type = INSN_X64_CMP32MI;
    break;
  case INSN::CMP32mi8:
    iinfo.type = INSN_X64_CMP32MI8;
    break;
  case INSN::CMP64mi32:
    iinfo.type = INSN_X64_CMP64MI32;
    break;
  case INSN::CMP64mi8:
    iinfo.type = INSN_X64_CMP64MI8;
    break;
  case INSN::CMP8rm:
    iinfo.type = INSN_X64_CMP8RM;
    break;
  case INSN::CMP16rm:
    iinfo.type = INSN_X64_CMP16RM;
    break;
  case INSN::CMP32rm:
    iinfo.type = INSN_X64_CMP32RM;
    break;
  case INSN::CMP64rm:
    iinfo.type = INSN_X64_CMP64RM;
    break;
  case INSN::CMP8mr:
    iinfo.type = INSN_X64_CMP8MR;
    break;
  case INSN::CMP16mr:
    iinfo.type = INSN_X64_CMP16MR;
    break;
  case INSN::CMP32mr:
    iinfo.type = INSN_X64_CMP32MR;
    break;
  case INSN::CMP64mr:
    iinfo.type = INSN_X64_CMP64MR;
    break;
  case INSN::MOVSX16rm8:
    iinfo.type = INSN_X64_MOVSX16RM8;
    break;
  case INSN::MOVSX16rm16:
    iinfo.type = INSN_X64_MOVSX16RM16;
    break;
  case INSN::MOVSX16rm32:
    iinfo.type = INSN_X64_MOVSX16RM32;
    break;
  case INSN::MOVSX32rm8:
    iinfo.type = INSN_X64_MOVSX32RM8;
    break;
  case INSN::MOVSX32rm16:
    iinfo.type = INSN_X64_MOVSX32RM16;
    break;
  case INSN::MOVSX32rm32:
    iinfo.type = INSN_X64_MOVSX32RM32;
    break;
  case INSN::MOVSX64rm8:
    iinfo.type = INSN_X64_MOVSX64RM8;
    break;
  case INSN::MOVSX64rm16:
    iinfo.type = INSN_X64_MOVSX64RM16;
    break;
  case INSN::MOVSX64rm32:
    iinfo.type = INSN_X64_MOVSX64RM32;
    break;
  case INSN::MOVZX16rm8:
    iinfo.type = INSN_X64_MOVZX16RM8;
    break;
  case INSN::MOVZX16rm16:
    iinfo.type = INSN_X64_MOVZX16RM16;
    break;
  case INSN::MOVZX32rm8:
    iinfo.type = INSN_X64_MOVZX32RM8;
    break;
  case INSN::MOVZX32rm16:
    iinfo.type = INSN_X64_MOVZX32RM16;
    break;
  case INSN::MOVZX64rm8:
    iinfo.type = INSN_X64_MOVZX64RM8;
    break;
  case INSN::MOVZX64rm16:
    iinfo.type = INSN_X64_MOVZX64RM16;
    break;
  case INSN::TEST8mi:
    iinfo.type = INSN_X64_TEST8MI;
    break;
  case INSN::TEST8mr:
    iinfo.type = INSN_X64_TEST8MR;
    break;
  case INSN::TEST16mi:
    iinfo.type = INSN_X64_TEST16MI;
    break;
  case INSN::TEST16mr:
    iinfo.type = INSN_X64_TEST16MR;
    break;
  case INSN::TEST32mi:
    iinfo.type = INSN_X64_TEST32MI;
    break;
  case INSN::TEST32mr:
    iinfo.type = INSN_X64_TEST32MR;
    break;
  case INSN::TEST64mi32:
    iinfo.type = INSN_X64_TEST64MI32;
    break;
  case INSN::TEST64mr:
    iinfo.type = INSN_X64_TEST64MR;
    break;
  case INSN::CMOV16rm:
  case INSN::CMOV16rm_ND:
    iinfo.type = INSN_X64_CMOV16RM;
    break;
  case INSN::CMOV32rm:
  case INSN::CMOV32rm_ND:
    iinfo.type = INSN_X64_CMOV32RM;
    break;
  case INSN::CMOV64rm:
  case INSN::CMOV64rm_ND:
    iinfo.type = INSN_X64_CMOV64RM;
    break;
  default:
    iinfo.type = INSN_HARDWARE;
    return;
  }
  for (unsigned i = 0; i < inst.getNumOperands(); i++) {
    auto opr = inst.getOperand(i);
    if (opr.isReg()) {
      switch (opr.getReg()) {
      case INSN::DS:
      case INSN::FS:
      case INSN::GS:
      case INSN::SS:
        iinfo.segflag = 1;
        break;
      default:
        break;
      }
    }
  }
}

#endif // end of ICPP_HAS_X64

#define MACHO_MAGIC_BIT 0x10000
#define ELF_MAGIC_BIT 0x20000
#define COFF_MAGIC_BIT 0x40000

using SymbolRef = object::SymbolRef;

struct RelocSymbol {
  /*
  symbol detail
  */
  SymbolRef sym;
  StringRef name;
  SymbolRef::Type stype;
  unsigned int sflags = 0;
  /*
  relocation detail
  */
  uint32_t rtype = 0;
  int addend = 0;
};

static SymbolRef::Type reloc_symtype(const InsnInfo &inst, ArchType arch,
                                     ObjectType otype,
                                     const RelocSymbol &rsym) {
  auto rtype = rsym.rtype;
  switch (otype) {
  case MachO_Reloc:
    rtype |= MACHO_MAGIC_BIT;
    break;
  case ELF_Reloc:
    rtype |= ELF_MAGIC_BIT;
    break;
  default:
    rtype |= COFF_MAGIC_BIT;
    break;
  }

  switch (arch) {
  case AArch64: {
    switch (rtype) {
    case MachO::ARM64_RELOC_GOT_LOAD_PAGE21 | MACHO_MAGIC_BIT:
    case ELF::R_AARCH64_GOTREL64 | ELF_MAGIC_BIT:
    case ELF::R_AARCH64_GOT_LD_PREL19 | ELF_MAGIC_BIT:
    case ELF::R_AARCH64_ADR_GOT_PAGE | ELF_MAGIC_BIT:
      return SymbolRef::ST_Data;
    default:
      break;
    }
    break;
  }
  case X86_64: {
    switch (rtype) {
    case MachO::X86_64_RELOC_GOT | MACHO_MAGIC_BIT:
    case MachO::X86_64_RELOC_GOT_LOAD | MACHO_MAGIC_BIT:
    case ELF::R_X86_64_GOTPCREL | ELF_MAGIC_BIT:
    case ELF::R_X86_64_REX_GOTPCRELX | ELF_MAGIC_BIT:
// undefine these macros from windows headers
#undef IMAGE_REL_AMD64_ADDR64
#undef IMAGE_REL_AMD64_REL32
    case COFF::RelocationTypeAMD64::IMAGE_REL_AMD64_ADDR64 | COFF_MAGIC_BIT:
      return SymbolRef::ST_Data;
    case COFF::RelocationTypeAMD64::IMAGE_REL_AMD64_REL32 | COFF_MAGIC_BIT:
      switch (inst.type) {
      case INSN_X64_CALLMEM:
      case INSN_X64_JUMPMEM:
      case INSN_X64_MOV8RM:
      case INSN_X64_MOV8MR:
      case INSN_X64_MOV16RM:
      case INSN_X64_MOV16MR:
      case INSN_X64_MOV32RM:
      case INSN_X64_MOV32MR:
      case INSN_X64_MOV64RM:
      case INSN_X64_MOV64MR:
      case INSN_X64_MOV64MI32:
      case INSN_X64_MOVAPSRM:
      case INSN_X64_MOVAPSMR:
      case INSN_X64_MOVUPSRM:
      case INSN_X64_MOVUPSMR:
      case INSN_X64_MOVAPDRM:
      case INSN_X64_MOVAPDMR:
      case INSN_X64_MOVUPDRM:
      case INSN_X64_MOVUPDMR:
      case INSN_X64_CMP8MI:
      case INSN_X64_CMP8MI8:
      case INSN_X64_CMP16MI:
      case INSN_X64_CMP16MI8:
      case INSN_X64_CMP32MI:
      case INSN_X64_CMP32MI8:
      case INSN_X64_CMP64MI32:
      case INSN_X64_CMP64MI8:
      case INSN_X64_CMP8RM:
      case INSN_X64_CMP16RM:
      case INSN_X64_CMP32RM:
      case INSN_X64_CMP64RM:
      case INSN_X64_MOVSX16RM8:
      case INSN_X64_MOVSX16RM16:
      case INSN_X64_MOVSX16RM32:
      case INSN_X64_MOVSX32RM8:
      case INSN_X64_MOVSX32RM16:
      case INSN_X64_MOVSX32RM32:
      case INSN_X64_MOVSX64RM8:
      case INSN_X64_MOVSX64RM16:
      case INSN_X64_MOVSX64RM32:
      case INSN_X64_MOVZX16RM8:
      case INSN_X64_MOVZX16RM16:
      case INSN_X64_MOVZX32RM8:
      case INSN_X64_MOVZX32RM16:
      case INSN_X64_MOVZX64RM8:
      case INSN_X64_MOVZX64RM16:
      case INSN_X64_TEST8MI:
      case INSN_X64_TEST8MR:
      case INSN_X64_TEST16MI:
      case INSN_X64_TEST16MR:
      case INSN_X64_TEST32MI:
      case INSN_X64_TEST32MR:
      case INSN_X64_TEST64MI32:
      case INSN_X64_TEST64MR:
      case INSN_X64_CMOV16RM:
      case INSN_X64_CMOV32RM:
      case INSN_X64_CMOV64RM:
        return ((rsym.sflags & SymbolRef::SF_Undefined) &&
                (rsym.name.starts_with("__imp_")))
                   ? SymbolRef::ST_Data
                   : SymbolRef::ST_Function;
      default:
        return SymbolRef::ST_Function;
      }
    default:
      break;
    }
    break;
  }
  default:
    break;
  }
  return SymbolRef::ST_Function;
}

static int reloc_addend(const CObjectFile *object,
                        object::RelocationRef reloc) {
  if (object->isMachO()) {
    auto O = static_cast<const object::MachOObjectFile *>(object);
    const DataRefImpl Rel = reloc.getRawDataRefImpl();
    const MachO::any_relocation_info RE = O->getRelocation(Rel);
    const unsigned r_type = O->getAnyRelocationType(RE);
    const bool r_scattered = O->isRelocationScattered(RE);
    const unsigned r_symbolnum =
        (r_scattered ? 0 : O->getPlainRelocationSymbolNum(RE));
    if (r_type == MachO::ARM64_RELOC_ADDEND)
      return r_symbolnum;
  } else if (object->isELF()) {
  } else if (object->isCOFF()) {
  }
  return 0;
}

static RelocSymbol get_symbol(uint64_t addr, const SymbolRef &sym,
                              uint32_t rtype) {
  RelocSymbol rsym;
  rsym.rtype = rtype;

  auto expName = sym.getName();
  auto expType = sym.getType();
  auto expFlags = sym.getFlags();
  auto checker = [addr]<typename T>(std::string_view name, T &exp) {
    if (exp)
      return true;
    auto strerr = toString(exp.takeError());
    // so many fake errors on macOS, ignoring them...
    log_print(Ignore, "Bad symbol {}: {:x}, {}.", name, addr, strerr);
    return false;
  };
  bool hasName = checker("name", expName);
  bool hasType = checker("type", expType);
  bool hasFlag = checker("flags", expFlags);
  if (hasName && hasType && hasFlag) {
    rsym.sym = sym;
    rsym.name = expName.get();
    rsym.stype = expType.get();
    rsym.sflags = expFlags.get();
#if _WIN32
    // on windows, the printf is a local function in object file, in order
    // to redirect it easily in icpp-gadget, change it to an extern symbol.
    if (RunConfig::gadget) {
      if (rsym.name == "printf")
        rsym.sflags |= SymbolRef::SF_Undefined;
    }
#endif
  }
  return rsym;
}

static void reloc_symbols(ObjectFile *ofile, ArchType arch, TextSection &text,
                          std::map<uint64_t, RelocSymbol> &rsyms) {
  for (auto &texts : ofile->sections()) {
    if (texts.getIndex() != text.index) {
      continue;
    }
    if (ofile->isELF()) {
      LLVM_ELF_IMPORT_TYPES_ELFT(object::ELF64LE);

      auto expName = texts.getName();
      if (!expName) {
        auto err = expName.takeError();
        auto strerr = toString(std::move(err));
        log_print(Runtime, "Bad symbol name: {}.", strerr);
        return;
      }
      auto textname = expName.get();
      if (textname.size() == 0)
        continue;
      // all of our supported platforms are 64-bit little endian Linux/Android
      // system
      auto oelf = static_cast<ELF64LEObjectFile *>(ofile);
      auto elf = oelf->getELFFile();
      auto checker = [&texts, &oelf](const Elf_Shdr &Sec) {
        return texts == oelf->toSectionRef(&Sec);
      };
      Expected<MapVector<const Elf_Shdr *, const Elf_Shdr *>> expTextReloc =
          elf.getSectionAndRelocations(checker);
      if (!expTextReloc) {
        log_print(Runtime, "Unable to get {} map section: {}.", textname.data(),
                  toString(expTextReloc.takeError()));
        return;
      }
      if (!expTextReloc->size())
        continue;
      auto relocsect = expTextReloc->begin()->second;
      if (!relocsect)
        continue;
      auto expSymtab = elf.getSection(relocsect->sh_link);
      if (!expSymtab) {
        log_print(Runtime, "Unable to locate a symbol table: {}.",
                  toString(expSymtab.takeError()));
        return;
      }
      if (Expected<Elf_Rela_Range> expRelas = elf.relas(*relocsect)) {
        for (const Elf_Rela &r : *expRelas) {
          auto sym = oelf->toSymbolRef(*expSymtab, r.getSymbol(false));
          auto addr = text.frva + r.r_offset;
          auto rsym = get_symbol(addr, sym, r.getType(false));
          if (rsym.name.size()) {
            rsym.addend = r.r_addend;
            /*
            I don't know why there's always a -4 addend on x86_64 linux,
            and it doesn't make any sense in icpp, so restore it to 0.
            */
            // FIXME:: calculate the real addend with relocation ?
            if (arch == X86_64) {
              if (rsym.addend < -4)
                rsym.addend = 0;
              else
                rsym.addend += 4;
            }
            rsyms.insert({addr, rsym});
          }
        }
      } else {
        log_print(Runtime, "Unable to get rela list: {}.",
                  toString(expRelas.takeError()));
      }
      break;
    }
    uint64_t addend = 0;
    for (auto &r : texts.relocations()) {
      auto addr = text.frva + r.getOffset();
      auto sym = r.getSymbol();
      auto rsym = get_symbol(addr, *sym, r.getType());
      if (rsym.name.size()) {
        if (addend) {
          rsym.addend = addend;
          addend = 0;
        } else {
          rsym.addend = reloc_addend(ofile, r);
        }
        rsyms.insert({addr, rsym});
      } else if (arch == AArch64) {
        // arm64 addend relocation is separated with the main relocation,
        // usually it's in this sequence:
        // 0 - ARM64_RELOC_ADDEND
        // 1 - ARM64_RELOC_PAGE21
        // 2 - ARM64_RELOC_PAGEOFF12
        addend = reloc_addend(ofile, r);
      }
    }
    break;
  }
}

#if ARCH_X64
static bool prefix_inst(const MCInst &inst) {
  namespace INSN = llvm::X86;
  switch (inst.getOpcode()) {
  case INSN::ADDR16_PREFIX:
  case INSN::ADDR32_PREFIX:
  case INSN::CS_PREFIX:
  case INSN::DATA16_PREFIX:
  case INSN::DS_PREFIX:
  case INSN::ES_PREFIX:
  case INSN::FS_PREFIX:
  case INSN::GS_PREFIX:
  case INSN::LOCK_PREFIX:
  case INSN::REPNE_PREFIX:
  case INSN::REP_PREFIX:
  case INSN::REX64_PREFIX:
  case INSN::SS_PREFIX:
  case INSN::XACQUIRE_PREFIX:
  case INSN::XRELEASE_PREFIX:
    return true;
  default:
    return false;
  }
}
#endif

void Object::decodeInsns(TextSection &text) {
  // load text relocation symbols
  std::map<uint64_t, RelocSymbol> rsyms;
  reloc_symbols(ofile_.get(), arch(), text, rsyms);

  int skipsz = arch_ == AArch64 ? 4 : 1;
  // decode instructions in text section
  MCInst inst;
  for (auto opc = text.vm, opcend = text.vm + text.size; opc < opcend;) {
    uint64_t size = 0;
    auto status = odiser_.DT->DisAsm->getInstruction(
        inst, size, BuildIDRef(reinterpret_cast<const uint8_t *>(opc), 16), opc,
        outs());
    InsnInfo iinfo{};
    iinfo.rva = text.frva + opc - text.vm;

#if ARCH_X64
    if (prefix_inst(inst)) {
      uint64_t size2 = 0;
      auto opc2 = opc + size;
      // reset inst to the real instruction informtion
      status = odiser_.DT->DisAsm->getInstruction(
          inst, size2, BuildIDRef(reinterpret_cast<const uint8_t *>(opc2), 16),
          opc2, outs());
      // the composite opcode size = prefix + inst
      size += size2;
    }
#endif

    switch (status) {
    case MCDisassembler::Fail: {
      iinfo.type = INSN_ABORT;
      iinfo.len = skipsz;
      break;
    }
    case MCDisassembler::SoftFail: {
      iinfo.type = INSN_ABORT;
      iinfo.len = size ? static_cast<uint32_t>(size) : skipsz;
      break;
    }
    default: {
      iinfo.len = static_cast<uint32_t>(size);
      // convert llvm opcode to icpp InsnType
      std::function<uint16_t(unsigned)> llvm2uc_register;
      if (arch() == AArch64) {
#if ICPP_HAS_AARCH64
        llvm2uc_register = llvm2ucRegisterAArch64;
        parseInstAArch64(inst, opc, idecinfs_, iinfo);
#endif
      } else {
#if ICPP_HAS_X64
        llvm2uc_register = llvm2ucRegisterX64;
        parseInstX64(inst, opc, idecinfs_, iinfo);
#endif
      }
      // check and resolve the relocation symbol
#if ARCH_ARM64
      auto found = rsyms.find(iinfo.rva);
#else
      auto found = rsyms.end();
      for (int i = 1; i <= iinfo.len - 4; i++) {
        found = rsyms.find(iinfo.rva + i);
        if (found != rsyms.end())
          break;
      }
#endif
      if (found != rsyms.end()) {
        const void *rtaddr = nullptr;
        auto &rsym = found->second;
        auto symtype = reloc_symtype(iinfo, arch(), type(), rsym);
        if (rsym.sflags & SymbolRef::SF_Undefined) {
          // an extern relocation
          rtaddr =
              Loader::locateSymbol(rsym.name, symtype == SymbolRef::ST_Data);
        } else {
          // a local relocation
          auto expSect = rsym.sym.getSection();
          auto expAddr = rsym.sym.getAddress();
          if (!expSect || !expAddr) {
            // never be here
            log_print(
                Runtime,
                "Fatal error, the symbol section/address of '{}'.'{:x}' is "
                "missing for "
                "relocation.",
                rsym.name.data(), vm2rva(opc));
            abort();
          }
          bool dyn = false;
          auto sectname = expSect.get()->getName();
          if (!sectname) {
            // never be here
            log_print(
                Runtime,
                "Fatal error, the section name is missing for relocation.");
            abort();
          }
          auto symoff =
              expAddr.get() - expSect.get()->getAddress() + rsym.addend;
          for (auto &ds : dynsects_) {
            if (expSect.get()->getIndex() == ds.index) {
              // dynamically allocated section
              dyn = true;

              rtaddr =
                  reinterpret_cast<const void *>(ds.buffer.data() + symoff);
              break;
            }
          }
          if (!dyn) {
            // inner section from file
            auto expContent = expSect.get()->getContents();
            if (!expContent) {
              // never be here
              log_print(
                  Runtime,
                  "Fatal error, the section content of '{}' is missing for "
                  "relocation.",
                  sectname->data());
              abort();
            }
            rtaddr =
                reinterpret_cast<const void *>(expContent->data() + symoff);
          }
        }
        // check the existed relocation
        auto rit = irelocs_.end();
        for (auto it = irelocs_.begin(), end = irelocs_.end(); it != end;
             it++) {
          if (rtaddr == it->target && symtype == it->type) {
            rit = it;
            // fix it as a data relocation for coff object
            if (arch_ == AArch64 && ofile_->isCOFF() &&
                (rsym.sflags & SymbolRef::SF_Undefined)) {
#undef IMAGE_REL_ARM64_PAGEOFFSET_12L
              if (rsym.rtype ==
                  COFF::RelocationTypesARM64::IMAGE_REL_ARM64_PAGEOFFSET_12L) {
                symtype = SymbolRef::ST_Data;
                it->target = Loader::locateSymbol(rsym.name, true);
                it->type = symtype;
              }
            }
            break;
          }
        }
        if (rit == irelocs_.end()) {
          // insert a new relocation record
          rit = irelocs_.insert(irelocs_.end(),
                                RelocInfo{rsym.name.data(), rtaddr,
                                          static_cast<uint32_t>(symtype)});
        }
        if (0) {
          log_print(Develop, "Relocated {:06x}.{} symbol {} at {}.",
                    (int)iinfo.rva,
                    symtype == SymbolRef::ST_Data ? "data" : "func", rit->name,
                    rit->target);
        }
        // record its relocation index
        iinfo.rflag = 1;
        iinfo.reloc = rit - irelocs_.begin();
      }
      // encode none-hardware instruction if there's no one
      if (iinfo.type != INSN_HARDWARE &&
          idecinfs_.find(std::string(reinterpret_cast<char *>(opc),
                                     iinfo.len)) == idecinfs_.end()) {
        auto newi =
            idecinfs_
                .insert({std::string(reinterpret_cast<char *>(opc), iinfo.len),
                         std::string()})
                .first;
        auto optr = const_cast<std::string *>(&newi->second);
        // we encode the instruction operands as follows:
        // if it's a register, then encode it to uc register index as uint16_t
        // if it's an immediate, then encode it as uint64_t
        for (unsigned i = 0; i < inst.getNumOperands(); i++) {
          auto opr = inst.getOperand(i);
          if (opr.isImm()) {
            auto imm = opr.getImm();
            optr->append(
                std::string(reinterpret_cast<char *>(&imm), sizeof(imm)));
          } else if (opr.isReg()) {
            auto reg = llvm2uc_register(opr.getReg());
            optr->append(
                std::string(reinterpret_cast<char *>(&reg), sizeof(reg)));
          } else {
            // nerver be here
            log_print(Runtime, "Fatal error when decoding instruction at {:x}.",
                      vm2rva(opc));
            abort();
          }
        }
      }
      break;
    }
    } // end of switch
    text.iinfs.push_back(iinfo);
    opc += iinfo.len;
  }
}

static uint64_t relocate_data(StringRef content, uint64_t offset,
                              const RelocSymbol &rsym,
                              const std::vector<DynSection> &dynsects,
                              ObjectType otype, ArchType arch,
                              CObjectFile *ofile) {
  uint64_t target = 0;
  bool istext = false;
  if (rsym.sflags & SymbolRef::SF_Undefined) {
    // extern relocation
    target = reinterpret_cast<uint64_t>(Loader::locateSymbol(rsym.name, false));
  } else if (!rsym.name.size()) {
    if (!ofile->isMachO())
      return 0;
    if (rsym.rtype != MachO::X86_64_RELOC_UNSIGNED &&
        rsym.rtype != MachO::X86_64_RELOC_SIGNED)
      return 0;
    // macho local relocation
    auto saddr = *reinterpret_cast<const uint64_t *>(content.data() + offset);
    for (auto &s : ofile->sections()) {
      auto start = s.getAddress();
      auto end = start + s.getSize();
      if (start <= saddr && saddr < end) {
        auto expContent = s.getContents();
        if (!expContent) {
          // never be here
          log_print(Runtime, "Fatal error, the section content is "
                             "missing for "
                             "relocation.");
          abort();
        }
        target = reinterpret_cast<uint64_t>(expContent->data() + saddr - start);
        break;
      }
    }
    if (!target)
      return 0;
  } else {
    // local relocation
    auto expSect = rsym.sym.getSection();
    auto expAddr = rsym.sym.getAddress();
    if (!expSect || !expAddr) {
      // never be here
      log_print(Runtime,
                "Fatal error, the symbol section/address of '{}' is "
                "missing for "
                "relocation.",
                rsym.name.data());
      abort();
    }
    istext = expSect.get()->isText();
    bool dyn = false;
    auto sectname = expSect.get()->getName();
    if (!sectname) {
      // never be here
      log_print(Runtime,
                "Fatal error, the section name is missing for relocation.");
      abort();
    }
    auto symoff = expAddr.get() - expSect.get()->getAddress() + rsym.addend;
    for (auto &ds : dynsects) {
      if (expSect.get()->getIndex() == ds.index) {
        // dynamically allocated section
        dyn = true;

        target = reinterpret_cast<uint64_t>(ds.buffer.data() + symoff);
        break;
      }
    }
    if (!dyn) {
      // inner section from file
      auto expContent = expSect.get()->getContents();
      if (!expContent) {
        // never be here
        log_print(Runtime,
                  "Fatal error, the section content of '{}' is missing for "
                  "relocation.",
                  sectname->data());
        abort();
      }
      target = reinterpret_cast<uint64_t>(expContent->data() + symoff);
    }
  }

  auto rtype = rsym.rtype;
  switch (otype) {
  case MachO_Reloc:
    rtype |= MACHO_MAGIC_BIT;
    break;
  case ELF_Reloc:
    rtype |= ELF_MAGIC_BIT;
    break;
  default:
    rtype |= COFF_MAGIC_BIT;
    break;
  }
  switch (arch) {
  case X86_64: {
    switch (rtype) {
    case ELF::R_X86_64_PC32 | ELF_MAGIC_BIT: {
      /*
      the following instruction sequence refers this kind of relocation:
        leaq (content), %rcx
        movslq (%rcx,%rax,4), %rax
        addq %rcx, %rax
        jmpq *%rax
      in object file:
        ref-address = target_symbol + addend
      at runtime:
        ref-address = reloc_section + offset + relpc32
      so, the relocating 32-bit value should be:
        relpc32 = target_symbol + addend - (reloc_section + offset)
      */
      uint32_t rel32 = static_cast<uint32_t>(
          target - reinterpret_cast<uint64_t>(content.data() + offset));
      if (0) {
        log_print(Develop, "Relocated data symbol {} at 0x{:x}.",
                  rsym.name.data(), rel32);
      }
      if (offset + 4 > content.size()) {
        log_print(Runtime,
                  "Warning, relocation 4 bytes for {} is out fo range, "
                  "max {:x}, offset {:x}.\n",
                  rsym.name.data(), content.size(), offset);
        return 0;
      }
      *reinterpret_cast<uint32_t *>(
          const_cast<char *>(content.data() + offset)) = rel32;
      return 0;
    }
    case COFF::RelocationTypeAMD64::IMAGE_REL_AMD64_REL32 | COFF_MAGIC_BIT: {
      auto relocpot = reinterpret_cast<uint32_t *>(
          const_cast<char *>(content.data() + offset));
      // coff places the static addend here
      auto addend = relocpot[0];
      /*
      the same formula used by ELF::R_X86_64_PC32 on Linux, but it needs to
      minus an extra 4. I didn't find the documentation to explain this
      constant value, but I found the worked magic at llvm lld's coff linker
      code: SectionChunk::applyRelX64: case IMAGE_REL_AMD64_REL32: add32(off,
      s - p - 4);
      */
      uint32_t rel32 = static_cast<uint32_t>(
          target + addend -
          reinterpret_cast<uint64_t>(content.data() + offset) - 4);
      if (0) {
        log_print(Develop, "Relocated data symbol {} at 0x{:x}.",
                  rsym.name.data(), rel32);
      }
      if (offset + 4 > content.size()) {
        log_print(Runtime,
                  "Warning, relocation 4 bytes for {} is out fo range, "
                  "max {:x}, offset {:x}.\n",
                  rsym.name.data(), content.size(), offset);
        return 0;
      }
      relocpot[0] = rel32;
      return 0;
    }
#undef IMAGE_REL_AMD64_ADDR32NB
    case COFF::RelocationTypeAMD64::IMAGE_REL_AMD64_ADDR32NB | COFF_MAGIC_BIT:
      return 0; // ignore this kind of reloc currently
    default:
      break;
    }
    break;
  }
  case AArch64: {
    switch (rtype) {
#undef IMAGE_REL_ARM64_ADDR32NB
    case COFF::RelocationTypesARM64::IMAGE_REL_ARM64_ADDR32NB | COFF_MAGIC_BIT:
      return 0; // ignore this kind of reloc currently
    default:
      break;
    }
    break;
  }
  default:
    break;
  }
  if (0) {
    log_print(Develop, "Relocated data symbol {} at 0x{:x}.", rsym.name.data(),
              target);
  }
  if (offset + 8 > content.size()) {
    log_print(Runtime,
              "Warning, relocation 8 bytes for {} is out fo range, "
              "max {:x}, offset {:x}.\n",
              rsym.name.data(), content.size(), offset);
    return 0;
  }
  auto relocpot = reinterpret_cast<uint64_t>(content.data() + offset);
  *reinterpret_cast<uint64_t *>(relocpot) = reinterpret_cast<uint64_t>(target);
  return istext ? relocpot : 0;
}

void Object::relocateData(uint32_t index, const StringRef &content,
                          uint64_t offset, const void *prsym) {
  auto rsym = reinterpret_cast<const RelocSymbol *>(prsym);
  auto spot = relocate_data(content, offset, *rsym, dynsects_, type(), arch(),
                            ofile_.get());
  if (spot)
    stubspots_.push_back(
        {index, static_cast<uint32_t>(offset), spot, rsym->name.data()});
}

void Object::parseSections() {
  struct vmrva_updator {
    ~vmrva_updator() {
      if (!commit)
        return;
      // update the next section's vm rva
      vmrva += size ? size : 8;
      vmrva = alignToPowerOf2(vmrva, 8);
    }

    uint64_t size;
    uint32_t &vmrva;
    bool commit = true;
  };
  // it doesn't make any sense for runtime but useful to locate the section in
  // VMPStudio or IDA when debugging the following code
  uint32_t vmrva = 0;
  for (auto &s : ofile_->sections()) {
    vmrva_updator update{s.getSize(), vmrva};
    if (ofile_->isMachO())
      vmrva = s.getAddress();

    auto expName = s.getName();
    if (!expName)
      continue;
    auto name = expName.get();
    if (s.isText()) {
      if (!update.size)
        continue; // empty section

      auto expContent = s.getContents();
      if (!expContent) {
        log_print(Develop,
                  "Empty object file, there's no content of {} section.",
                  name.data());
        break;
      }
      auto &news = textsects_.emplace_back(
          TextSection{static_cast<uint32_t>(s.getIndex()),
                      static_cast<uint32_t>(s.getSize()), 0, vmrva,
                      reinterpret_cast<uint64_t>(expContent->data())});
      // file rva relative to text[0] section
      news.frva = news.vm - textsects_[0].vm;
      if (ofile_->isELF())
        news.vrva = news.frva - textsects_[0].frva;
      if (0) {
        log_print(Develop,
                  "Section index={} frva={:x} vmrva={:x} vm={:x} size={} {}.",
                  news.index, news.frva, news.vrva, news.vm, news.size,
                  name.data());
      }
    } else if (s.isBSS() || name.ends_with("bss") || name.ends_with("common")) {
      dynsects_.push_back(
          {static_cast<uint32_t>(s.getIndex()), std::string(s.getSize(), 0)});
    } else {
      auto expContent = s.getContents();
      if (!expContent)
        continue;
      // commit relocations for this data section
      if (type() == ELF_Reloc) {
        LLVM_ELF_IMPORT_TYPES_ELFT(object::ELF64LE);

        auto oelf = static_cast<ELF64LEObjectFile *>(ofile_.get());
        auto elf = oelf->getELFFile();
        auto checker = [&s, &oelf](const Elf_Shdr &Sec) {
          return s == oelf->toSectionRef(&Sec);
        };
        Expected<MapVector<const Elf_Shdr *, const Elf_Shdr *>> expDataReloc =
            elf.getSectionAndRelocations(checker);
        if (!expDataReloc) {
          log_print(Runtime, "Unable to get {} map section: {}.", name.data(),
                    toString(expDataReloc.takeError()));
          return;
        }
        if (!expDataReloc->size())
          continue;
        auto elfsect = expDataReloc->begin()->first;
        if (elfsect->sh_type != ELF::SHT_PROGBITS) {
          update.commit = false;
        }
        if (!update.size)
          continue; // empty section
        auto relocsect = expDataReloc->begin()->second;
        if (!relocsect)
          continue;
        auto expSymtab = elf.getSection(relocsect->sh_link);
        if (!expSymtab) {
          log_print(Runtime, "Unable to locate a symbol table: {}.",
                    toString(expSymtab.takeError()));
          return;
        }
        if (Expected<Elf_Rela_Range> expRelas = elf.relas(*relocsect)) {
          for (const Elf_Rela &r : *expRelas) {
            auto sym = oelf->toSymbolRef(*expSymtab, r.getSymbol(false));
            auto addr = s.getAddress() + r.r_offset;
            auto rsym = get_symbol(addr, sym, r.getType(false));
            if (rsym.name.size()) {
              rsym.addend = r.r_addend;
              relocateData(s.getIndex(), expContent.get(), r.r_offset, &rsym);
            }
          }
        } else {
          log_print(Runtime, "Unable to get rela list: {}.",
                    toString(expRelas.takeError()));
        }
        continue;
      }

      if (!update.size)
        continue; // empty section

      auto sectBuff = expContent.get();
#if _WIN32
      if (!s.relocations().empty() &&
          (reinterpret_cast<uint64_t>(sectBuff.data()) & 1)) {
        // move this section to dynamic sections as we need it to be 4/8 bytes
        // aligned for .text reference
        auto &dyns = dynsects_.emplace_back(
            DynSection{static_cast<uint32_t>(s.getIndex()),
                       std::string(sectBuff.size(), 0)});
        std::memcpy(const_cast<char *>(dyns.buffer.data()), sectBuff.data(),
                    sectBuff.size());
        sectBuff = dyns.buffer;
      }
#endif
      for (auto r : s.relocations()) {
        auto sym = r.getSymbol();
        auto expFlags = sym->getFlags();
        if (!expFlags) {
          log_print(Develop, "Bad symbol flags: {}.",
                    toString(expFlags.takeError()));
          continue;
        }
        auto rsym = get_symbol(0, *sym, r.getType());
        if (rsym.stype == SymbolRef::ST_File)
          continue;
        relocateData(s.getIndex(), sectBuff, r.getOffset(), &rsym);
      }
    }
  }
}

} // namespace icpp
