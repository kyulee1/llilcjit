//===---- objwriter.cpp --------------------------------*- C++ -*-===//
//
// object writer
//
// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license.
// See LICENSE file in the project root for full license information.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// \brief Implementation of object writer API for JIT/AOT
///
//===----------------------------------------------------------------------===//

#include "llvm/CodeGen/AsmPrinter.h"
#include "llvm/MC/MCAsmBackend.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCInstPrinter.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCObjectFileInfo.h"
#include "llvm/MC/MCParser/AsmLexer.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSectionMachO.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/MCTargetAsmParser.h"
#include "llvm/MC/MCTargetOptionsCommandFlags.h"
#include "llvm/MC/MCWinCOFFStreamer.h"
#include "llvm/Support/COFF.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Compression.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/FormattedStream.h"
#include "llvm/Support/Host.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/Target/TargetSubtargetInfo.h"

using namespace llvm;

static cl::opt<std::string>
    ArchName("arch", cl::desc("Target arch to assemble for, "
                              "see -version for available targets"));

static cl::opt<std::string>
    TripleName("triple", cl::desc("Target triple to assemble for, "
                                  "see -version for available targets"));

static cl::opt<std::string>
    MCPU("mcpu",
         cl::desc("Target a specific cpu type (-mcpu=help for details)"),
         cl::value_desc("cpu-name"), cl::init(""));

static cl::opt<Reloc::Model> RelocModel(
    "relocation-model", cl::desc("Choose relocation model"),
    cl::init(Reloc::Default),
    cl::values(
        clEnumValN(Reloc::Default, "default",
                   "Target default relocation model"),
        clEnumValN(Reloc::Static, "static", "Non-relocatable code"),
        clEnumValN(Reloc::PIC_, "pic",
                   "Fully relocatable, position independent code"),
        clEnumValN(Reloc::DynamicNoPIC, "dynamic-no-pic",
                   "Relocatable external references, non-relocatable code"),
        clEnumValEnd));

static cl::opt<llvm::CodeModel::Model> CMModel(
    "code-model", cl::desc("Choose code model"), cl::init(CodeModel::Default),
    cl::values(clEnumValN(CodeModel::Default, "default",
                          "Target default code model"),
               clEnumValN(CodeModel::Small, "small", "Small code model"),
               clEnumValN(CodeModel::Kernel, "kernel", "Kernel code model"),
               clEnumValN(CodeModel::Medium, "medium", "Medium code model"),
               clEnumValN(CodeModel::Large, "large", "Large code model"),
               clEnumValEnd));

static cl::opt<bool> SaveTempLabels("save-temp-labels",
                                    cl::desc("Don't discard temporary labels"));

static cl::opt<bool> NoExecStack("no-exec-stack",
                                 cl::desc("File doesn't need an exec stack"));

static const Target *GetTarget() {
  // Figure out the target triple.
  if (TripleName.empty())
    TripleName = sys::getDefaultTargetTriple();
  Triple TheTriple(Triple::normalize(TripleName));

  // Get the target specific parser.
  std::string Error;
  const Target *TheTarget =
      TargetRegistry::lookupTarget(ArchName, TheTriple, Error);
  if (!TheTarget) {
    errs() << "Error: " << Error;
    return nullptr;
  }

  // Update the triple name and return the found target.
  TripleName = TheTriple.getTriple();
  return TheTarget;
}

bool error(const Twine &Error) {
  errs() << Twine("error: ") + Error + "\n";
  return false;
}

class ObjectWriter {
public:
  std::unique_ptr<MCRegisterInfo> MRI;
  std::unique_ptr<MCAsmInfo> MAI;
  std::unique_ptr<MCObjectFileInfo> MOFI;
  std::unique_ptr<MCContext> MC;
  MCAsmBackend *MAB; // Owned by MCStreamer
  std::unique_ptr<MCInstrInfo> MII;
  std::unique_ptr<MCSubtargetInfo> MSTI;
  MCCodeEmitter *MCE; // Owned by MCStreamer
  std::unique_ptr<TargetMachine> TM;
  std::unique_ptr<AsmPrinter> Asm;

  std::unique_ptr<MCAsmParser> Parser;
  std::unique_ptr<MCTargetAsmParser> TAP;

  std::unique_ptr<raw_fd_ostream> OS;
  MCTargetOptions MCOptions;

public:
  bool init(StringRef FunctionName);
  void finish();
  AsmPrinter &getAsmPrinter() const { return *Asm; }
  MCStreamer *MS; // Owned by AsmPrinter
  const Target *TheTarget;
};

