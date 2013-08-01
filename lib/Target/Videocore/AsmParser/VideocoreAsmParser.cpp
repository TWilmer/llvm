//===-- VideocoreAsmParser.cpp - Parse Videocore assembly instructions --------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/VideocoreMCTargetDesc.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCParser/MCParsedAsmOperand.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/MCTargetAsmParser.h"
#include "llvm/Support/TargetRegistry.h"

using namespace llvm;

// Return true if Expr is in the range [MinValue, MaxValue].
static bool inRange(const MCExpr *Expr, int64_t MinValue, int64_t MaxValue) {
  if (const MCConstantExpr *CE = dyn_cast<MCConstantExpr>(Expr)) {
    int64_t Value = CE->getValue();
    return Value >= MinValue && Value <= MaxValue;
  }
  return false;
}

namespace {
class VideocoreOperand : public MCParsedAsmOperand {
public:
  enum RegisterKind {
    ScalarReg
  };

private:
  enum OperandKind {
    KindToken,
    KindReg,
    KindAccessReg,
    KindImm,
    KindMem
  };

  OperandKind Kind;
  SMLoc StartLoc, EndLoc;

  // A string of length Length, starting at Data.
  struct TokenOp {
    const char *Data;
    unsigned Length;
  };

  // LLVM register Num, which has kind Kind.
  struct RegOp {
    RegisterKind Kind;
    unsigned Num;
  };

  union {
    TokenOp Token;
    RegOp Reg;
    unsigned AccessReg;
    const MCExpr *Imm;
  };

  VideocoreOperand(OperandKind kind, SMLoc startLoc, SMLoc endLoc)
    : Kind(kind), StartLoc(startLoc), EndLoc(endLoc)
  {}

  void addExpr(MCInst &Inst, const MCExpr *Expr) const {
    // Add as immediates when possible.  Null MCExpr = 0.
    if (Expr == 0)
      Inst.addOperand(MCOperand::CreateImm(0));
    else if (const MCConstantExpr *CE = dyn_cast<MCConstantExpr>(Expr))
      Inst.addOperand(MCOperand::CreateImm(CE->getValue()));
    else
      Inst.addOperand(MCOperand::CreateExpr(Expr));
  }

public:
  
  // Create particular kinds of operand.
  static VideocoreOperand *createToken(StringRef Str, SMLoc Loc) {
    VideocoreOperand *Op = new VideocoreOperand(KindToken, Loc, Loc);
    Op->Token.Data = Str.data();
    Op->Token.Length = Str.size();
    return Op;
  }
  static VideocoreOperand *createReg(int Num,
                                   SMLoc StartLoc, SMLoc EndLoc) {
    VideocoreOperand *Op = new VideocoreOperand(KindReg, StartLoc, EndLoc);
    Op->Reg.Kind = ScalarReg;
    Op->Reg.Num = Num;
    return Op;
  }
  static VideocoreOperand *createImm(const MCExpr *Expr, SMLoc StartLoc,
                                   SMLoc EndLoc) {
    VideocoreOperand *Op = new VideocoreOperand(KindImm, StartLoc, EndLoc);
    Op->Imm = Expr;
    return Op;
  }
  
  // Token operands
  virtual bool isToken() const LLVM_OVERRIDE {
    return Kind == KindToken;
  }
  StringRef getToken() const {
    assert(Kind == KindToken && "Not a token");
    return StringRef(Token.Data, Token.Length);
  }

  // Register operands.
  virtual bool isReg() const LLVM_OVERRIDE {
    return Kind == KindReg;
  }
  /*bool isReg(RegisterKind RegKind) const {
    return Kind == KindReg && Reg.Kind == RegKind;
  }*/
  virtual unsigned getReg() const LLVM_OVERRIDE {
    assert(Kind == KindReg && "Not a register");
    return Reg.Num;
  }

  // Immediate operands.
  virtual bool isImm() const LLVM_OVERRIDE {
    return Kind == KindImm;
  }
  /*bool isImm(int64_t MinValue, int64_t MaxValue) const {
    return Kind == KindImm && inRange(Imm, MinValue, MaxValue);
  }*/
  const MCExpr *getImm() const {
    assert(Kind == KindImm && "Not an immediate");
    return Imm;
  }

