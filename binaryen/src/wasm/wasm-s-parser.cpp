/*
 * Copyright 2015 WebAssembly Community Group participants
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "wasm-s-parser.h"

#include <cmath>
#include <cctype>
#include <limits>

#include "asm_v_wasm.h"
#include "asmjs/shared-constants.h"
#include "ir/branch-utils.h"
#include "shared-constants.h"
#include "wasm-binary.h"
#include "wasm-builder.h"

#define abort_on(str) { throw ParseException(std::string("abort_on ") + str); }
#define element_assert(condition) assert((condition) ? true : (std::cerr << "on: " << *this << '\n' && 0));

using cashew::IString;

namespace {
int unhex(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  throw wasm::ParseException("invalid hexadecimal");
}
}

namespace wasm {

static Address getCheckedAddress(const Element* s, const char* errorText) {
  uint64_t num = atoll(s->c_str());
  if (num > std::numeric_limits<Address::address_t>::max()) {
    throw ParseException(errorText, s->line, s->col);
  }
  return num;
}

Element::List& Element::list() {
  if (!isList()) throw ParseException("expected list", line, col);
  return list_;
}

Element* Element::operator[](unsigned i) {
  if (!isList()) throw ParseException("expected list", line, col);
  if (i >= list().size()) {
    throw ParseException("expected more elements in list", line, col);
  }
  return list()[i];
}

IString Element::str() const {
  if (!isStr()) throw ParseException("expected string", line, col);
  return str_;
}

const char* Element::c_str() const {
  if (!isStr()) throw ParseException("expected string", line, col);
  return str_.str;
}

Element* Element::setString(IString str__, bool dollared__, bool quoted__) {
  isList_ = false;
  str_ = str__;
  dollared_ = dollared__;
  quoted_ = quoted__;
  return this;
}

Element* Element::setMetadata(size_t line_, size_t col_, SourceLocation* loc_) {
  line = line_;
  col = col_;
  loc = loc_;
  return this;
}

std::ostream& operator<<(std::ostream& o, Element& e) {
  if (e.isList_) {
    o << '(';
    for (auto item : e.list_) o << ' ' << *item;
    o << " )";
  } else {
    o << e.str_.str;
  }
  return o;
}

void Element::dump() {
  std::cout << "dumping " << this << " : " << *this << ".\n";
}


SExpressionParser::SExpressionParser(char* input) : input(input), loc(nullptr) {
  root = nullptr;
  line = 1;
  lineStart = input;
  while (!root) { // keep parsing until we pass an initial comment
    root = parse();
  }
}

Element* SExpressionParser::parse() {
  std::vector<Element *> stack;
  std::vector<SourceLocation*> stackLocs;
  Element *curr = allocator.alloc<Element>();
  while (1) {
    skipWhitespace();
    if (input[0] == 0) break;
    if (input[0] == '(') {
      input++;
      stack.push_back(curr);
      curr = allocator.alloc<Element>()->setMetadata(line, input - lineStart - 1, loc);
      stackLocs.push_back(loc);
      /*
      if(stack.size() != stackLocs.size())
      {
        std::cout<< " stack.size() :::" << stack.size() << std::endl;
        std::cout<< "  stackLocs.size() :::" <<  stackLocs.size() << std::endl;
      }*/
      assert(stack.size() == stackLocs.size());
    } else if (input[0] == ')') {
      input++;
      auto last = curr;
      if (stack.empty()) {
        continue;
        //throw ParseException("s-expr stack empty");
      }
      curr = stack.back();
      assert(stack.size() == stackLocs.size());
      stack.pop_back();
      loc = stackLocs.back();
      stackLocs.pop_back();
      curr->list().push_back(last);
    } else {
      curr->list().push_back(parseString());
    }
  }
  if (stack.size() != 0) throw ParseException("stack is not empty", curr->line, curr->col);
  return curr;
}

void SExpressionParser::parseDebugLocation() {
  // Extracting debug location (if valid)
  char* debugLoc = input + 3; // skipping ";;@"
  while (debugLoc[0] && debugLoc[0] == ' ') debugLoc++;
  char* debugLocEnd = debugLoc;
  while (debugLocEnd[0] && debugLocEnd[0] != '\n') debugLocEnd++;
  char* pos = debugLoc;
  while (pos < debugLocEnd && pos[0] != ':') pos++;
  if (pos >= debugLocEnd) {
    return; // no line number
  }
  std::string name(debugLoc, pos);
  char* lineStart = ++pos;
  while (pos < debugLocEnd && pos[0] != ':') pos++;
  std::string lineStr(lineStart, pos);
  if (pos >= debugLocEnd) {
    return; // no column number
  }
  std::string colStr(++pos, debugLocEnd);
  void* buf = allocator.allocSpace(sizeof(SourceLocation));
  loc = new (buf) SourceLocation(IString(name.c_str(), false), atoi(lineStr.c_str()), atoi(colStr.c_str()));
}

void SExpressionParser::skipWhitespace() {
  while (1) {
    while (isspace(input[0])) {
      if (input[0] == '\n') {
        line++;
        lineStart = input + 1;
      }
      input++;
    }
    if (input[0] == ';' && input[1] == ';') {
      if (input[2] == '@') {
        parseDebugLocation();
      }
      while (input[0] && input[0] != '\n') input++;
      line++;
      lineStart = ++input;
    } else if (input[0] == '(' && input[1] == ';') {
      // Skip nested block comments.
      input += 2;
      int depth = 1;
      while (1) {
        if (input[0] == 0) {
          return;
        }
        if (input[0] == '(' && input[1] == ';') {
          input += 2;
          depth++;
        } else if (input[0] == ';' && input[1] == ')') {
          input += 2;
          --depth;
          if (depth == 0) {
            break;
          }
        } else if (input[0] == '\n') {
          line++;
          lineStart = input;
          input++;
        } else {
          input++;
        }
      }
    } else {
      return;
    }
  }
}

Element* SExpressionParser::parseString() {
  bool dollared = false;
  if (input[0] == '$') {
    input++;
    dollared = true;
  }
  char *start = input;
  if (input[0] == '"') {
    // parse escaping \", but leave code escaped - we'll handle escaping in memory segments specifically
    input++;
    std::string str;
    while (1) {
      if (input[0] == 0) throw ParseException("unterminated string", line, start - lineStart);
      if (input[0] == '"') break;
      if (input[0] == '\\') {
        str += input[0];
        if (input[1] == 0) throw ParseException("unterminated string escape", line, start - lineStart);
        str += input[1];
        input += 2;
        continue;
      }
      str += input[0];
      input++;
    }
    input++;
    return allocator.alloc<Element>()->setString(IString(str.c_str(), false), dollared, true)->setMetadata(line, start - lineStart, loc);
  }
  while (input[0] && !isspace(input[0]) && input[0] != ')' && input[0] != '(' && input[0] != ';') input++;
  if (start == input) throw ParseException("expected string", line, input - lineStart);
  char temp = input[0];
  input[0] = 0;
  auto ret = allocator.alloc<Element>()->setString(IString(start, false), dollared, false)->setMetadata(line, start - lineStart, loc);
  input[0] = temp;
  return ret;
}

SExpressionWasmBuilder::SExpressionWasmBuilder(Module& wasm, Element& module, Name* moduleName) : wasm(wasm), allocator(wasm.allocator), globalCounter(0) {
  if (module.size() == 0) throw ParseException("empty toplevel, expected module");
  if (module[0]->str() != MODULE) throw ParseException("toplevel does not start with module");
  if (module.size() == 1) return;
  Index i = 1;
  if (module[i]->dollared()) {
    if (moduleName) {
      *moduleName = module[i]->str();
    }
    i++;
  }
  if (i < module.size() && module[i]->isStr()) {
    // these s-expressions contain a binary module, actually
    std::vector<char> data;
    while (i < module.size()) {
      auto str = module[i++]->c_str();
      if (auto size = strlen(str)) {
        stringToBinary(str, size, data);
      }
    }
    WasmBinaryBuilder binaryBuilder(wasm, data, false);
    binaryBuilder.read();
    return;
  }
  Index implementedFunctions = 0;
  functionCounter = 0;
  for (unsigned j = i; j < module.size(); j++) {
    auto& s = *module[j];
    preParseFunctionType(s);
    preParseImports(s);
    if (s[0]->str() == FUNC && !isImport(s)) {
      implementedFunctions++;
    }
  }
  functionCounter -= implementedFunctions; // we go through the functions again, now parsing them, and the counter begins from where imports ended
  for (unsigned j = i; j < module.size(); j++) {
    parseModuleElement(*module[j]);
  }
}

bool SExpressionWasmBuilder::isImport(Element& curr) {
  for (Index i = 0; i < curr.size(); i++) {
    auto& x = *curr[i];
    if (x.isList() && x.size() > 0 && x[0]->isStr() && x[0]->str() == IMPORT) return true;
  }
  return false;
}

void SExpressionWasmBuilder::preParseImports(Element& curr) {
  IString id = curr[0]->str();
  if (id == IMPORT) parseImport(curr);
  if (isImport(curr)) {
    if (id == FUNC) parseFunction(curr, true /* preParseImport */);
    else if (id == GLOBAL) parseGlobal(curr, true /* preParseImport */);
    else if (id == TABLE) parseTable(curr, true /* preParseImport */);
    else if (id == MEMORY) parseMemory(curr, true /* preParseImport */);
    else throw ParseException("fancy import we don't support yet", curr.line, curr.col);
  }
}