bool ObjectWriter::init(llvm::StringRef ObjectFilePath) {
  // Print a stack trace if we signal out.
  sys::PrintStackTraceOnErrorSignal();
  llvm_shutdown_obj Y; // Call llvm_shutdown() on exit.

  // Initialize targets
  InitializeNativeTarget();
  InitializeNativeTargetAsmPrinter();

  MCOptions = InitMCTargetOptionsFromFlags();
  TripleName = Triple::normalize(TripleName);

  TheTarget = GetTarget();
  if (!TheTarget)
    return error("Unable to get Target");
  // Now that GetTarget() has (potentially) replaced TripleName, it's safe to
  // construct the Triple object.
  Triple TheTriple(TripleName);

  std::error_code EC;
  OS.reset(new raw_fd_ostream(ObjectFilePath, EC, sys::fs::F_None));
  if (EC)
    return error("Unable to create file for " + ObjectFilePath + ": " +
                 EC.message());

  MRI.reset(TheTarget->createMCRegInfo(TripleName));
  if (!MRI)
    return error("Unable to create target register info!");

  MAI.reset(TheTarget->createMCAsmInfo(*MRI, TripleName));
  if (!MAI)
    return error("Unable to create target asm info!");

  MOFI.reset(new MCObjectFileInfo);
  MC.reset(new MCContext(MAI.get(), MRI.get(), MOFI.get()));
  MOFI->InitMCObjectFileInfo(TheTriple, RelocModel, CMModel, *MC);

  std::string FeaturesStr;

  MII.reset(TheTarget->createMCInstrInfo());
  if (!MII)
    return error("no instr info info for target " + TripleName);

  MSTI.reset(TheTarget->createMCSubtargetInfo(TripleName, MCPU, FeaturesStr));
  if (!MSTI)
    return error("no subtarget info for target " + TripleName);

  MCE = TheTarget->createMCCodeEmitter(*MII, *MRI, *MC);
  if (!MCE)
    return error("no code emitter for target " + TripleName);

  MAB = TheTarget->createMCAsmBackend(*MRI, TripleName, MCPU);
  if (!MAB)
    return error("no asm backend for target " + TripleName);

  MS = TheTarget->createMCObjectStreamer(TheTriple, *MC, *MAB, *OS, MCE, *MSTI,
                                         RelaxAll,
                                         /*DWARFMustBeAtTheEnd*/ false);
  if (!MS)
    return error("no object streamer for target " + TripleName);

  TM.reset(TheTarget->createTargetMachine(TripleName, MCPU, FeaturesStr,
                                          TargetOptions()));
  if (!TM)
    return error("no target machine for target " + TripleName);

  Asm.reset(TheTarget->createAsmPrinter(*TM, std::unique_ptr<MCStreamer>(MS)));
  if (!Asm)
    return error("no asm printer for target " + TripleName);

  return true;
}

void ObjectWriter::finish() { MS->Finish(); }

// When object writer is created/initialized successfully, it is returned.
// Or null object is returned. Client should check this.
extern "C" ObjectWriter *InitObjWriter(const char *ObjectFilePath) {
  ObjectWriter *OW = new ObjectWriter();
  if (OW->init(ObjectFilePath)) {
    return OW;
  }

  delete OW;
  return nullptr;
}

extern "C" void FinishObjWriter(ObjectWriter *OW) {
  assert(OW && "ObjWriter is null");
  OW->finish();
  delete OW;
}

extern "C" void SwitchSection(ObjectWriter *OW, const char *SectionName) {
  assert(OW && "ObjWriter is null");
  auto *AsmPrinter = &OW->getAsmPrinter();
  auto &OST = *AsmPrinter->OutStreamer;
  MCContext &OutContext = OST.getContext();
  const MCObjectFileInfo *MOFI = OutContext.getObjectFileInfo();

  MCSection *Section = nullptr;
  if (strcmp(SectionName, "text") == 0) {
    Section = MOFI->getTextSection();
  } else if (strcmp(SectionName, "data") == 0) {
    Section = MOFI->getDataSection();
  } else if (strcmp(SectionName, "rdata") == 0) {
    Section = MOFI->getReadOnlySection();
  } else {
    // Add more general cases
    assert(!"Unsupported section");
  }

  OST.SwitchSection(Section);

  if (strcmp(SectionName, "text") == 0) {
  MCSection *Sec = OST.getCurrentSection().first;
  Sec->setHasInstructions(true); // set instruction exists.
  OutContext.setGenDwarfForAssembly(true); // fake we emitting dwarf for assembly file

  if (!Sec->getBeginSymbol()) {
     MCSymbol *SectionStartSym = OutContext.createTempSymbol();
     OST.EmitLabel(SectionStartSym);
     Sec->setBeginSymbol(SectionStartSym);
  }
  bool InsertResult = OutContext.addGenDwarfSection(Sec);
  assert(InsertResult && ".text section should not have debug info yet");
  (void)InsertResult;
  OutContext.setGenDwarfFileNumber(OST.EmitDwarfFileDirective(
     1, StringRef(), "t3.cpp"));
  }
}