  virtual bool isCondCode() const {
    return true; // TODO: implement this
  }

  // Memory operands.
  virtual bool isMem() const LLVM_OVERRIDE {
    return Kind == KindMem;
  }
  
  // Override MCParsedAsmOperand.
  virtual SMLoc getStartLoc() const LLVM_OVERRIDE { return StartLoc; }
  virtual SMLoc getEndLoc() const LLVM_OVERRIDE { return EndLoc; }
  virtual void print(raw_ostream &OS) const LLVM_OVERRIDE;


  // Used by the TableGen code to add particular types of operand
  // to an instruction.
  void addRegOperands(MCInst &Inst, unsigned N) const {
    assert(N == 1 && "Invalid number of operands");
    Inst.addOperand(MCOperand::CreateReg(getReg()));
  }
  void addImmOperands(MCInst &Inst, unsigned N) const {
    assert(N == 1 && "Invalid number of operands");
    addExpr(Inst, getImm());
  }
  void addCondCodeOperands(MCInst &Inst, unsigned N) const {
    assert(N == 1 && "Invalid number of operands");
    // TODO: do something here
  }

};

class VideocoreAsmParser : public MCTargetAsmParser {
#define GET_ASSEMBLER_HEADER
#include "VideocoreGenAsmMatcher.inc"

private:
  MCSubtargetInfo &STI;
  MCAsmParser &Parser;
  struct Register {
    char Prefix;
    unsigned Number;
    SMLoc StartLoc, EndLoc;
  };

  int tryParseRegister();
  bool tryParseRegisterWithWriteBack(SmallVectorImpl<MCParsedAsmOperand*> &Operands);

  bool parseOperand(SmallVectorImpl<MCParsedAsmOperand*> &Operands,
                    StringRef Mnemonic);

public:
  enum VideocoreMatchResultTy {
    Match_NullMatchType = FIRST_TARGET_MATCH_RESULT_TY,
#define GET_OPERAND_DIAGNOSTIC_TYPES
#include "VideocoreGenAsmMatcher.inc"

  };

  VideocoreAsmParser(MCSubtargetInfo &sti, MCAsmParser &parser)
    : MCTargetAsmParser(), STI(sti), Parser(parser) {
    MCAsmParserExtension::Initialize(Parser);

    // Initialize the set of available features.
    setAvailableFeatures(ComputeAvailableFeatures(STI.getFeatureBits()));
  }

  // Override MCTargetAsmParser.
  virtual bool ParseDirective(AsmToken DirectiveID) LLVM_OVERRIDE;
  virtual bool ParseRegister(unsigned &RegNo, SMLoc &StartLoc,
                             SMLoc &EndLoc) LLVM_OVERRIDE;
  virtual bool ParseInstruction(ParseInstructionInfo &Info,
                                StringRef Name, SMLoc NameLoc,
                                SmallVectorImpl<MCParsedAsmOperand*> &Operands)
    LLVM_OVERRIDE;
  virtual bool
    MatchAndEmitInstruction(SMLoc IDLoc, unsigned &Opcode,
                            SmallVectorImpl<MCParsedAsmOperand*> &Operands,
                            MCStreamer &Out, unsigned &ErrorInfo,
                            bool MatchingInlineAsm) LLVM_OVERRIDE;

};
}

#define GET_REGISTER_MATCHER
#define GET_SUBTARGET_FEATURE_NAME
#define GET_MATCHER_IMPLEMENTATION
#include "VideocoreGenAsmMatcher.inc"

void VideocoreOperand::print(raw_ostream &OS) const {
  llvm_unreachable("Not implemented");
}

bool VideocoreAsmParser::ParseDirective(AsmToken DirectiveID) {
  return true;
}

bool VideocoreAsmParser::ParseRegister(unsigned &RegNo, SMLoc &StartLoc,
                                     SMLoc &EndLoc) {
  StartLoc = Parser.getTok().getLoc();
  EndLoc = Parser.getTok().getEndLoc();
  RegNo = tryParseRegister();

  return (RegNo == (unsigned)-1);
}