void SExpressionWasmBuilder::parseModuleElement(Element& curr) {
  if (isImport(curr)) return; // already done
  IString id = curr[0]->str();
  if (id == START) return parseStart(curr);
  if (id == FUNC) return parseFunction(curr);
  if (id == MEMORY) return parseMemory(curr);
  if (id == DATA) return parseData(curr);
  if (id == EXPORT) return parseExport(curr);
  if (id == IMPORT) return; // already done
  if (id == GLOBAL) return parseGlobal(curr);
  if (id == TABLE) return parseTable(curr);
  if (id == ELEM) return parseElem(curr);
  if (id == TYPE) return; // already done
  std::cerr << "bad module element " << id.str << '\n';
  throw ParseException("unknown module element", curr.line, curr.col);
}

Name SExpressionWasmBuilder::getFunctionName(Element& s) {
  if (s.dollared()) {
    return s.str();
  } else {
    // index
    size_t offset = atoi(s.str().c_str());
    if (offset >= functionNames.size()) throw ParseException("unknown function in getFunctionName");
    return functionNames[offset];
  }
}

Name SExpressionWasmBuilder::getFunctionTypeName(Element& s) {
  if (s.dollared()) {
    return s.str();
  } else {
    // index
    size_t offset = atoi(s.str().c_str());
    if (offset >= functionTypeNames.size()) throw ParseException("unknown function type in getFunctionTypeName");
    return functionTypeNames[offset];
  }
}

Name SExpressionWasmBuilder::getGlobalName(Element& s) {
  if (s.dollared()) {
    return s.str();
  } else {
    // index
    size_t offset = atoi(s.str().c_str());
    if (offset >= globalNames.size()) throw ParseException("unknown global in getGlobalName");
    return globalNames[offset];
  }
}


void SExpressionWasmBuilder::preParseFunctionType(Element& s) {
  IString id = s[0]->str();
  if (id == TYPE) return parseType(s);
  if (id != FUNC) return;
  size_t i = 1;
  Name name, exportName;
  i = parseFunctionNames(s, name, exportName);
  if (!name.is()) {
    // unnamed, use an index
    name = Name::fromInt(functionCounter);
  }
  functionNames.push_back(name);
  functionCounter++;
  FunctionType* type = nullptr;
  functionTypes[name] = none;
  std::vector<Type> params;
  for (;i < s.size(); i++) {
    Element& curr = *s[i];
    IString id = curr[0]->str();
    if (id == RESULT) {
      if (curr.size() > 2) throw ParseException("invalid result arity", curr.line, curr.col);
      functionTypes[name] = stringToType(curr[1]->str());
    } else if (id == TYPE) {
      Name typeName = getFunctionTypeName(*curr[1]);
      if (!wasm.getFunctionTypeOrNull(typeName)) throw ParseException("unknown function type", curr.line, curr.col);
      type = wasm.getFunctionType(typeName);
      functionTypes[name] = type->result;
    } else if (id == PARAM && curr.size() > 1) {
      Index j = 1;
      if (curr[j]->dollared()) {
        // dollared input symbols cannot be types
        params.push_back(stringToType(curr[j + 1]->str(), true));
      } else {
        while (j < curr.size()) {
          params.push_back(stringToType(curr[j++]->str(), true));
        }
      }
    }
  }
  if (!type) {
    // if no function type provided, generate one, but reuse a previous one with the
    // right structure if there is one.
    // see https://github.com/WebAssembly/spec/pull/301
    bool need = true;
    std::unique_ptr<FunctionType> functionType = make_unique<FunctionType>();
    functionType->result = functionTypes[name];
    functionType->params = std::move(params);
    for (auto& existing : wasm.functionTypes) {
      if (existing->structuralComparison(*functionType)) {
        need = false;
        break;
      }
    }
    if (need) {
      functionType->name = Name::fromInt(wasm.functionTypes.size());
      functionTypeNames.push_back(functionType->name);
      if (wasm.getFunctionTypeOrNull(functionType->name)) throw ParseException("duplicate function type", s.line, s.col);
      wasm.addFunctionType(functionType.release());
    }
  }
}

size_t SExpressionWasmBuilder::parseFunctionNames(Element& s, Name& name, Name& exportName) {
  size_t i = 1;
  while (i < s.size() && i < 3 && s[i]->isStr()) {
    if (s[i]->quoted()) {
      // an export name
      exportName = s[i]->str();
      i++;
    } else if (s[i]->dollared()) {
      name = s[i]->str();
      i++;
    } else {
      break;
    }
  }
  if (i < s.size() && s[i]->isList()) {
    auto& inner = *s[i];
    if (inner.size() > 0 && inner[0]->str() == EXPORT) {
      exportName = inner[1]->str();
      i++;
    }
  }
#if 0
  if (exportName.is() && !name.is()) {
    name = exportName; // useful for debugging
  }
#endif
  return i;
}

void SExpressionWasmBuilder::parseFunction(Element& s, bool preParseImport) {
  size_t i = 1;
  Name name, exportName;
  i = parseFunctionNames(s, name, exportName);
  if (!preParseImport) {
    if (!name.is()) {
      // unnamed, use an index
      name = Name::fromInt(functionCounter);
    }
    functionCounter++;
  } else {
    // just preparsing, functionCounter was incremented by preParseFunctionType
    if (!name.is()) {
      // unnamed, use an index
      name = functionNames[functionCounter - 1];
    }
  }
  if (exportName.is()) {
    auto ex = make_unique<Export>();
    ex->name = exportName;
    ex->value = name;
    ex->kind = ExternalKind::Function;
    if (wasm.getExportOrNull(ex->name)) throw ParseException("duplicate export", s.line, s.col);
    wasm.addExport(ex.release());
  }
  Expression* body = nullptr;
  localIndex = 0;
  otherIndex = 0;
  brokeToAutoBlock = false;
  std::vector<NameType> typeParams; // we may have both params and a type. store the type info here
  std::vector<NameType> params;
  std::vector<NameType> vars;
  Type result = none;
  Name type;
  Block* autoBlock = nullptr; // we may need to add a block for the very top level
  Name importModule, importBase;
  auto makeFunction = [&]() {
    currFunction = std::unique_ptr<Function>(Builder(wasm).makeFunction(
        name,
        std::move(params),
        result,
        std::move(vars)
    ));
  };
  auto ensureAutoBlock = [&]() {
    if (!autoBlock) {
      autoBlock = allocator.alloc<Block>();
      autoBlock->list.push_back(body);
      body = autoBlock;
    }
  };
  for (;i < s.size(); i++) {
    Element& curr = *s[i];
    IString id = curr[0]->str();
    if (id == PARAM || id == LOCAL) {
      size_t j = 1;
      while (j < curr.size()) {
        IString name;
        Type type = none;
        if (!curr[j]->dollared()) { // dollared input symbols cannot be types
          type = stringToType(curr[j]->str(), true);
        }
        if (type != none) {
          // a type, so an unnamed parameter
          name = Name::fromInt(localIndex);
        } else {
          name = curr[j]->str();
          type = stringToType(curr[j+1]->str());
          j++;
        }
        j++;
        if (id == PARAM) {
          params.emplace_back(name, type);
        } else {
          vars.emplace_back(name, type);
        }
        localIndex++;
        currLocalTypes[name] = type;
      }
    } else if (id == RESULT) {
      if (curr.size() > 2) throw ParseException("invalid result arity", curr.line, curr.col);
      result = stringToType(curr[1]->str());
    } else if (id == TYPE) {
      Name name = getFunctionTypeName(*curr[1]);
      type = name;
      if (!wasm.getFunctionTypeOrNull(name)) throw ParseException("unknown function type");
      FunctionType* type = wasm.getFunctionType(name);
      result = type->result;
      for (size_t j = 0; j < type->params.size(); j++) {
        IString name = Name::fromInt(j);
        Type currType = type->params[j];
        typeParams.emplace_back(name, currType);
        currLocalTypes[name] = currType;
      }
    } else if (id == IMPORT) {
      importModule = curr[1]->str();
      importBase = curr[2]->str();
    } else {
      // body
      if (typeParams.size() > 0 && params.size() == 0) {
        params = typeParams;
      }
      if (!currFunction) makeFunction();
      Expression* ex = parseExpression(curr);
      if (!body) {
        body = ex;
      } else {
        ensureAutoBlock();
        autoBlock->list.push_back(ex);
      }
    }
  }
  // see https://github.com/WebAssembly/spec/pull/301
  if (type.isNull()) {
    // if no function type name provided, then we generated one
    std::unique_ptr<FunctionType> functionType = std::unique_ptr<FunctionType>(sigToFunctionType(getSigFromStructs(result, params)));
    for (auto& existing : wasm.functionTypes) {
      if (existing->structuralComparison(*functionType)) {
        type = existing->name;
        break;
      }
    }
    if (!type.is()) throw ParseException("no function type [internal error?]", s.line, s.col);
  }
  if (importModule.is()) {
    // this is an import, actually
    if (!preParseImport) throw ParseException("!preParseImport in func");
    std::unique_ptr<Import> im = make_unique<Import>();
    im->name = name;
    im->module = importModule;
    im->base = importBase;
    im->kind = ExternalKind::Function;
    im->functionType = wasm.getFunctionType(type)->name;
    if (wasm.getImportOrNull(im->name)) throw ParseException("duplicate import", s.line, s.col);
    wasm.addImport(im.release());
    if (currFunction) throw ParseException("import module inside function dec");
    currLocalTypes.clear();
    nameMapper.clear();
    return;
  }
  if (preParseImport) throw ParseException("preParseImport in func");
  if (brokeToAutoBlock) {
    ensureAutoBlock();
    autoBlock->name = FAKE_RETURN;
  }
  if (autoBlock) {
    autoBlock->finalize(result);
  }
  if (!currFunction) {
    makeFunction();
    body = allocator.alloc<Nop>();
  }
  if (currFunction->result != result) throw ParseException("bad func declaration", s.line, s.col);
  currFunction->body = body;
  currFunction->type = type;
  if (wasm.getFunctionOrNull(currFunction->name)) throw ParseException("duplicate function", s.line, s.col);
  wasm.addFunction(currFunction.release());
  currLocalTypes.clear();
  nameMapper.clear();
}

