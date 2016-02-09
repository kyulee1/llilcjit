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
#include "llvm/DebugInfo/CodeView/CodeView.h"
#include "llvm/DebugInfo/CodeView/Line.h"
#include "llvm/DebugInfo/CodeView/SymbolRecord.h"
#include "llvm/MC/MCAsmBackend.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCDwarf.h"
#include "llvm/MC/MCInstPrinter.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCObjectFileInfo.h"
#include "llvm/MC/MCParser/AsmLexer.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSectionCOFF.h"
#include "llvm/MC/MCSectionELF.h"
#include "llvm/MC/MCSectionMachO.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/MCParser/MCTargetAsmParser.h"
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
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Support/Win64EH.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/Target/TargetSubtargetInfo.h"
#include "cfi.h"
#include <string>
#include "jitDebugInfo.h"

using namespace llvm;
using namespace llvm::codeview;

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
  bool FrameOpened;
  std::vector<DebugVarInfo> DebugVarInfos;

  std::map<std::string, MCSection *> CustomSections;
  int FuncId;

public:
  bool init(StringRef FunctionName);
  void finish();
  AsmPrinter &getAsmPrinter() const { return *Asm; }
  MCStreamer *MS; // Owned by AsmPrinter
  const Target *TheTarget;
};

bool ObjectWriter::init(llvm::StringRef ObjectFilePath) {
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
                                         /*IncrementalLinkerCompatible*/ true,
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

  FrameOpened = false;
  FuncId = 1;

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

extern "C" bool CreateDataSection(ObjectWriter *OW, const char *SectionName,
                                  bool IsReadOnly) {
  assert(OW && "ObjWriter is null");
  Triple TheTriple(TripleName);
  auto *AsmPrinter = &OW->getAsmPrinter();
  auto &OST = *AsmPrinter->OutStreamer;
  MCContext &OutContext = OST.getContext();

  std::string SectionNameStr(SectionName);
  assert(OW->CustomSections.find(SectionNameStr) == OW->CustomSections.end() &&
         "Section with duplicate name already exists");

  MCSection *Section = nullptr;
  SectionKind Kind =
      IsReadOnly ? SectionKind::getReadOnly() : SectionKind::getData();

  switch (TheTriple.getObjectFormat()) {
  case Triple::MachO:
    Section = OutContext.getMachOSection("__DATA", SectionName, 0, Kind);
    break;
  case Triple::COFF: {
    unsigned Characteristics =
        IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ;

    if (!IsReadOnly) {
      Characteristics |= IMAGE_SCN_MEM_WRITE;
    }
    Section = OutContext.getCOFFSection(SectionName, Characteristics, Kind);
    break;
  }
  case Triple::ELF: {
    unsigned Flags = ELF::SHF_ALLOC;
    if (!IsReadOnly) {
      Flags |= ELF::SHF_WRITE;
    }
    Section = OutContext.getELFSection(SectionName, ELF::SHT_PROGBITS, Flags);
    break;
  }
  default:
    return error("Unknown output format for target " + TripleName);
    break;
  }

  OW->CustomSections[SectionNameStr] = Section;
  return true;
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
    std::string SectionNameStr(SectionName);
    if (OW->CustomSections.find(SectionNameStr) != OW->CustomSections.end()) {
      Section = OW->CustomSections[SectionNameStr];
    } else {
      // Add more general cases
      assert(!"Unsupported section");
    }
  }

  OST.SwitchSection(Section);
}

extern "C" void EmitAlignment(ObjectWriter *OW, int ByteAlignment) {
  assert(OW && "ObjWriter is null");
  auto *AsmPrinter = &OW->getAsmPrinter();
  auto &OST = *AsmPrinter->OutStreamer;

  OST.EmitValueToAlignment(ByteAlignment, 0x90 /* Nop */);
}

extern "C" void EmitBlob(ObjectWriter *OW, int BlobSize, const char *Blob) {
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
  OST.EmitLabel(Sym);
}