/// Try to parse a register name.  The token must be an Identifier when called,
/// and if it is a register name the token is eaten and the register number is
/// returned.  Otherwise return -1.
///
int VideocoreAsmParser::tryParseRegister() {
  const AsmToken &Tok = Parser.getTok();
  if (Tok.isNot(AsmToken::Identifier)) return -1;

  std::string lowerCase = Tok.getString().lower();
  unsigned RegNum = MatchRegisterName(lowerCase);
  if (!RegNum) {
    RegNum = StringSwitch<unsigned>(lowerCase)
      .Case("r25", VC::SP)
      .Case("r26", VC::LR)
      .Case("r30", VC::SR)
      .Case("r31", VC::PC)
      .Default(0);
  }

  if (!RegNum)
    return -1;

  Parser.Lex();

  return RegNum;
}

/// Try to parse a register name.  The token must be an Identifier when called.
/// If it's a register, an AsmOperand is created. Another AsmOperand is created
/// if there is a "writeback". 'true' if it's not a register.
///
/// TODO this is likely to change to allow different register types and or to
/// parse for a specific register type.
bool VideocoreAsmParser::
tryParseRegisterWithWriteBack(SmallVectorImpl<MCParsedAsmOperand*> &Operands) {
  const AsmToken &RegTok = Parser.getTok();
  int RegNo = tryParseRegister();
  if (RegNo == -1)
    return true;

  Operands.push_back(VideocoreOperand::createReg(RegNo, RegTok.getLoc(),
                                           RegTok.getEndLoc()));

  return false;
}

bool VideocoreAsmParser::
ParseInstruction(ParseInstructionInfo &Info, StringRef Name, SMLoc NameLoc,
                 SmallVectorImpl<MCParsedAsmOperand*> &Operands) {
  Operands.push_back(VideocoreOperand::createToken(Name, NameLoc));

  // Read the remaining operands.
  if (getLexer().isNot(AsmToken::EndOfStatement)) {
    // Read the first operand.
    if (parseOperand(Operands, Name)) {
      Parser.eatToEndOfStatement();
      return true;
    }

    // Read any subsequent operands.
    while (getLexer().is(AsmToken::Comma)) {
      Parser.Lex();
      if (parseOperand(Operands, Name)) {
        Parser.eatToEndOfStatement();
        return true;
      }
    }
    if (getLexer().isNot(AsmToken::EndOfStatement)) {
      SMLoc Loc = getLexer().getLoc();
      Parser.eatToEndOfStatement();
      return Error(Loc, "unexpected token in argument list");
    }
  }

  // Consume the EndOfStatement.
  Parser.Lex();
  return false;
}