Type SExpressionWasmBuilder::stringToType(const char* str, bool allowError, bool prefix) {
  if (str[0] == 'i') {
    if (str[1] == '3' && str[2] == '2' && (prefix || str[3] == 0)) return i32;
    if (str[1] == '6' && str[2] == '4' && (prefix || str[3] == 0)) return i64;
  }
  if (str[0] == 'f') {
    if (str[1] == '3' && str[2] == '2' && (prefix || str[3] == 0)) return f32;
    if (str[1] == '6' && str[2] == '4' && (prefix || str[3] == 0)) return f64;
  }
  if (allowError) return none;
  throw ParseException("invalid wasm type");
}

Expression* SExpressionWasmBuilder::parseExpression(Element& s) {
  Expression* result = makeExpression(s);
  if (s.loc) {
    IString file = s.loc->filename;
    auto& debugInfoFileNames = wasm.debugInfoFileNames;
    auto iter = debugInfoFileIndices.find(file);
    if (iter == debugInfoFileIndices.end()) {
      Index index = debugInfoFileNames.size();
      debugInfoFileNames.push_back(file.c_str());
      debugInfoFileIndices[file] = index;
    }
    uint32_t fileIndex = debugInfoFileIndices[file];
    currFunction->debugLocations[result] = {fileIndex, s.loc->line, s.loc->column};
  }
  return result;
}

Expression* SExpressionWasmBuilder::makeExpression(Element& s) {
  IString id = s[0]->str();
  const char *str = id.str;
  const char *dot = strchr(str, '.');
  if (dot) {
    // type.operation (e.g. i32.add)
    Type type = stringToType(str, false, true);
    // Local copy to index into op without bounds checking.
    enum { maxNameSize = 15 };
    char op[maxNameSize + 1] = {'\0'};
    strncpy(op, dot + 1, maxNameSize);
#define BINARY_INT_OR_FLOAT(op) (type == i32 ? BinaryOp::op##Int32 : (type == i64 ? BinaryOp::op##Int64 : (type == f32 ? BinaryOp::op##Float32 : BinaryOp::op##Float64)))
#define BINARY_INT(op) (type == i32 ? BinaryOp::op##Int32 : BinaryOp::op##Int64)
#define BINARY_FLOAT(op) (type == f32 ? BinaryOp::op##Float32 : BinaryOp::op##Float64)
    switch (op[0]) {
      case 'a': {
        if (op[1] == 'b') return makeUnary(s, type == f32 ? UnaryOp::AbsFloat32 : UnaryOp::AbsFloat64, type);
        if (op[1] == 'd') return makeBinary(s, BINARY_INT_OR_FLOAT(Add), type);
        if (op[1] == 'n') return makeBinary(s, BINARY_INT(And), type);
        if (op[1] == 't' && !strncmp(op, "atomic.", strlen("atomic."))) {
          if (op[7] == 'l') return makeLoad(s, type, /*isAtomic=*/true);
          if (op[7] == 's') return makeStore(s, type, /*isAtomic=*/true);
          if (op[7] == 'r') return makeAtomicRMWOrCmpxchg(s, type);
        }
        abort_on(op);
      }
      case 'c': {
        if (op[1] == 'e') return makeUnary(s, type == f32 ? UnaryOp::CeilFloat32 : UnaryOp::CeilFloat64, type);
        if (op[1] == 'l') return makeUnary(s, type == i32 ? UnaryOp::ClzInt32 : UnaryOp::ClzInt64, type);
        if (op[1] == 'o') {
          if (op[2] == 'p') return makeBinary(s, BINARY_FLOAT(CopySign), type);
          if (op[2] == 'n') {
            if (op[3] == 'v') {
              if (op[8] == 's') return makeUnary(s, op[11] == '3' ? (type == f32 ? UnaryOp::ConvertSInt32ToFloat32 : UnaryOp::ConvertSInt32ToFloat64) : (type == f32 ? UnaryOp::ConvertSInt64ToFloat32 : UnaryOp::ConvertSInt64ToFloat64), type);
              if (op[8] == 'u') return makeUnary(s, op[11] == '3' ? (type == f32 ? UnaryOp::ConvertUInt32ToFloat32 : UnaryOp::ConvertUInt32ToFloat64) : (type == f32 ? UnaryOp::ConvertUInt64ToFloat32 : UnaryOp::ConvertUInt64ToFloat64), type);
            }
            if (op[3] == 's') return makeConst(s, type);
          }
        }
        if (op[1] == 't') return makeUnary(s, type == i32 ? UnaryOp::CtzInt32 : UnaryOp::CtzInt64, type);
        abort_on(op);
      }
      case 'd': {
        if (op[1] == 'i') {
          if (op[3] == '_') return makeBinary(s, op[4] == 'u' ? BINARY_INT(DivU) : BINARY_INT(DivS), type);
          if (op[3] == 0) return makeBinary(s, BINARY_FLOAT(Div), type);
        }
        if (op[1] == 'e') return makeUnary(s, UnaryOp::DemoteFloat64, type);
        abort_on(op);
      }
      case 'e': {
        if (op[1] == 'q') {
          if (op[2] == 0) return makeBinary(s, BINARY_INT_OR_FLOAT(Eq), type);
          if (op[2] == 'z') return makeUnary(s, type == i32 ? UnaryOp::EqZInt32 : UnaryOp::EqZInt64, type);
        }
        if (op[1] == 'x') {
          if (op[6] == '8') return makeUnary(s, type == i32 ? UnaryOp::ExtendS8Int32 : UnaryOp::ExtendS8Int64, type);
          if (op[6] == '1') return makeUnary(s, type == i32 ? UnaryOp::ExtendS16Int32 : UnaryOp::ExtendS16Int64, type);
          if (op[6] == '3') return makeUnary(s, UnaryOp::ExtendS32Int64, type);
          return makeUnary(s, op[7] == 'u' ? UnaryOp::ExtendUInt32 : UnaryOp::ExtendSInt32, type);
        }
        abort_on(op);
      }
      case 'f': {
        if (op[1] == 'l') return makeUnary(s, type == f32 ? UnaryOp::FloorFloat32 : UnaryOp::FloorFloat64, type);
        abort_on(op);
      }
      case 'g': {
        if (op[1] == 't') {
          if (op[2] == '_') return makeBinary(s, op[3] == 'u' ? BINARY_INT(GtU) : BINARY_INT(GtS), type);
          if (op[2] == 0) return makeBinary(s, BINARY_FLOAT(Gt), type);
        }
        if (op[1] == 'e') {
          if (op[2] == '_') return makeBinary(s, op[3] == 'u' ? BINARY_INT(GeU) : BINARY_INT(GeS), type);
          if (op[2] == 0) return makeBinary(s, BINARY_FLOAT(Ge), type);
        }
        abort_on(op);
      }
      case 'l': {
        if (op[1] == 't') {
          if (op[2] == '_') return makeBinary(s, op[3] == 'u' ? BINARY_INT(LtU) : BINARY_INT(LtS), type);
          if (op[2] == 0) return makeBinary(s, BINARY_FLOAT(Lt), type);
        }
        if (op[1] == 'e') {
          if (op[2] == '_') return makeBinary(s, op[3] == 'u' ? BINARY_INT(LeU) : BINARY_INT(LeS), type);
          if (op[2] == 0) return makeBinary(s, BINARY_FLOAT(Le), type);
        }
        if (op[1] == 'o') return makeLoad(s, type, /*isAtomic=*/false);
        abort_on(op);
      }
      case 'm': {
        if (op[1] == 'i') return makeBinary(s, BINARY_FLOAT(Min), type);
        if (op[1] == 'a') return makeBinary(s, BINARY_FLOAT(Max), type);
        if (op[1] == 'u') return makeBinary(s, BINARY_INT_OR_FLOAT(Mul), type);
        abort_on(op);
      }
      case 'n': {
        if (op[1] == 'e') {
          if (op[2] == 0) return makeBinary(s, BINARY_INT_OR_FLOAT(Ne), type);
          if (op[2] == 'a') return makeUnary(s, type == f32 ? UnaryOp::NearestFloat32 : UnaryOp::NearestFloat64, type);
          if (op[2] == 'g') return makeUnary(s, type == f32 ? UnaryOp::NegFloat32 : UnaryOp::NegFloat64, type);
        }
        abort_on(op);
      }
      case 'o': {
        if (op[1] == 'r') return makeBinary(s, BINARY_INT(Or), type);
        abort_on(op);
      }
      case 'p': {
        if (op[1] == 'r') return makeUnary(s,  UnaryOp::PromoteFloat32, type);
        if (op[1] == 'o') return makeUnary(s, type == i32 ? UnaryOp::PopcntInt32 : UnaryOp::PopcntInt64, type);
        abort_on(op);
      }
      case 'r': {
        if (op[1] == 'e') {
          if (op[2] == 'm') return makeBinary(s, op[4] == 'u' ? BINARY_INT(RemU) : BINARY_INT(RemS), type);
          if (op[2] == 'i') return makeUnary(s, isFloatType(type) ? (type == f32 ? UnaryOp::ReinterpretInt32 : UnaryOp::ReinterpretInt64) : (type == i32 ? UnaryOp::ReinterpretFloat32 : UnaryOp::ReinterpretFloat64), type);
        }
        if (op[1] == 'o' && op[2] == 't') {
          return makeBinary(s, op[3] == 'l' ? BINARY_INT(RotL) : BINARY_INT(RotR), type);
        }
        abort_on(op);
      }
      case 's': {
        if (op[1] == 'h') {
          if (op[2] == 'l') return makeBinary(s, BINARY_INT(Shl), type);
          return makeBinary(s, op[4] == 'u' ? BINARY_INT(ShrU) : BINARY_INT(ShrS), type);
        }
        if (op[1] == 'u') return makeBinary(s, BINARY_INT_OR_FLOAT(Sub), type);
        if (op[1] == 'q') return makeUnary(s, type == f32 ? UnaryOp::SqrtFloat32 : UnaryOp::SqrtFloat64, type);
        if (op[1] == 't') return makeStore(s, type, /*isAtomic=*/false);
        abort_on(op);
      }
      case 't': {
        if (op[1] == 'r') {
          if (op[6] == 's') return makeUnary(s, op[9] == '3' ? (type == i32 ? UnaryOp::TruncSFloat32ToInt32 : UnaryOp::TruncSFloat32ToInt64) : (type == i32 ? UnaryOp::TruncSFloat64ToInt32 : UnaryOp::TruncSFloat64ToInt64), type);
          if (op[6] == 'u') return makeUnary(s, op[9] == '3' ? (type == i32 ? UnaryOp::TruncUFloat32ToInt32 : UnaryOp::TruncUFloat32ToInt64) : (type == i32 ? UnaryOp::TruncUFloat64ToInt32 : UnaryOp::TruncUFloat64ToInt64), type);
          if (op[2] == 'u') return makeUnary(s, type == f32 ? UnaryOp::TruncFloat32 : UnaryOp::TruncFloat64, type);
        }
        abort_on(op);
      }
      case 'w': {
        if (!strncmp(op, "wait", strlen("wait"))) return makeAtomicWait(s, type);
        if (op[1] == 'r') return makeUnary(s,  UnaryOp::WrapInt64, type);
        abort_on(op);
      }
      case 'x': {
        if (op[1] == 'o') return makeBinary(s, BINARY_INT(Xor), type);
        abort_on(op);
      }
      default: abort_on(op);
    }
  } else {
    // other expression
    switch (str[0]) {
      case 'b': {
        if (str[1] == 'l') return makeBlock(s);
        if (str[1] == 'r') {
          if (str[2] == '_' && str[3] == 't') return makeBreakTable(s);
          return makeBreak(s);
        }
        abort_on(str);
      }
      case 'c': {
        if (str[1] == 'a') {
          if (id == CALL) return makeCall(s);
          if (id == CALL_IMPORT) return makeCallImport(s);
          if (id == CALL_INDIRECT) return makeCallIndirect(s);
        } else if (str[1] == 'u') return makeHost(s, HostOp::CurrentMemory);
        abort_on(str);
      }
      case 'd': {
        if (str[1] == 'r') return makeDrop(s);
        abort_on(str);
      }
      case 'e': {
        if (str[1] == 'l') return makeThenOrElse(s);
        abort_on(str);
      }
      case 'g': {
        if (str[1] == 'e') {
          if (str[4] == 'l') return makeGetLocal(s);
          if (str[4] == 'g') return makeGetGlobal(s);
        }
        if (str[1] == 'r') return makeHost(s, HostOp::GrowMemory);
        abort_on(str);
      }
      case 'h': {
        if (str[1] == 'a') return makeHost(s, HostOp::HasFeature);
        abort_on(str);
      }
      case 'i': {
        if (str[1] == 'f') return makeIf(s);
        abort_on(str);
      }
      case 'l': {
        if (str[1] == 'o') return makeLoop(s);
        abort_on(str);
      }
      case 'n': {
        if (str[1] == 'o') return allocator.alloc<Nop>();
        abort_on(str);
      }
      case 'p': {
        if (str[1] == 'a') return makeHost(s, HostOp::PageSize);
        abort_on(str);
      }
      case 's': {
        if (str[1] == 'e' && str[2] == 't') {
          if (str[4] == 'l') return makeSetLocal(s);
          if (str[4] == 'g') return makeSetGlobal(s);
        }
        if (str[1] == 'e' && str[2] == 'l') return makeSelect(s);
        abort_on(str);
      }
      case 'r': {
        if (str[1] == 'e') return makeReturn(s);
        abort_on(str);
      }
      case 't': {
        if (str[1] == 'h') return makeThenOrElse(s);
        if (str[1] == 'e' && str[2] == 'e') return makeTeeLocal(s);
        abort_on(str);
      }
      case 'u': {
        if (str[1] == 'n') return allocator.alloc<Unreachable>();
        abort_on(str);
      }
      case 'w': {
        if (!strncmp(str, "wake", strlen("wake"))) return makeAtomicWake(s);
        abort_on(str);
      }
      default: abort_on(str);
    }
  }
  abort_on("unrecognized input string for parsing");
}