static const MCSymbolRefExpr *
GetSymbolRefExpr(ObjectWriter *OW, const char *SymbolName,
                 MCSymbolRefExpr::VariantKind Kind = MCSymbolRefExpr::VK_None) {
  assert(OW && "ObjWriter is null");
  auto *AsmPrinter = &OW->getAsmPrinter();
  auto &OST = static_cast<MCObjectStreamer &>(*AsmPrinter->OutStreamer);
  MCContext &OutContext = OST.getContext();

  // Create symbol reference
  MCSymbol *T = OutContext.getOrCreateSymbol(SymbolName);
  MCAssembler &MCAsm = OST.getAssembler();
  MCAsm.registerSymbol(*T);
  return MCSymbolRefExpr::create(T, Kind, OutContext);
}

extern "C" void EmitSymbolRef(ObjectWriter *OW, const char *SymbolName,
                              int Size, bool IsPCRelative, int Delta = 0) {
  assert(OW && "ObjWriter is null");
  auto *AsmPrinter = &OW->getAsmPrinter();
  auto &OST = static_cast<MCObjectStreamer &>(*AsmPrinter->OutStreamer);
  MCContext &OutContext = OST.getContext();

  // Get symbol reference expression
  const MCExpr *TargetExpr = GetSymbolRefExpr(OW, SymbolName);

  switch (Size) {
  case 8:
    assert(!IsPCRelative && "NYI no support for 8 byte pc-relative");
    break;
  case 4:
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

extern "C" void EmitWinFrameInfo(ObjectWriter *OW, const char *FunctionName,
                                 int StartOffset, int EndOffset, int BlobSize,
                                 const char *BlobData,
                                 const char *PersonalityFunctionName,
                                 int LSDASize, const char *LSDA) {
  assert(OW && "ObjWriter is null");
  auto *AsmPrinter = &OW->getAsmPrinter();
  auto &OST = static_cast<MCObjectStreamer &>(*AsmPrinter->OutStreamer);
  MCContext &OutContext = OST.getContext();
  const MCObjectFileInfo *MOFI = OutContext.getObjectFileInfo();

  assert(MOFI->getObjectFileType() == MOFI->IsCOFF);

  // .xdata emission
  MCSection *Section = MOFI->getXDataSection();
  OST.SwitchSection(Section);
  OST.EmitValueToAlignment(4);

  MCSymbol *FrameSymbol = OutContext.createTempSymbol();
  OST.EmitLabel(FrameSymbol);

  EmitBlob(OW, BlobSize, BlobData);

  OST.EmitValueToAlignment(4);
  uint8_t flags = BlobData[0];
  // The chained info is not currently emitted, verify that we don't see it.
  assert((flags & (Win64EH::UNW_ChainInfo << 3)) == 0);
  if ((flags &
       (Win64EH::UNW_TerminateHandler | Win64EH::UNW_ExceptionHandler) << 3) !=
      0) {
    assert(PersonalityFunctionName != nullptr);
    const MCExpr *PersonalityFn = GetSymbolRefExpr(
        OW, PersonalityFunctionName, MCSymbolRefExpr::VK_COFF_IMGREL32);
    OST.EmitValue(PersonalityFn, 4);
  }

  if (LSDASize != 0) {
    EmitBlob(OW, LSDASize, LSDA);
  }

  // .pdata emission
  Section = MOFI->getPDataSection();
  OST.SwitchSection(Section);
  OST.EmitValueToAlignment(4);

  const MCExpr *BaseRefRel =
      GetSymbolRefExpr(OW, FunctionName, MCSymbolRefExpr::VK_COFF_IMGREL32);

  // start offset
  const MCExpr *StartOfs = MCConstantExpr::create(StartOffset, OutContext);
  OST.EmitValue(MCBinaryExpr::createAdd(BaseRefRel, StartOfs, OutContext), 4);

  // end offset
  const MCExpr *EndOfs = MCConstantExpr::create(EndOffset, OutContext);
  OST.EmitValue(MCBinaryExpr::createAdd(BaseRefRel, EndOfs, OutContext), 4);

  // frame symbol reference
  OST.EmitValue(MCSymbolRefExpr::create(
                    FrameSymbol, MCSymbolRefExpr::VK_COFF_IMGREL32, OutContext),
                4);
}

extern "C" void EmitCFIStart(ObjectWriter *OW, int Offset) {
  assert(OW && "ObjWriter is null");
  assert(!OW->FrameOpened && "frame should be closed before CFIStart");
  auto *AsmPrinter = &OW->getAsmPrinter();
  auto &OST = *AsmPrinter->OutStreamer;

  OST.EmitCFIStartProc(false);
  OW->FrameOpened = true;
}

extern "C" void EmitCFIEnd(ObjectWriter *OW, int Offset) {
  assert(OW && "ObjWriter is null");
  assert(OW->FrameOpened && "frame should be opened before CFIEnd");
  auto *AsmPrinter = &OW->getAsmPrinter();
  auto &OST = *AsmPrinter->OutStreamer;

  OST.EmitCFIEndProc();
  OW->FrameOpened = false;
}

extern "C" void EmitCFICode(ObjectWriter *OW, int Offset, const char *Blob) {
  assert(OW && "ObjWriter is null");
  assert(OW->FrameOpened && "frame should be opened before CFICode");
  auto *AsmPrinter = &OW->getAsmPrinter();
  auto &OST = *AsmPrinter->OutStreamer;

  CFI_CODE *CfiCode = (CFI_CODE *)Blob;
  switch (CfiCode->CfiOpCode) {
  case CFI_ADJUST_CFA_OFFSET:
    assert(CfiCode->DwarfReg == DWARF_REG_ILLEGAL &&
           "Unexpected Register Value for OpAdjustCfaOffset");
    OST.EmitCFIAdjustCfaOffset(CfiCode->Offset);
    break;
  case CFI_REL_OFFSET:
    OST.EmitCFIRelOffset(CfiCode->DwarfReg, CfiCode->Offset);
    break;
  case CFI_DEF_CFA_REGISTER:
    assert(CfiCode->Offset == 0 &&
           "Unexpected Offset Value for OpDefCfaRegister");
    OST.EmitCFIDefCfaRegister(CfiCode->DwarfReg);
    break;
  default:
    assert(!"Unrecognized CFI");
    break;
  }
}

static void EmitLabelDiff(MCStreamer &Streamer, const MCSymbol *From,
                          const MCSymbol *To, unsigned int Size = 4) {
  MCSymbolRefExpr::VariantKind Variant = MCSymbolRefExpr::VK_None;
  MCContext &Context = Streamer.getContext();
  const MCExpr *FromRef = MCSymbolRefExpr::create(From, Variant, Context),
               *ToRef = MCSymbolRefExpr::create(To, Variant, Context);
  const MCExpr *AddrDelta =
      MCBinaryExpr::create(MCBinaryExpr::Sub, ToRef, FromRef, Context);
  Streamer.EmitValue(AddrDelta, Size);
}

extern "C" void EmitDebugFileInfo(ObjectWriter *OW, int FileId,
                                  const char *FileName) {
  assert(OW && "ObjWriter is null");
  auto *AsmPrinter = &OW->getAsmPrinter();
  auto &OST = static_cast<MCObjectStreamer &>(*AsmPrinter->OutStreamer);
  MCContext &OutContext = OST.getContext();
  const MCObjectFileInfo *MOFI = OutContext.getObjectFileInfo();

  // TODO: Should convert this for non-Windows.
  if (MOFI->getObjectFileType() != MOFI->IsCOFF) {
    return;
  }

  assert(FileId > 0 && "FileId should be greater than 0.");
  OST.EmitCVFileDirective(FileId, FileName);
}

static void EmitPDBDebugVarInfo(MCObjectStreamer &OST, const MCSymbol *Fn,
                                DebugVarInfo LocInfos[], int NumVarInfos) {
  MCContext &OutContext = OST.getContext();
  const MCObjectFileInfo *MOFI = OutContext.getObjectFileInfo();
  assert(MOFI->getObjectFileType() == MOFI->IsCOFF);

  for (int i = 0; i < NumVarInfos; i++) {
    // Emit an S_LOCAL record
    DebugVarInfo var = LocInfos[i];
    LocalSym sym;

    memset(&sym, 0, sizeof(sym));

    int sizeofSym = sizeof(LocalSym);
    short rectyp = SymbolRecordKind::S_LOCAL;
    short reclen = sizeof(rectyp) + sizeofSym + var.name.length() + 1;
    TypeIndex typeIndex(var.typeIndex);
    sym.Type = typeIndex;

    if (var.isParam) {
      sym.Flags |= LocalSym::IsParameter;
    }

    OST.EmitIntValue(reclen, 2);
    OST.EmitIntValue(rectyp, 2);
    OST.EmitBytes(StringRef((char *)&sym, sizeofSym));
    OST.EmitBytes(StringRef(var.name.c_str(), var.name.length() + 1));

    for (const auto &range : *var.ranges) {
      assert(range.varNumber == var.varNumber);

      // Emit a range record
      LocalVariableAddrRange *prange = nullptr;

      switch (range.loc.vlType) {
      case ICorDebugInfo::VLT_REG:
      case ICorDebugInfo::VLT_REG_FP: {
        DefRangeRegisterSym rec;
        int sizeofRec = sizeof(rec);
        memset(&rec, 0, sizeofRec);
        short rectyp = SymbolRecordKind::S_DEFRANGE_REGISTER;
        short reclen = sizeofRec + sizeof(rectyp);
        rec.Range.OffsetStart = range.startOffset;
        rec.Range.Range = range.endOffset - range.startOffset;
        rec.Range.ISectStart = 0;
        rec.Register = cvRegMapAmd64[range.loc.vlReg.vlrReg];
        prange = &rec.Range;
        OST.EmitIntValue(reclen, 2);
        OST.EmitIntValue(rectyp, 2);
        OST.EmitBytes(
            StringRef((char *)&rec, offsetof(DefRangeRegisterSym, Range)));
        break;
      }

      case ICorDebugInfo::VLT_STK: {
        DefRangeRegisterRelSym rec;
        int sizeofRec = sizeof(DefRangeRegisterRelSym);
        memset(&rec, 0, sizeofRec);
        short rectyp = SymbolRecordKind::S_DEFRANGE_REGISTER_REL;
        short reclen = sizeofRec + sizeof(rectyp);
        rec.Range.OffsetStart = range.startOffset;
        rec.Range.Range = range.endOffset - range.startOffset;
        rec.Range.ISectStart = 0;
        rec.BaseRegister = cvRegMapAmd64[range.loc.vlStk.vlsBaseReg];
        rec.BasePointerOffset = range.loc.vlStk.vlsOffset;
        prange = &rec.Range;
        OST.EmitIntValue(reclen, 2);
        OST.EmitIntValue(rectyp, 2);
        OST.EmitBytes(
            StringRef((char *)&rec, offsetof(DefRangeRegisterRelSym, Range)));
        break;
      }

      case ICorDebugInfo::VLT_REG_BYREF:
      case ICorDebugInfo::VLT_STK_BYREF:
      case ICorDebugInfo::VLT_REG_REG:
      case ICorDebugInfo::VLT_REG_STK:
      case ICorDebugInfo::VLT_STK_REG:
      case ICorDebugInfo::VLT_STK2:
      case ICorDebugInfo::VLT_FPSTK:
      case ICorDebugInfo::VLT_FIXED_VA:
        // TODO: for optimized debugging
        break;

      default:
        _ASSERTE(!"Unknown varloc type!");
        break;
      }

      // Emit range
      if (prange != nullptr) {
        const MCSymbolRefExpr *baseSym =
            MCSymbolRefExpr::create(Fn, OST.getContext());
        const MCExpr *offset =
            MCConstantExpr::create(prange->OffsetStart, OST.getContext());
        const MCExpr *expr =
            MCBinaryExpr::createAdd(baseSym, offset, OST.getContext());
        OST.EmitCOFFSecRel32Value(expr);
        OST.EmitCOFFSectionIndex(Fn);
        OST.EmitIntValue(prange->Range, 2);
      }
    }
  }
}

static void EmitPDBDebugFunctionInfo(ObjectWriter *OW, const char *FunctionName,
                                     int FunctionSize) {
  assert(OW && "ObjWriter is null");
  auto *AsmPrinter = &OW->getAsmPrinter();
  auto &OST = static_cast<MCObjectStreamer &>(*AsmPrinter->OutStreamer);
  MCContext &OutContext = OST.getContext();
  const MCObjectFileInfo *MOFI = OutContext.getObjectFileInfo();
  assert(MOFI->getObjectFileType() == MOFI->IsCOFF);

  // Mark the end of function.
  MCSymbol *FnEnd = OutContext.createTempSymbol();
  OST.EmitLabel(FnEnd);

  MCSection *Section = MOFI->getCOFFDebugSymbolsSection();
  OST.SwitchSection(Section);
  // Emit debug section magic before the first entry.
  if (OW->FuncId == 1) {
    OST.EmitIntValue(COFF::DEBUG_SECTION_MAGIC, 4);
  }

  MCSymbol *Fn = OutContext.getOrCreateSymbol(Twine(FunctionName));

  // Emit a symbol subsection, required by VS2012+ to find function boundaries.
  MCSymbol *SymbolsBegin = OutContext.createTempSymbol(),
           *SymbolsEnd = OutContext.createTempSymbol();
  OST.EmitIntValue(unsigned(ModuleSubstreamKind::Symbols), 4);
  EmitLabelDiff(OST, SymbolsBegin, SymbolsEnd);
  OST.EmitLabel(SymbolsBegin);
  {
    MCSymbol *ProcSegmentBegin = OutContext.createTempSymbol(),
             *ProcSegmentEnd = OutContext.createTempSymbol();
    EmitLabelDiff(OST, ProcSegmentBegin, ProcSegmentEnd, 2);
    OST.EmitLabel(ProcSegmentBegin);

    OST.EmitIntValue(unsigned(SymbolRecordKind::S_GPROC32_ID), 2);
    // Some bytes of this segment don't seem to be required for basic debugging,
    // so just fill them with zeroes.
    OST.EmitFill(12, 0);
    // This is the important bit that tells the debugger where the function
    // code is located and what's its size:
    OST.EmitIntValue(FunctionSize, 4);
    OST.EmitFill(4, 0);                // SS_DBGSTART
    OST.EmitIntValue(FunctionSize, 4); // SS_DBGEND
    OST.EmitFill(4, 0);                // SS_TINDEX
    OST.EmitCOFFSecRel32(Fn);
    OST.EmitCOFFSectionIndex(Fn);

    // Emit flags, optimize debugging
    OST.EmitIntValue(0x80, 1);
    // Emit the function display name as a null-terminated string.
    OST.EmitBytes(FunctionName);
    OST.EmitIntValue(0, 1);
    OST.EmitLabel(ProcSegmentEnd);

    // Emit local var info
    int NumVarInfos = OW->DebugVarInfos.size();
    if (NumVarInfos > 0) {
      std::unique_ptr<DebugVarInfo[]> varInfos(new DebugVarInfo[NumVarInfos]);
      std::copy(OW->DebugVarInfos.begin(), OW->DebugVarInfos.end(),
                varInfos.get());
      EmitPDBDebugVarInfo(OST, Fn, varInfos.get(), NumVarInfos);
      OW->DebugVarInfos.clear();
    }

    // We're done with this function.
    OST.EmitIntValue(0x0002, 2);
    OST.EmitIntValue(unsigned(SymbolRecordKind::S_PROC_ID_END), 2);
  }

  OST.EmitLabel(SymbolsEnd);

  // Every subsection must be aligned to a 4-byte boundary.
  OST.EmitValueToAlignment(4);

  // We have an assembler directive that takes care of the whole line table.
  // We also increase function id for the next function.
  OST.EmitCVLinetableDirective(OW->FuncId++, Fn, FnEnd);
}

extern "C" void EmitDebugFunctionInfo(ObjectWriter *OW,
                                      const char *FunctionName,
                                      int FunctionSize) {
  assert(OW && "ObjWriter is null");
  auto *AsmPrinter = &OW->getAsmPrinter();
  auto &OST = static_cast<MCObjectStreamer &>(*AsmPrinter->OutStreamer);
  MCContext &OutContext = OST.getContext();
  const MCObjectFileInfo *MOFI = OutContext.getObjectFileInfo();

  if (MOFI->getObjectFileType() == MOFI->IsCOFF) {
    EmitPDBDebugFunctionInfo(OW, FunctionName, FunctionSize);
  } else {
    // TODO: Should convert this for non-Windows.
  }
}

extern "C" void EmitDebugVar(ObjectWriter *OW, char *name, int typeIndex,
                             bool isParm, int rangeCount, char *ranges) {
  assert(OW && "ObjWriter is null");

  if (rangeCount == 0)
    return;

  // Interpret ranges as an array of NativeVarInfo structs.
  ICorDebugInfo::NativeVarInfo *varInfos =
      (ICorDebugInfo::NativeVarInfo *)ranges;

  int varNumber = varInfos[0].varNumber;

  DebugVarInfo var = {varNumber, name, typeIndex, isParm, NULL};
  var.ranges = new std::vector<ICorDebugInfo::NativeVarInfo>();

  for (int i = 0; i < rangeCount; i++) {
    assert(var.varNumber == varInfos[i].varNumber);
    var.ranges->push_back(varInfos[i]);
  }

  OW->DebugVarInfos.push_back(var);
}

extern "C" void EmitDebugLoc(ObjectWriter *OW, int NativeOffset, int FileId,
                             int LineNumber, int ColNumber) {
  assert(OW && "ObjWriter is null");
  auto *AsmPrinter = &OW->getAsmPrinter();
  auto &OST = static_cast<MCObjectStreamer &>(*AsmPrinter->OutStreamer);
  MCContext &OutContext = OST.getContext();
  const MCObjectFileInfo *MOFI = OutContext.getObjectFileInfo();

  // TODO: Should convert this for non-Windows.
  if (MOFI->getObjectFileType() != MOFI->IsCOFF) {
    return;
  }

  assert(FileId > 0 && "FileId should be greater than 0.");
  OST.EmitCVLocDirective(OW->FuncId, FileId, LineNumber, ColNumber, false, true,
                         "");
}

// This should be invoked at the end of module emission to finalize
// debug module info.
extern "C" void EmitDebugModuleInfo(ObjectWriter *OW) {
  assert(OW && "ObjWriter is null");
  auto *AsmPrinter = &OW->getAsmPrinter();
  auto &OST = static_cast<MCObjectStreamer &>(*AsmPrinter->OutStreamer);
  MCContext &OutContext = OST.getContext();
  const MCObjectFileInfo *MOFI = OutContext.getObjectFileInfo();

  // TODO: Should convert this for non-Windows.
  if (MOFI->getObjectFileType() != MOFI->IsCOFF) {
    return;
  }

  MCSection *Section = MOFI->getCOFFDebugSymbolsSection();
  OST.SwitchSection(Section);
  OST.EmitCVFileChecksumsDirective();
  OST.EmitCVStringTableDirective();
}