extern "C" void EmitLoc(ObjectWriter *OW, int Line, int Col) {
   auto *AsmPrinter = &OW->getAsmPrinter();
   auto &OST = *AsmPrinter->OutStreamer;
   MCContext &OutContext = OST.getContext();
   OST.EmitDwarfLocDirective(
      OutContext.getGenDwarfFileNumber(), Line, Col,
   DWARF2_FLAG_IS_STMT, 0, 0, StringRef());
}


extern "C" void EmitAlignment(ObjectWriter *OW, int ByteAlignment) {
  assert(OW && "ObjWriter is null");
  auto *AsmPrinter = &OW->getAsmPrinter();
  auto &OST = *AsmPrinter->OutStreamer;

  OST.EmitValueToAlignment(ByteAlignment, 0x90 /* Nop */);
}

extern "C" void EmitBlob(ObjectWriter *OW, const char *Blob, int BlobSize) {
  assert(OW && "ObjWriter null");
  auto *AsmPrinter = &OW->getAsmPrinter();
  auto &OST = *AsmPrinter->OutStreamer;

  OST.EmitBytes(StringRef(Blob, BlobSize));
}

extern "C" void EmitIntValue(ObjectWriter *OW, uint64_t Value, unsigned Size) {
  assert(OW && "ObjWriter is null");
  auto *AsmPrinter = &OW->getAsmPrinter();
  auto &OST = *AsmPrinter->OutStreamer;

  OST.EmitIntValue(Value, Size);
}

extern "C" void EmitSymbolDef(ObjectWriter *OW, const char *SymbolName) {
  assert(OW && "ObjWriter is null");
  auto *AsmPrinter = &OW->getAsmPrinter();
  auto &OST = *AsmPrinter->OutStreamer;
  MCContext &OutContext = OST.getContext();

  MCSymbol *Sym = OutContext.getOrCreateSymbol(Twine(SymbolName));
  OST.EmitSymbolAttribute(Sym, MCSA_Global);

// debug

     // We create a temporary symbol for use for the AT_high_pc and AT_low_pc
     // values so that they don't have things like an ARM thumb bit from the
     // original symbol. So when used they won't get a low bit set after
     // relocation.
     MCSymbol *Label = OutContext.createTempSymbol();
     OST.EmitLabel(Label);

     // Create and entry for the info and add it to the other entries.
     OutContext.addMCGenDwarfLabelEntry(
        MCGenDwarfLabelEntry(SymbolName, 1, 1, Label));
  OST.EmitLabel(Sym);
}

extern "C" void EmitSymbolRef(ObjectWriter *OW, const char *SymbolName,
                              int Size, bool IsPCRelative, int Delta = 0) {
  assert(OW && "ObjWriter is null");
  auto *AsmPrinter = &OW->getAsmPrinter();
  auto &OST = static_cast<MCObjectStreamer &>(*AsmPrinter->OutStreamer);
  MCContext &OutContext = OST.getContext();

  // Create symbol reference
  MCSymbol *target = OutContext.getOrCreateSymbol(SymbolName);
  MCAssembler &MCAsm = OST.getAssembler();
  MCAsm.registerSymbol(*target);
  const MCExpr *TargetExpr = MCSymbolRefExpr::create(target, OutContext);

  switch (Size) {
  case 8:
    assert(!IsPCRelative && "NYI no support for 8 byte pc-relative");
    break;
  case 4:
    Size = 4;
    // If the fixup is pc-relative, we need to bias the value to be relative to
    // the start of the field, not the end of the field
    if (IsPCRelative) {
      TargetExpr = MCBinaryExpr::createSub(
          TargetExpr, MCConstantExpr::create(Size, OutContext), OutContext);
    }
    break;
  default:
    assert(false && "NYI symbol reference size!");
  }

  if (Delta != 0) {
    TargetExpr = MCBinaryExpr::createAdd(
        TargetExpr, MCConstantExpr::create(Delta, OutContext), OutContext);
  }

  OST.EmitValue(TargetExpr, Size, SMLoc(), IsPCRelative);
}