Expression* SExpressionWasmBuilder::makeBinary(Element& s, BinaryOp op, Type type) {
  auto ret = allocator.alloc<Binary>();
  ret->op = op;
  ret->left = parseExpression(s[1]);
  ret->right = parseExpression(s[2]);
  ret->finalize();
  return ret;
}


Expression* SExpressionWasmBuilder::makeUnary(Element& s, UnaryOp op, Type type) {
  auto ret = allocator.alloc<Unary>();
  ret->op = op;
  ret->value = parseExpression(s[1]);
  ret->finalize();
  // type is the reported type, e.g. i64.ctz reports i64 (but has a return type of i32, in this case)
  // verify the reported type is correct
  switch (op) {
    case EqZInt32:
    case NegFloat32:
    case AbsFloat32:
    case CeilFloat32:
    case FloorFloat32:
    case TruncFloat32:
    case NearestFloat32:
    case SqrtFloat32:
    case ClzInt32:
    case CtzInt32:
    case PopcntInt32:
    case EqZInt64:
    case NegFloat64:
    case AbsFloat64:
    case CeilFloat64:
    case FloorFloat64:
    case TruncFloat64:
    case NearestFloat64:
    case SqrtFloat64:
    case ClzInt64:
    case CtzInt64:
    case PopcntInt64: {
      if (ret->value->type != unreachable && type != ret->value->type) throw ParseException(std::string("bad type for ") + getExpressionName(ret) + ": " + printType(type) + " vs value type " + printType(ret->value->type), s.line, s.col);
      break;
    }
    case ExtendSInt32: case ExtendUInt32:
    case ExtendS8Int32: case ExtendS16Int32:
    case ExtendS8Int64: case ExtendS16Int64: case ExtendS32Int64:
    case WrapInt64:
    case PromoteFloat32:
    case DemoteFloat64:
    case TruncSFloat32ToInt32:
    case TruncUFloat32ToInt32:
    case TruncSFloat64ToInt32:
    case TruncUFloat64ToInt32:
    case ReinterpretFloat32:
    case TruncSFloat32ToInt64:
    case TruncUFloat32ToInt64:
    case TruncSFloat64ToInt64:
    case TruncUFloat64ToInt64:
    case ReinterpretFloat64:
    case ReinterpretInt32:
    case ConvertSInt32ToFloat32:
    case ConvertUInt32ToFloat32:
    case ConvertSInt64ToFloat32:
    case ConvertUInt64ToFloat32:
    case ReinterpretInt64:
    case ConvertSInt32ToFloat64:
    case ConvertUInt32ToFloat64:
    case ConvertSInt64ToFloat64:
    case ConvertUInt64ToFloat64: break;
    default: WASM_UNREACHABLE();
  }
  return ret;
}

Expression* SExpressionWasmBuilder::makeSelect(Element& s) {
  auto ret = allocator.alloc<Select>();
  ret->ifTrue = parseExpression(s[1]);
  ret->ifFalse = parseExpression(s[2]);
  ret->condition = parseExpression(s[3]);
  ret->finalize();
  return ret;
}

Expression* SExpressionWasmBuilder::makeDrop(Element& s) {
  auto ret = allocator.alloc<Drop>();
  ret->value = parseExpression(s[1]);
  ret->finalize();
  return ret;
}