bool VideocoreAsmParser::
parseOperand(SmallVectorImpl<MCParsedAsmOperand*> &Operands,
             StringRef Mnemonic) {
  SMLoc S, E;

  /*
  // Check if the current operand has a custom associated parser, if so, try to
  // custom parse the operand, or fallback to the general approach.
  OperandMatchResultTy ResTy = MatchOperandParserImpl(Operands, Mnemonic);
  if (ResTy == MatchOperand_Success)
    return false;

  // If there wasn't a custom match, try the generic matcher below. Otherwise,
  // there was a match, but an error occurred, in which case, just return that
  // the operand parsing failed.
  if (ResTy == MatchOperand_ParseFail)
    return true;
  */

  switch (getLexer().getKind()) {
  default:
    Error(Parser.getTok().getLoc(), "unexpected token in operand");
    return true;
  case AsmToken::Identifier: {
    // If we've seen a branch mnemonic, the next operand must be a label.  This
    // is true even if the label is a register name.  So "br r1" means branch to
    // label "r1".
    bool ExpectLabel = Mnemonic == "b" || Mnemonic == "bl";
    if (!ExpectLabel) {
      if (!tryParseRegisterWithWriteBack(Operands))
        return false;
    }

    // Fall though for the Identifier case that is not a register or a
    // special name.
  }
  //case AsmToken::LParen:  // parenthesized expressions like (_strcmp-4)
  case AsmToken::Integer: // immediates
  case AsmToken::Minus:   // negtiave immediates
  case AsmToken::Dot: {   // . as a branch target
    // This was not a register so parse other operands that start with an
    // identifier (like labels) as expressions and create them as immediates.
    const MCExpr *IdVal;
    S = Parser.getTok().getLoc();
    if (getParser().parseExpression(IdVal))
      return true;
    E = SMLoc::getFromPointer(Parser.getTok().getLoc().getPointer() - 1);
    Operands.push_back(VideocoreOperand::createImm(IdVal, S, E));
    return false;
  }
  case AsmToken::LParen: { // Memory Operand
    Parser.Lex();

    if(tryParseRegisterWithWriteBack(Operands)) { // base
      Error(Parser.getTok().getLoc(), "expected register");
      return true;
    }
    const MCExpr *disp;
    if(!getLexer().is(AsmToken::RParen)) {
      if(tryParseRegisterWithWriteBack(Operands)) { // index
        S = Parser.getTok().getLoc();
        if (!getLexer().is(AsmToken::Plus) && !getLexer().is(AsmToken::Minus)) {
           Error(Parser.getTok().getLoc(), "expected '+' or '-'");
           return true;
        }
        // Don't swallow the '+' or '-', the expression will parse it
        if (getParser().parseExpression(disp)) { // displacement
          Error(Parser.getTok().getLoc(), "problem parsing expression");
          return true;
        }
        E = SMLoc::getFromPointer(Parser.getTok().getLoc().getPointer() - 1);
        Operands.push_back(VideocoreOperand::createImm(disp, S, E));
      }
    }
    if(!getLexer().is(AsmToken::RParen)) {
      Error(Parser.getTok().getLoc(), "expected ')'");
      return true;
    }
    Parser.Lex();
    return false;
  }
  }

}

bool VideocoreAsmParser::
MatchAndEmitInstruction(SMLoc IDLoc, unsigned &Opcode,
                        SmallVectorImpl<MCParsedAsmOperand*> &Operands,
                        MCStreamer &Out, unsigned &ErrorInfo,
                        bool MatchingInlineAsm) {
  MCInst Inst;
  unsigned MatchResult;

  MatchResult = MatchInstructionImpl(Operands, Inst, ErrorInfo,
                                     MatchingInlineAsm);
  switch (MatchResult) {
  default: break;
  case Match_Success:
    Inst.setLoc(IDLoc);
    Out.EmitInstruction(Inst);
    return false;

  case Match_MissingFeature: {
    assert(ErrorInfo && "Unknown missing feature!");
    // Special case the error message for the very common case where only
    // a single subtarget feature is missing
    std::string Msg = "instruction requires:";
    unsigned Mask = 1;
    for (unsigned I = 0; I < sizeof(ErrorInfo) * 8 - 1; ++I) {
      if (ErrorInfo & Mask) {
        Msg += " ";
        Msg += getSubtargetFeatureName(ErrorInfo & Mask);
      }
      Mask <<= 1;
    }
    return Error(IDLoc, Msg);
  }

  case Match_InvalidOperand: {
    SMLoc ErrorLoc = IDLoc;
    if (ErrorInfo != ~0U) {
      if (ErrorInfo >= Operands.size())
        return Error(IDLoc, "too few operands for instruction");

      ErrorLoc = ((VideocoreOperand*)Operands[ErrorInfo])->getStartLoc();
      if (ErrorLoc == SMLoc())
        ErrorLoc = IDLoc;
    }
    return Error(ErrorLoc, "invalid operand for instruction");
  }

  case Match_MnemonicFail:
    return Error(IDLoc, "invalid instruction");
  }

  llvm_unreachable("Unexpected match type");
}

// Force static initialization.
extern "C" void LLVMInitializeVideocoreAsmParser() {
  RegisterMCAsmParser<VideocoreAsmParser> X(TheVideocoreTarget);
}