Expression* SExpressionWasmBuilder::makeHost(Element& s, HostOp op) {
  auto ret = allocator.alloc<Host>();
  ret->op = op;
  if (op == HostOp::HasFeature) {
    ret->nameOperand = s[1]->str();
  } else {
    parseCallOperands(s, 1, s.size(), ret);
  }
  if (ret->op == HostOp::GrowMemory) {
    if (ret->operands.size() != 1) {
      throw ParseException("grow_memory needs one operand");
    }
  } else {
    if (ret->operands.size() != 0) {
      throw ParseException("host needs zero operands");
    }
  }
  ret->finalize();
  return ret;
}

Index SExpressionWasmBuilder::getLocalIndex(Element& s) {
  if (!currFunction) throw ParseException("local access in non-function scope", s.line, s.col);
  if (s.dollared()) {
    auto ret = s.str();
    if (currFunction->localIndices.count(ret) == 0) throw ParseException("bad local name", s.line, s.col);
    return currFunction->getLocalIndex(ret);
  }
  // this is a numeric index
  Index ret = atoi(s.c_str());
  if (ret >= currFunction->getNumLocals()) throw ParseException("bad local index", s.line, s.col);
  return ret;
}

Expression* SExpressionWasmBuilder::makeGetLocal(Element& s) {
  auto ret = allocator.alloc<GetLocal>();
  ret->index = getLocalIndex(*s[1]);
  ret->type = currFunction->getLocalType(ret->index);
  return ret;
}

Expression* SExpressionWasmBuilder::makeTeeLocal(Element& s) {
  auto ret = allocator.alloc<SetLocal>();
  ret->index = getLocalIndex(*s[1]);
  ret->value = parseExpression(s[2]);
  ret->setTee(true);
  ret->finalize();
  return ret;
}

Expression* SExpressionWasmBuilder::makeSetLocal(Element& s) {
  auto ret = allocator.alloc<SetLocal>();
  ret->index = getLocalIndex(*s[1]);
  ret->value = parseExpression(s[2]);
  ret->setTee(false);
  ret->finalize();
  return ret;
}

Expression* SExpressionWasmBuilder::makeGetGlobal(Element& s) {
  auto ret = allocator.alloc<GetGlobal>();
  ret->name = getGlobalName(*s[1]);
  auto* global = wasm.getGlobalOrNull(ret->name);
  if (global) {
    ret->type = global->type;
    return ret;
  }
  auto* import = wasm.getImportOrNull(ret->name);
  if (import && import->kind == ExternalKind::Global) {
    ret->type = import->globalType;
    return ret;
  }
  throw ParseException("bad get_global name", s.line, s.col);
}

Expression* SExpressionWasmBuilder::makeSetGlobal(Element& s) {
  auto ret = allocator.alloc<SetGlobal>();
  ret->name = getGlobalName(*s[1]);
  if (wasm.getGlobalOrNull(ret->name) && !wasm.getGlobalOrNull(ret->name)->mutable_) throw ParseException("set_global of immutable", s.line, s.col);
  ret->value = parseExpression(s[2]);
  ret->finalize();
  return ret;
}


Expression* SExpressionWasmBuilder::makeBlock(Element& s) {
  // special-case Block, because Block nesting (in their first element) can be incredibly deep
  auto curr = allocator.alloc<Block>();
  auto* sp = &s;
  std::vector<std::pair<Element*, Block*>> stack;
  while (1) {
    stack.emplace_back(sp, curr);
    auto& s = *sp;
    Index i = 1;
    Name sName;
    if (i < s.size() && s[i]->isStr()) {
      // could be a name or a type
      if (s[i]->dollared() || stringToType(s[i]->str(), true /* allowError */) == none) {
        sName = s[i++]->str();
      } else {
        sName = "block";
      }
    } else {
      sName = "block";
    }
    curr->name = nameMapper.pushLabelName(sName);
    // block signature
    curr->type = parseOptionalResultType(s, i);
    if (i >= s.size()) break; // empty block
    auto& first = *s[i];
    if (first[0]->str() == BLOCK) {
      // recurse
      curr = allocator.alloc<Block>();
      sp = &first;
      continue;
    }
    break;
  }
  // we now have a stack of Blocks, with their labels, but no contents yet
  for (int t = int(stack.size()) - 1; t >= 0; t--) {
    auto* sp = stack[t].first;
    auto* curr = stack[t].second;
    auto& s = *sp;
    size_t i = 1;
    if (i < s.size()) {
      while (i < s.size() && s[i]->isStr()) {
        i++;
      }
      if (i < s.size() && (*s[i])[0]->str() == RESULT) {
        i++;
      }
      if (t < int(stack.size()) - 1) {
        // first child is one of our recursions
        curr->list.push_back(stack[t + 1].second);
        i++;
      }
      for (; i < s.size(); i++) {
        curr->list.push_back(parseExpression(s[i]));
      }
    }
    nameMapper.popLabelName(curr->name);
    curr->finalize(curr->type);
  }
  return stack[0].second;
}

// Similar to block, but the label is handled by the enclosing if (since there might not be a then or else, ick)
Expression* SExpressionWasmBuilder::makeThenOrElse(Element& s) {
  auto ret = allocator.alloc<Block>();
  size_t i = 1;
  if (s.size() > 1){
    if (s[1]->isStr()) {
      i++;
    }
    for (; i < s.size(); i++) {
      ret->list.push_back(parseExpression(s[i]));
    }
    ret->finalize();
  }
  return ret;
}

Expression* SExpressionWasmBuilder::makeConst(Element& s, Type type) {
  auto ret = parseConst(s[1]->str(), type, allocator);
  if (!ret) throw ParseException("bad const");
  return ret;
}

static uint8_t parseMemBytes(const char** in, uint8_t fallback) {
  uint8_t ret;
  const char* s = *in;
  if (s[0] == '8') {
    ret = 1;
    (*in)++;
  } else if (s[0] == '1') {
    if (s[1] != '6') throw ParseException("expected 16 for memop size");
    ret = 2;
    *in += 2;
  } else if (s[0] == '3') {
    if (s[1] != '2') throw ParseException("expected 32 for memop size");;
    ret = 4;
    *in += 2;
  } else {
    ret = fallback;
  }
  return ret;
}

static size_t parseMemAttributes(Element& s, Address* offset, Address* align, Address fallback) {
  size_t i = 1;
  *offset = 0;
  *align = fallback;
  while (!s[i]->isList()) {
    const char *str = s[i]->c_str();
    const char *eq = strchr(str, '=');
    if (!eq) throw ParseException("missing = in memory attribute");
    eq++;
    if (*eq == 0) throw ParseException("missing value in memory attribute", s.line, s.col);
    char* endptr;
    uint64_t value = strtoll(eq, &endptr, 10);
    if (*endptr != 0) {
      throw ParseException("bad memory attribute immediate", s.line, s.col);
    }
    if (str[0] == 'a') {
      if (value > std::numeric_limits<uint32_t>::max()) throw ParseException("bad align");
      *align = value;
    } else if (str[0] == 'o') {
      if (value > std::numeric_limits<uint32_t>::max()) throw ParseException("bad offset");
      *offset = value;
    } else throw ParseException("bad memory attribute");
    i++;
  }
  return i;
}

Expression* SExpressionWasmBuilder::makeLoad(Element& s, Type type, bool isAtomic) {
  const char *extra = strchr(s[0]->c_str(), '.') + 5; // after "type.load"
  if (isAtomic) extra += 7; // after "type.atomic.load"
  auto* ret = allocator.alloc<Load>();
  ret->isAtomic = isAtomic;
  ret->type = type;
  ret->bytes = parseMemBytes(&extra, getTypeSize(type));
  ret->signed_ = extra[0] && extra[1] == 's';
  size_t i = parseMemAttributes(s, &ret->offset, &ret->align, ret->bytes);
  ret->ptr = parseExpression(s[i]);
  ret->finalize();
  return ret;
}

Expression* SExpressionWasmBuilder::makeStore(Element& s, Type type, bool isAtomic) {
  const char *extra = strchr(s[0]->c_str(), '.') + 6; // after "type.store"
  if (isAtomic) extra += 7; // after "type.atomic.store"
  auto ret = allocator.alloc<Store>();
  ret->isAtomic = isAtomic;
  ret->valueType = type;
  ret->bytes = parseMemBytes(&extra, getTypeSize(type));
  size_t i = parseMemAttributes(s, &ret->offset, &ret->align, ret->bytes);
  ret->ptr = parseExpression(s[i]);
  ret->value = parseExpression(s[i+1]);
  ret->finalize();
  return ret;
}

Expression* SExpressionWasmBuilder::makeAtomicRMWOrCmpxchg(Element& s, Type type) {
  const char* extra = strchr(s[0]->c_str(), '.') + 11; // afer "type.atomic.rmw"
  auto bytes = parseMemBytes(&extra, getTypeSize(type));
  extra = strchr(extra, '.'); // after the optional '_u' and before the opcode
  if (!extra) throw ParseException("malformed atomic rmw instruction");
  extra++; // after the '.'
  if (!strncmp(extra, "cmpxchg", 7)) return makeAtomicCmpxchg(s, type, bytes, extra);
  return makeAtomicRMW(s, type, bytes, extra);
}

Expression* SExpressionWasmBuilder::makeAtomicRMW(Element& s, Type type, uint8_t bytes, const char* extra) {
  auto ret = allocator.alloc<AtomicRMW>();
  ret->type = type;
  ret->bytes = bytes;
  if (!strncmp(extra, "add", 3)) ret->op = Add;
  else if (!strncmp(extra, "and", 3)) ret->op = And;
  else if (!strncmp(extra, "or", 2)) ret->op = Or;
  else if (!strncmp(extra, "sub", 3)) ret->op = Sub;
  else if (!strncmp(extra, "xor", 3)) ret->op = Xor;
  else if (!strncmp(extra, "xchg", 4)) ret->op = Xchg;
  else throw ParseException("bad atomic rmw operator");
  Address align;
  size_t i = parseMemAttributes(s, &ret->offset, &align, ret->bytes);
  if (align != ret->bytes) throw ParseException("Align of Atomic RMW must match size");
  ret->ptr = parseExpression(s[i]);
  ret->value = parseExpression(s[i+1]);
  ret->finalize();
  return ret;
}

Expression* SExpressionWasmBuilder::makeAtomicCmpxchg(Element& s, Type type, uint8_t bytes, const char* extra) {
  auto ret = allocator.alloc<AtomicCmpxchg>();
  ret->type = type;
  ret->bytes = bytes;
  Address align;
  size_t i = parseMemAttributes(s, &ret->offset, &align, ret->bytes);
  if (align != ret->bytes) throw ParseException("Align of Atomic Cmpxchg must match size");
  ret->ptr = parseExpression(s[i]);
  ret->expected = parseExpression(s[i+1]);
  ret->replacement = parseExpression(s[i+2]);
  ret->finalize();
  return ret;
}

Expression* SExpressionWasmBuilder::makeAtomicWait(Element& s, Type type) {
  auto ret = allocator.alloc<AtomicWait>();
  ret->type = i32;
  ret->expectedType = type;
  ret->ptr = parseExpression(s[1]);
  ret->expected = parseExpression(s[2]);
  ret->timeout = parseExpression(s[3]);
  ret->finalize();
  return ret;
}

Expression* SExpressionWasmBuilder::makeAtomicWake(Element& s) {
  auto ret = allocator.alloc<AtomicWake>();
  ret->type = i32;
  ret->ptr = parseExpression(s[1]);
  ret->wakeCount = parseExpression(s[2]);
  ret->finalize();
  return ret;
}

Expression* SExpressionWasmBuilder::makeIf(Element& s) {
  auto ret = allocator.alloc<If>();
  Index i = 1;
  Name sName;
  if (s[i]->dollared()) {
    // the if is labeled
    sName = s[i++]->str();
  } else {
    sName = "if";
  }
  auto label = nameMapper.pushLabelName(sName);
  // if signature
  Type type = parseOptionalResultType(s, i);
  ret->condition = parseExpression(s[i++]);
  ret->ifTrue = parseExpression(*s[i++]);
  if (i < s.size()) {
    ret->ifFalse = parseExpression(*s[i++]);
  }
  ret->finalize(type);
  nameMapper.popLabelName(label);
  // create a break target if we must
  if (BranchUtils::BranchSeeker::hasNamed(ret, label)) {
    auto* block = allocator.alloc<Block>();
    block->name = label;
    block->list.push_back(ret);
    block->finalize(ret->type);
    return block;
  }
  return ret;
}


Expression* SExpressionWasmBuilder::makeMaybeBlock(Element& s, size_t i, Type type) {
  Index stopAt = -1;
  if (s.size() == i) return allocator.alloc<Nop>();
  if (s.size() == i+1) return parseExpression(s[i]);
  auto ret = allocator.alloc<Block>();
  for (; i < s.size() && i < stopAt; i++) {
    ret->list.push_back(parseExpression(s[i]));
  }
  ret->finalize(type);
  // Note that we do not name these implicit/synthetic blocks. They
  // are the effects of syntactic sugar, and nothing can branch to
  // them anyhow.
  return ret;
}

Type SExpressionWasmBuilder::parseOptionalResultType(Element& s, Index& i) {
  if (s.size() == i)
    return none;

  // TODO(sbc): Remove support for old result syntax (bare streing) once the
  // spec tests are updated.
  if (s[i]->isStr())
    return stringToType(s[i++]->str());

  Element& params = *s[i];
  IString id = params[0]->str();
  if (id != RESULT)
    return none;

  i++;
  return stringToType(params[1]->str());
}

Expression* SExpressionWasmBuilder::makeLoop(Element& s) {
  auto ret = allocator.alloc<Loop>();
  Index i = 1;
  Name sName;
  if (s.size() > i && s[i]->dollared()) {
    sName = s[i++]->str();
  } else {
    sName = "loop-in";
  }
  ret->name = nameMapper.pushLabelName(sName);
  ret->type = parseOptionalResultType(s, i);
  ret->body = makeMaybeBlock(s, i, ret->type);
  nameMapper.popLabelName(ret->name);
  ret->finalize(ret->type);
  return ret;
}

Expression* SExpressionWasmBuilder::makeCall(Element& s) {
  auto target = getFunctionName(*s[1]);
  auto* import = wasm.getImportOrNull(target);
  if (import && import->kind == ExternalKind::Function) {
    auto ret = allocator.alloc<CallImport>();
    ret->target = target;
    Import* import = wasm.getImport(ret->target);
    ret->type = wasm.getFunctionType(import->functionType)->result;
    parseCallOperands(s, 2, s.size(), ret);
    return ret;
  }
  auto ret = allocator.alloc<Call>();
  ret->target = target;
  ret->type = functionTypes[ret->target];
  parseCallOperands(s, 2, s.size(), ret);
  ret->finalize();
  return ret;
}

Expression* SExpressionWasmBuilder::makeCallImport(Element& s) {
  auto ret = allocator.alloc<CallImport>();
  ret->target = s[1]->str();
  Import* import = wasm.getImport(ret->target);
  ret->type = wasm.getFunctionType(import->functionType)->result;
  parseCallOperands(s, 2, s.size(), ret);
  ret->finalize();
  return ret;
}

Expression* SExpressionWasmBuilder::makeCallIndirect(Element& s) {
  if (!wasm.table.exists) throw ParseException("no table");
  auto ret = allocator.alloc<CallIndirect>();
  Element& typeElement = *s[1];
  if (typeElement[0]->str() != "type") throw ParseException("expected 'type' in call_indirect", s.line, s.col);
  IString type = typeElement[1]->str();
  auto* fullType = wasm.getFunctionTypeOrNull(type);
  if (!fullType) throw ParseException("invalid call_indirect type", s.line, s.col);
  ret->fullType = fullType->name;
  ret->type = fullType->result;
  parseCallOperands(s, 2, s.size() - 1, ret);
  ret->target = parseExpression(s[s.size() - 1]);
  ret->finalize();
  return ret;
}

Name SExpressionWasmBuilder::getLabel(Element& s) {
  if (s.dollared()) {
    return nameMapper.sourceToUnique(s.str());
  } else {
    // offset, break to nth outside label
    uint64_t offset;
    try {
      offset = std::stoll(s.c_str(), nullptr, 0);
    } catch (std::invalid_argument&) {
      throw ParseException("invalid break offset");
    } catch (std::out_of_range&) {
      throw ParseException("out of range break offset");
    }
    if (offset > nameMapper.labelStack.size()) throw ParseException("invalid label", s.line, s.col);
    if (offset == nameMapper.labelStack.size()) {
      // a break to the function's scope. this means we need an automatic block, with a name
      brokeToAutoBlock = true;
      return FAKE_RETURN;
    }
    return nameMapper.labelStack[nameMapper.labelStack.size() - 1 - offset];
  }
}

Expression* SExpressionWasmBuilder::makeBreak(Element& s) {
  auto ret = allocator.alloc<Break>();
  size_t i = 1;
  ret->name = getLabel(*s[i]);
  i++;
  if (i == s.size()) return ret;
  if (s[0]->str() == BR_IF) {
    if (i + 1 < s.size()) {
      ret->value = parseExpression(s[i]);
      i++;
    }
    ret->condition = parseExpression(s[i]);
  } else {
    ret->value = parseExpression(s[i]);
  }
  ret->finalize();
  return ret;
}

Expression* SExpressionWasmBuilder::makeBreakTable(Element& s) {
  auto ret = allocator.alloc<Switch>();
  size_t i = 1;
  while (!s[i]->isList()) {
    ret->targets.push_back(getLabel(*s[i++]));
  }
  if (ret->targets.size() == 0) throw ParseException("switch with no targets");
  ret->default_ = ret->targets.back();
  ret->targets.pop_back();
  ret->condition = parseExpression(s[i++]);
  if (i < s.size()) {
    ret->value = ret->condition;
    ret->condition = parseExpression(s[i++]);
  }
  return ret;
}

Expression* SExpressionWasmBuilder::makeReturn(Element& s) {
  auto ret = allocator.alloc<Return>();
  if (s.size() >= 2) {
    ret->value = parseExpression(s[1]);
  }
  return ret;
}

// converts an s-expression string representing binary data into an output sequence of raw bytes
// this appends to data, which may already contain content.
void SExpressionWasmBuilder::stringToBinary(const char* input, size_t size, std::vector<char>& data) {
  auto originalSize = data.size();
  data.resize(originalSize + size);
  char *write = data.data() + originalSize;
  while (1) {
    if (input[0] == 0) break;
    if (input[0] == '\\') {
      if (input[1] == '"') {
        *write++ = '"';
        input += 2;
        continue;
      } else if (input[1] == '\'') {
        *write++ = '\'';
        input += 2;
        continue;
      } else if (input[1] == '\\') {
        *write++ = '\\';
        input += 2;
        continue;
      } else if (input[1] == 'n') {
        *write++ = '\n';
        input += 2;
        continue;
      } else if (input[1] == 't') {
        *write++ = '\t';
        input += 2;
        continue;
      } else {
        *write++ = (char)(unhex(input[1])*16 + unhex(input[2]));
        input += 3;
        continue;
      }
    }
    *write++ = input[0];
    input++;
  }
  assert(write >= data.data());
  size_t actual = write - data.data();
  assert(actual <= data.size());
  data.resize(actual);
}

Index SExpressionWasmBuilder::parseMemoryLimits(Element& s, Index i) {
  wasm.memory.initial = getCheckedAddress(s[i++], "excessive memory init");
  if (i == s.size()) return i;
  uint64_t max = atoll(s[i++]->c_str());
  if (max > Memory::kMaxSize) throw ParseException("total memory must be <= 4GB");
  wasm.memory.max = max;
  return i;
}

void SExpressionWasmBuilder::parseMemory(Element& s, bool preParseImport) {
  if (wasm.memory.exists) throw ParseException("too many memories");
  wasm.memory.exists = true;
  wasm.memory.imported = preParseImport;
  wasm.memory.shared = false;
  Index i = 1;
  if (s[i]->dollared()) {
    wasm.memory.name = s[i++]->str();
  }
  Name importModule, importBase;
  if (s[i]->isList()) {
    auto& inner = *s[i];
    if (inner[0]->str() == EXPORT) {
      auto ex = make_unique<Export>();
      ex->name = inner[1]->str();
      ex->value = wasm.memory.name;
      ex->kind = ExternalKind::Memory;
      if (wasm.getExportOrNull(ex->name)) throw ParseException("duplicate export", s.line, s.col);
      wasm.addExport(ex.release());
      i++;
    } else if (inner[0]->str() == IMPORT) {
      importModule = inner[1]->str();
      importBase = inner[2]->str();
      auto im = make_unique<Import>();
      im->kind = ExternalKind::Memory;
      im->module = importModule;
      im->base = importBase;
      im->name = importModule;
      if (wasm.getImportOrNull(im->name)) throw ParseException("duplicate import", s.line, s.col);
      wasm.addImport(im.release());
      i++;
    } else if (inner[0]->str() == "shared") {
      wasm.memory.shared = true;
      parseMemoryLimits(inner, 1);
      i++;
    } else {
      if (!(inner.size() > 0 ? inner[0]->str() != IMPORT : true)) throw ParseException("bad import ending");
      // (memory (data ..)) format
      parseInnerData(*s[i]);
      wasm.memory.initial = wasm.memory.segments[0].data.size();
      return;
    }
  }
  if (!wasm.memory.shared) i = parseMemoryLimits(s, i);

  // Parse memory initializers.
  while (i < s.size()) {
    Element& curr = *s[i];
    size_t j = 1;
    Address offsetValue;
    if (curr[0]->str() == DATA) {
      offsetValue = 0;
    } else {
      offsetValue = getCheckedAddress(curr[j++], "excessive memory offset");
    }
    const char *input = curr[j]->c_str();
    auto* offset = allocator.alloc<Const>();
    offset->type = i32;
    offset->value = Literal(int32_t(offsetValue));
    if (auto size = strlen(input)) {
      std::vector<char> data;
      stringToBinary(input, size, data);
      wasm.memory.segments.emplace_back(offset, data.data(), data.size());
    } else {
      wasm.memory.segments.emplace_back(offset, "", 0);
    }
    i++;
  }
}

void SExpressionWasmBuilder::parseData(Element& s) {
  if (!wasm.memory.exists) throw ParseException("data but no memory");
  Index i = 1;
  if (!s[i]->isList()) {
    // the memory is named
    i++;
  }
  auto* offset = parseExpression(s[i++]);
  parseInnerData(s, i, offset);
}

void SExpressionWasmBuilder::parseInnerData(Element& s, Index i, Expression* offset) {
  std::vector<char> data;
  while (i < s.size()) {
    const char *input = s[i++]->c_str();
    if (auto size = strlen(input)) {
      stringToBinary(input, size, data);
    }
  }
  if (!offset) {
    offset = allocator.alloc<Const>()->set(Literal(int32_t(0)));
  }
  wasm.memory.segments.emplace_back(offset, data.data(), data.size());
}

void SExpressionWasmBuilder::parseExport(Element& s) {
  std::unique_ptr<Export> ex = make_unique<Export>();
  ex->name = s[1]->str();
  if (s[2]->isList()) {
    auto& inner = *s[2];
    ex->value = inner[1]->str();
    if (inner[0]->str() == FUNC) {
      ex->kind = ExternalKind::Function;
    } else if (inner[0]->str() == MEMORY) {
      ex->kind = ExternalKind::Memory;
    } else if (inner[0]->str() == TABLE) {
      ex->kind = ExternalKind::Table;
    } else if (inner[0]->str() == GLOBAL) {
      ex->kind = ExternalKind::Global;
      if (wasm.getGlobalOrNull(ex->value) && wasm.getGlobal(ex->value)->mutable_) throw ParseException("cannot export a mutable global", s.line, s.col);
    } else {
      throw ParseException("invalid export");
    }
  } else if (!s[2]->dollared() && !std::isdigit(s[2]->str()[0])) {
    ex->value = s[3]->str();
    if (s[2]->str() == MEMORY) {
      if (!wasm.memory.exists) throw ParseException("memory exported but no memory");
      ex->kind = ExternalKind::Memory;
    } else if (s[2]->str() == TABLE) {
      ex->kind = ExternalKind::Table;
    } else if (s[2]->str() == GLOBAL) {
      ex->kind = ExternalKind::Global;
    } else {
      throw ParseException("invalid ext export");
    }
  } else {
    // function
    ex->value = s[2]->str();
    ex->kind = ExternalKind::Function;
  }
  if (wasm.getExportOrNull(ex->name)) throw ParseException("duplicate export", s.line, s.col);
  wasm.addExport(ex.release());
}

void SExpressionWasmBuilder::parseImport(Element& s) {
  std::unique_ptr<Import> im = make_unique<Import>();
  size_t i = 1;
  bool newStyle = s.size() == 4 && s[3]->isList(); // (import "env" "STACKTOP" (global $stackTop i32))
  if (newStyle) {
    if ((*s[3])[0]->str() == FUNC) {
      im->kind = ExternalKind::Function;
    } else if ((*s[3])[0]->str() == MEMORY) {
      im->kind = ExternalKind::Memory;
      if (wasm.memory.exists) throw ParseException("more than one memory");
      wasm.memory.exists = true;
      wasm.memory.imported = true;
    } else if ((*s[3])[0]->str() == TABLE) {
      im->kind = ExternalKind::Table;
      if (wasm.table.exists) throw ParseException("more than one table");
      wasm.table.exists = true;
      wasm.table.imported = true;
    } else if ((*s[3])[0]->str() == GLOBAL) {
      im->kind = ExternalKind::Global;
    } else {
      newStyle = false; // either (param..) or (result..)
    }
  }
  Index newStyleInner = 1;
  if (s.size() > 3 && s[3]->isStr()) {
    im->name = s[i++]->str();
  } else if (newStyle && newStyleInner < s[3]->size() && (*s[3])[newStyleInner]->dollared()) {
    im->name = (*s[3])[newStyleInner++]->str();
  }
  if (!im->name.is()) {
    if (im->kind == ExternalKind::Function) {
      im->name = Name("import$function$" + std::to_string(functionCounter++));
      functionNames.push_back(im->name);
    } else if (im->kind == ExternalKind::Global) {
      im->name = Name("import$global" + std::to_string(globalCounter++));
      globalNames.push_back(im->name);
    } else if (im->kind == ExternalKind::Memory) {
      im->name = Name("import$memory$" + std::to_string(0));
    } else if (im->kind == ExternalKind::Table) {
      im->name = Name("import$table$" + std::to_string(0));
    } else {
      throw ParseException("invalid import");
    }
  }
  if (!s[i]->quoted()) {
    if (s[i]->str() == MEMORY) {
      im->kind = ExternalKind::Memory;
    } else if (s[i]->str() == TABLE) {
      im->kind = ExternalKind::Table;
    } else if (s[i]->str() == GLOBAL) {
      im->kind = ExternalKind::Global;
    } else {
      throw ParseException("invalid ext import");
    }
    i++;
  } else if (!newStyle) {
    im->kind = ExternalKind::Function;
  }
  im->module = s[i++]->str();
  if (!s[i]->isStr()) throw ParseException("no name for import");
  im->base = s[i++]->str();
  // parse internals
  Element& inner = newStyle ? *s[3] : s;
  Index j = newStyle ? newStyleInner : i;
  if (im->kind == ExternalKind::Function) {
    std::unique_ptr<FunctionType> type = make_unique<FunctionType>();
    if (inner.size() > j) {
      Element& params = *inner[j];
      IString id = params[0]->str();
      if (id == PARAM) {
        for (size_t k = 1; k < params.size(); k++) {
          type->params.push_back(stringToType(params[k]->str()));
        }
      } else if (id == RESULT) {
        type->result = stringToType(params[1]->str());
      } else if (id == TYPE) {
        IString name = params[1]->str();
        if (!wasm.getFunctionTypeOrNull(name)) throw ParseException("bad function type for import");
        *type = *wasm.getFunctionType(name);
      } else {
        throw ParseException("bad import element");
      }
      if (inner.size() > j+1) {
        Element& result = *inner[j+1];
        if (result[0]->str() != RESULT) throw ParseException("expected result");
        type->result = stringToType(result[1]->str());
      }
    }
    im->functionType = ensureFunctionType(getSig(type.get()), &wasm)->name;
  } else if (im->kind == ExternalKind::Global) {
    if (inner[j]->isStr()) {
      im->globalType = stringToType(inner[j]->str());
    } else {
      auto& inner2 = *inner[j];
      if (inner2[0]->str() != MUT) throw ParseException("expected mut");
      im->globalType = stringToType(inner2[1]->str());
      throw ParseException("cannot import a mutable global", s.line, s.col);
    }
  } else if (im->kind == ExternalKind::Table) {
    if (j < inner.size() - 1) {
      wasm.table.initial = getCheckedAddress(inner[j++], "excessive table init size");
    }
    if (j < inner.size() - 1) {
      wasm.table.max = getCheckedAddress(inner[j++], "excessive table max size");
    } else {
      wasm.table.max = Table::kMaxSize;
    }
    // ends with the table element type
  } else if (im->kind == ExternalKind::Memory) {
    if (inner[j]->isList()) {
      auto& limits = *inner[j];
      if (!(limits[0]->isStr() && limits[0]->str() == "shared")) throw ParseException("bad memory limit declaration");
      wasm.memory.shared = true;
      parseMemoryLimits(limits, 1);
    } else {
      parseMemoryLimits(inner, j);
    }
  }
  if (wasm.getImportOrNull(im->name)) throw ParseException("duplicate import", s.line, s.col);
  wasm.addImport(im.release());
}

void SExpressionWasmBuilder::parseGlobal(Element& s, bool preParseImport) {
  std::unique_ptr<Global> global = make_unique<Global>();
  size_t i = 1;
  if (s[i]->dollared() && !(s[i]->isStr() && isType(s[i]->str()))) {
    global->name = s[i++]->str();
  } else {
    global->name = Name::fromInt(globalCounter);
  }
  globalCounter++;
  globalNames.push_back(global->name);
  bool mutable_ = false;
  Type type = none;
  bool exported = false;
  Name importModule, importBase;
  while (i < s.size() && s[i]->isList()) {
    auto& inner = *s[i];
    if (inner[0]->str() == EXPORT) {
      auto ex = make_unique<Export>();
      ex->name = inner[1]->str();
      ex->value = global->name;
      ex->kind = ExternalKind::Global;
      if (wasm.getExportOrNull(ex->name)) throw ParseException("duplicate export", s.line, s.col);
      wasm.addExport(ex.release());
      exported = true;
      i++;
    } else if (inner[0]->str() == IMPORT) {
      importModule = inner[1]->str();
      importBase = inner[2]->str();
      i++;
    } else if (inner[0]->str() == MUT) {
      mutable_ = true;
      type = stringToType(inner[1]->str());
      i++;
    } else {
      break;
    }
  }
  if (exported && mutable_) throw ParseException("cannot export a mutable global", s.line, s.col);
  if (type == none) {
    type = stringToType(s[i++]->str());
  }
  if (importModule.is()) {
    // this is an import, actually
    if (!preParseImport) throw ParseException("!preParseImport in global");
    if (mutable_) throw ParseException("cannot import a mutable global", s.line, s.col);
    std::unique_ptr<Import> im = make_unique<Import>();
    im->name = global->name;
    im->module = importModule;
    im->base = importBase;
    im->kind = ExternalKind::Global;
    im->globalType = type;
    if (wasm.getImportOrNull(im->name)) throw ParseException("duplicate import", s.line, s.col);
    wasm.addImport(im.release());
    return;
  }
  if (preParseImport) throw ParseException("preParseImport in global");
  global->type = type;
  if (i < s.size()) {
    global->init = parseExpression(s[i++]);
  } else {
    throw ParseException("global without init", s.line, s.col);
  }
  global->mutable_ = mutable_;
  if (i != s.size()) throw ParseException("extra import elements");
  if (wasm.getGlobalOrNull(global->name)) throw ParseException("duplicate import", s.line, s.col);
  wasm.addGlobal(global.release());
}


void SExpressionWasmBuilder::parseTable(Element& s, bool preParseImport) {
  if (wasm.table.exists) throw ParseException("more than one table");
  wasm.table.exists = true;
  wasm.table.imported = preParseImport;
  Index i = 1;
  if (i == s.size()) return; // empty table in old notation
  if (s[i]->dollared()) {
    wasm.table.name = s[i++]->str();
  }
  if (i == s.size()) return;
  Name importModule, importBase;
  if (s[i]->isList()) {
    auto& inner = *s[i];
    if (inner[0]->str() == EXPORT) {
      auto ex = make_unique<Export>();
      ex->name = inner[1]->str();
      ex->value = wasm.table.name;
      ex->kind = ExternalKind::Table;
      if (wasm.getExportOrNull(ex->name)) throw ParseException("duplicate export", s.line, s.col);
      wasm.addExport(ex.release());
      i++;
    } else if (inner[0]->str() == IMPORT) {
      importModule = inner[1]->str();
      importBase = inner[2]->str();
      if (!preParseImport) throw ParseException("!preParseImport in table");
      auto im = make_unique<Import>();
      im->kind = ExternalKind::Table;
      im->module = importModule;
      im->base = importBase;
      im->name = importModule;
      if (wasm.getImportOrNull(im->name)) throw ParseException("duplicate import", s.line, s.col);
      wasm.addImport(im.release());
      i++;
    } else {
      throw ParseException("invalid table");
    }
  }
  if (i == s.size()) return;
  if (!s[i]->dollared()) {
    if (s[i]->str() == ANYFUNC) {
      // (table type (elem ..))
      parseInnerElem(*s[i + 1]);
      if (wasm.table.segments.size() > 0) {
        wasm.table.initial = wasm.table.max = wasm.table.segments[0].data.size();
      } else {
        wasm.table.initial = wasm.table.max = 0;
      }
      return;
    }
    // first element isn't dollared, and isn't anyfunc. this could be old syntax for (table 0 1) which means function 0 and 1, or it could be (table initial max? type), look for type
    if (s[s.size() - 1]->str() == ANYFUNC) {
      // (table initial max? type)
      if (i < s.size() - 1) {
        wasm.table.initial = atoi(s[i++]->c_str());
      }
      if (i < s.size() - 1) {
        wasm.table.max = atoi(s[i++]->c_str());
      }
      return;
    }
  }
  // old notation (table func1 func2 ..)
  parseInnerElem(s, i);
  if (wasm.table.segments.size() > 0) {
    wasm.table.initial = wasm.table.max = wasm.table.segments[0].data.size();
  } else {
    wasm.table.initial = wasm.table.max = 0;
  }
}

void SExpressionWasmBuilder::parseElem(Element& s) {
  Index i = 1;
  if (!s[i]->isList()) {
    // the table is named
    i++;
  }
  auto* offset = parseExpression(s[i++]);
  parseInnerElem(s, i, offset);
}

void SExpressionWasmBuilder::parseInnerElem(Element& s, Index i, Expression* offset) {
  if (!wasm.table.exists) throw ParseException("elem without table", s.line, s.col);
  if (!offset) {
    offset = allocator.alloc<Const>()->set(Literal(int32_t(0)));
  }
  Table::Segment segment(offset);
  for (; i < s.size(); i++) {
    segment.data.push_back(getFunctionName(*s[i]));
  }
  wasm.table.segments.push_back(segment);
}

void SExpressionWasmBuilder::parseType(Element& s) {
  std::unique_ptr<FunctionType> type = make_unique<FunctionType>();
  size_t i = 1;
  if (s[i]->isStr()) {
    type->name = s[i]->str();
    i++;
  }
  Element& func = *s[i];
  for (size_t k = 1; k < func.size(); k++) {
    Element& curr = *func[k];
    if (curr[0]->str() == PARAM) {
      for (size_t j = 1; j < curr.size(); j++) {
        type->params.push_back(stringToType(curr[j]->str()));
      }
    } else if (curr[0]->str() == RESULT) {
      if (curr.size() > 2) throw ParseException("invalid result arity", curr.line, curr.col);
      type->result = stringToType(curr[1]->str());
    }
  }
  if (!type->name.is()) {
    type->name = Name::fromInt(wasm.functionTypes.size());
  }
  functionTypeNames.push_back(type->name);
  if (wasm.getFunctionTypeOrNull(type->name)) throw ParseException("duplicate function type", s.line, s.col);
  wasm.addFunctionType(type.release());
}

} // namespace wasm
