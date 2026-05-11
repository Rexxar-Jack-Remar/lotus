#include "Concurrency/CUDA/PTXAnalyzer.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace concurrency::cuda::ptx {
namespace {

std::string trim(std::string value) {
  auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
  value.erase(value.begin(),
              std::find_if(value.begin(), value.end(),
                           [&](char c) { return !is_space(c); }));
  value.erase(std::find_if(value.rbegin(), value.rend(),
                           [&](char c) { return !is_space(c); })
                  .base(),
              value.end());
  return value;
}

bool startsWith(const std::string &value, const std::string &prefix) {
  return value.rfind(prefix, 0) == 0;
}

bool endsWith(const std::string &value, char suffix) {
  return !value.empty() && value.back() == suffix;
}

std::string trimRightChars(std::string value, const std::string &chars) {
  while (!value.empty() && chars.find(value.back()) != std::string::npos) {
    value.pop_back();
  }
  return value;
}

std::vector<std::string> splitWhitespace(const std::string &input) {
  std::vector<std::string> out;
  std::istringstream stream(input);
  std::string part;
  while (stream >> part) {
    out.push_back(part);
  }
  return out;
}

std::vector<std::string> splitOperands(const std::string &input) {
  std::vector<std::string> out;
  int depth = 0;
  size_t start = 0;
  for (size_t idx = 0; idx < input.size(); ++idx) {
    char ch = input[idx];
    if (ch == '[' || ch == '{' || ch == '(') {
      ++depth;
    } else if (ch == ']' || ch == '}' || ch == ')') {
      --depth;
    } else if (ch == ',' && depth == 0) {
      out.push_back(trim(input.substr(start, idx - start)));
      start = idx + 1;
    }
  }
  out.push_back(trim(input.substr(start)));
  out.erase(std::remove_if(out.begin(), out.end(),
                           [](const std::string &s) { return s.empty(); }),
            out.end());
  return out;
}

std::vector<std::pair<size_t, std::string>>
logicalLines(const std::string &source) {
  std::vector<std::pair<size_t, std::string>> lines;
  std::istringstream stream(source);
  std::string raw;
  size_t line_no = 1;
  while (std::getline(stream, raw)) {
    size_t comment = raw.find("//");
    std::string line = trim(raw.substr(0, comment));
    if (!line.empty()) {
      lines.emplace_back(line_no, line);
    }
    ++line_no;
  }
  return lines;
}

std::optional<int64_t> parseInt(const std::string &input) {
  std::string clean = trimRightChars(trim(input), ";");
  if (clean.empty()) {
    return std::nullopt;
  }
  try {
    size_t consumed = 0;
    int base = startsWith(clean, "0x") ? 16 : 10;
    int64_t value = std::stoll(clean, &consumed, base);
    if (consumed == clean.size()) {
      return value;
    }
  } catch (const std::exception &) {
  }
  return std::nullopt;
}

std::string stripOuter(std::string value, char left, char right) {
  value = trim(value);
  if (!value.empty() && value.front() == left) {
    value.erase(value.begin());
  }
  if (!value.empty() && value.back() == right) {
    value.pop_back();
  }
  return trim(value);
}

std::vector<std::string> parseParamNames(const std::string &line) {
  std::string cleaned = trimRightChars(trim(line), ",;");
  if (cleaned.find(".param") == std::string::npos) {
    return {};
  }
  std::vector<std::string> parts = splitWhitespace(cleaned);
  if (parts.empty()) {
    return {};
  }
  std::string name = parts.back();
  while (!name.empty() && (name.front() == ',' || name.front() == ';' ||
                           name.front() == ')' || name.front() == '(')) {
    name.erase(name.begin());
  }
  name = trimRightChars(name, ",;)(");
  if (name.empty() || name.front() == '.') {
    return {};
  }
  return {name};
}

std::vector<std::string> parseInlineParams(const std::string &line) {
  size_t start = line.find('(');
  size_t end = line.rfind(')');
  if (start == std::string::npos || end == std::string::npos || end <= start) {
    return {};
  }
  std::vector<std::string> params;
  for (const std::string &part :
       splitOperands(line.substr(start + 1, end - start - 1))) {
    std::vector<std::string> names = parseParamNames(part);
    params.insert(params.end(), names.begin(), names.end());
  }
  return params;
}

std::optional<DeclAst> parseDecl(const std::string &line, size_t line_no) {
  std::string compact = trimRightChars(trim(line), ";");
  if (!startsWith(compact, ".shared")) {
    return std::nullopt;
  }
  std::vector<std::string> parts = splitWhitespace(compact);
  if (parts.empty()) {
    return std::nullopt;
  }
  std::string name_part = parts.back();
  std::string name = name_part;
  uint64_t count = 1;
  size_t bracket = name_part.find('[');
  if (bracket != std::string::npos) {
    name = name_part.substr(0, bracket);
    std::string count_text = name_part.substr(bracket + 1);
    count_text = trimRightChars(count_text, "]");
    auto parsed = parseInt(count_text);
    if (!parsed || *parsed < 0) {
      return std::nullopt;
    }
    count = static_cast<uint64_t>(*parsed);
  }
  uint64_t elem_bytes = 1;
  for (const std::string &part : parts) {
    if (part.size() > 2 && part[0] == '.' && part[1] == 'b') {
      auto bits = parseInt(part.substr(2));
      if (bits && *bits > 0) {
        elem_bytes = static_cast<uint64_t>(*bits) / 8;
      }
    }
  }
  return DeclAst{".shared", name, elem_bytes * count, SourceSpan{line_no, 1}};
}

StmtAst parseStmt(const std::string &line, size_t line_no) {
  if (endsWith(line, ':')) {
    StmtAst stmt;
    stmt.kind = StmtKind::Label;
    stmt.name = trimRightChars(line, ":");
    stmt.span = {line_no, 1};
    return stmt;
  }
  if (startsWith(line, ".")) {
    StmtAst stmt;
    stmt.kind = StmtKind::Directive;
    stmt.text = line;
    stmt.span = {line_no, 1};
    return stmt;
  }

  std::string text = trimRightChars(trim(line), ";");
  std::string rest = text;
  std::optional<std::string> predicate;
  if (startsWith(rest, "@")) {
    size_t split = rest.find_first_of(" \t");
    predicate = rest.substr(1, split == std::string::npos ? std::string::npos
                                                          : split - 1);
    rest = split == std::string::npos ? "" : trim(rest.substr(split + 1));
  }
  size_t split = rest.find_first_of(" \t");
  std::string opcode =
      split == std::string::npos ? rest : rest.substr(0, split);
  std::vector<std::string> operands =
      split == std::string::npos ? std::vector<std::string>{}
                                 : splitOperands(rest.substr(split + 1));

  StmtAst stmt;
  stmt.kind = StmtKind::Instruction;
  stmt.instruction =
      InstructionAst{predicate, opcode, operands, text, SourceSpan{line_no, 1}};
  stmt.span = {line_no, 1};
  return stmt;
}

std::pair<EntryAst, size_t>
parseEntry(const std::vector<std::pair<size_t, std::string>> &lines,
           size_t start) {
  const auto &[line_no, first] = lines[start];
  size_t entry_pos = first.find(".entry");
  if (entry_pos == std::string::npos) {
    throw std::runtime_error("entry parse failure on line " +
                             std::to_string(line_no));
  }
  std::string after_entry = trim(first.substr(entry_pos + 6));
  size_t name_end = after_entry.find_first_of("({ \t");
  std::string name = after_entry.substr(0, name_end);
  if (name.empty()) {
    throw std::runtime_error("missing entry name on line " +
                             std::to_string(line_no));
  }

  EntryAst entry;
  entry.name = name;
  entry.params = parseInlineParams(after_entry);
  entry.span = {line_no, 1};

  int depth = static_cast<int>(std::count(first.begin(), first.end(), '{')) -
              static_cast<int>(std::count(first.begin(), first.end(), '}'));
  bool seen_body = first.find('{') != std::string::npos;
  size_t i = start + 1;
  for (; i < lines.size(); ++i) {
    const auto &[body_line_no, line] = lines[i];
    if (!seen_body) {
      std::vector<std::string> names = parseParamNames(line);
      entry.params.insert(entry.params.end(), names.begin(), names.end());
      if (line.find('{') != std::string::npos) {
        seen_body = true;
        depth += static_cast<int>(std::count(line.begin(), line.end(), '{'));
        depth -= static_cast<int>(std::count(line.begin(), line.end(), '}'));
      }
      continue;
    }

    depth += static_cast<int>(std::count(line.begin(), line.end(), '{'));
    depth -= static_cast<int>(std::count(line.begin(), line.end(), '}'));
    std::string body_line = trim(line);
    while (!body_line.empty() &&
           (body_line.front() == '{' || body_line.front() == '}')) {
      body_line.erase(body_line.begin());
      body_line = trim(body_line);
    }
    while (!body_line.empty() &&
           (body_line.back() == '{' || body_line.back() == '}')) {
      body_line.pop_back();
      body_line = trim(body_line);
    }
    if (!body_line.empty()) {
      if (startsWith(body_line, ".param")) {
        std::vector<std::string> names = parseParamNames(body_line);
        entry.params.insert(entry.params.end(), names.begin(), names.end());
      } else if (auto decl = parseDecl(body_line, body_line_no)) {
        StmtAst stmt;
        stmt.kind = StmtKind::Directive;
        stmt.text =
            decl->space + " " + decl->name + " " + std::to_string(decl->bytes);
        stmt.span = {body_line_no, 1};
        entry.body.push_back(std::move(stmt));
      } else {
        entry.body.push_back(parseStmt(body_line, body_line_no));
      }
    }
    if (seen_body && depth <= 0) {
      ++i;
      break;
    }
  }
  return {entry, i};
}

std::optional<std::pair<std::string, uint64_t>>
parseSharedDirective(const std::string &text) {
  std::vector<std::string> parts = splitWhitespace(text);
  if (parts.size() < 3 || parts[0] != ".shared") {
    return std::nullopt;
  }
  auto bytes = parseInt(parts[2]);
  if (!bytes || *bytes < 0) {
    return std::nullopt;
  }
  return std::make_pair(parts[1], static_cast<uint64_t>(*bytes));
}

const EntryAst &selectEntry(const ModuleAst &module,
                            const std::optional<std::string> &entry_name) {
  if (entry_name) {
    auto it = std::find_if(
        module.entries.begin(), module.entries.end(),
        [&](const EntryAst &entry) { return entry.name == *entry_name; });
    if (it == module.entries.end()) {
      throw std::runtime_error("entry not found: " + *entry_name);
    }
    return *it;
  }
  if (module.entries.empty()) {
    throw std::runtime_error("module contains no .entry kernel");
  }
  if (module.entries.size() != 1) {
    throw std::runtime_error(
        "module contains multiple entries; pass an entry name");
  }
  return module.entries.front();
}

struct Value {
  enum class Kind { Int, Bool, Ptr, Symbol, Unknown } kind = Kind::Unknown;
  int64_t int_value = 0;
  bool bool_value = false;
  std::string space;
  std::string base;
  int64_t offset = 0;
  std::string symbol;

  static Value intValue(int64_t value) {
    Value out;
    out.kind = Kind::Int;
    out.int_value = value;
    return out;
  }

  static Value boolValue(bool value) {
    Value out;
    out.kind = Kind::Bool;
    out.bool_value = value;
    return out;
  }

  static Value ptrValue(std::string space, std::string base, int64_t offset) {
    Value out;
    out.kind = Kind::Ptr;
    out.space = std::move(space);
    out.base = std::move(base);
    out.offset = offset;
    return out;
  }

  static Value symbolValue(std::string value) {
    Value out;
    out.kind = Kind::Symbol;
    out.symbol = std::move(value);
    return out;
  }
};

struct SyncSet {
  std::vector<uint32_t> threads;

  bool operator==(const SyncSet &other) const {
    return threads == other.threads;
  }
};

struct ThreadState {
  uint32_t tid = 0;
  size_t pc = 0;
  bool done = false;
  std::optional<SyncSet> blocked;
  std::unordered_map<std::string, Value> regs;
};

struct MemEvents {
  std::unordered_map<uint32_t, std::set<uint32_t>> readers;
  std::optional<std::pair<uint32_t, std::set<uint32_t>>> writer;
};

struct MemoryCell {
  std::optional<Value> value;
  MemEvents events;
};

Diagnostic errorDiagnostic(std::string code, std::string message,
                           std::optional<SourceSpan> span,
                           std::optional<uint32_t> thread = std::nullopt) {
  return Diagnostic{std::move(code), Severity::Error, std::move(message), span,
                    thread};
}

std::string opcodeClass(const std::string &opcode) {
  size_t dot = opcode.find('.');
  return dot == std::string::npos ? opcode : opcode.substr(0, dot);
}

std::string conditionKind(const std::string &opcode) {
  std::istringstream stream(opcode);
  std::string part;
  while (std::getline(stream, part, '.')) {
    if (part == "eq" || part == "ne" || part == "lt" || part == "le" ||
        part == "gt" || part == "ge") {
      return part;
    }
  }
  return "";
}

uint32_t accessWidth(const std::string &opcode) {
  std::istringstream stream(opcode);
  std::string part;
  while (std::getline(stream, part, '.')) {
    if (part.size() > 1 && (part[0] == 'b' || part[0] == 'u' ||
                            part[0] == 's' || part[0] == 'f')) {
      auto bits = parseInt(part.substr(1));
      if (bits && *bits > 0) {
        return static_cast<uint32_t>(*bits / 8);
      }
    }
  }
  return 4;
}

std::string memoryBase(const std::string &operand) {
  std::string inner = stripOuter(operand, '[', ']');
  size_t plus = inner.find('+');
  return trim(plus == std::string::npos ? inner : inner.substr(0, plus));
}

std::set<uint32_t> allThreads(uint32_t thread_count) {
  std::set<uint32_t> out;
  for (uint32_t tid = 0; tid < thread_count; ++tid) {
    out.insert(tid);
  }
  return out;
}

std::set<uint32_t> allPending(uint32_t tid, const KernelConfig &config) {
  std::set<uint32_t> out = allThreads(config.threadCount());
  out.insert(tid);
  return out;
}

Value addValues(const Value &lhs, const Value &rhs) {
  if (lhs.kind == Value::Kind::Int && rhs.kind == Value::Kind::Int) {
    return Value::intValue(lhs.int_value + rhs.int_value);
  }
  if (lhs.kind == Value::Kind::Ptr && rhs.kind == Value::Kind::Int) {
    return Value::ptrValue(lhs.space, lhs.base, lhs.offset + rhs.int_value);
  }
  if (lhs.kind == Value::Kind::Int && rhs.kind == Value::Kind::Ptr) {
    return Value::ptrValue(rhs.space, rhs.base, rhs.offset + lhs.int_value);
  }
  return {};
}

Value subValues(const Value &lhs, const Value &rhs) {
  if (lhs.kind == Value::Kind::Int && rhs.kind == Value::Kind::Int) {
    return Value::intValue(lhs.int_value - rhs.int_value);
  }
  if (lhs.kind == Value::Kind::Ptr && rhs.kind == Value::Kind::Int) {
    return Value::ptrValue(lhs.space, lhs.base, lhs.offset - rhs.int_value);
  }
  return {};
}

Value mulValues(const Value &lhs, const Value &rhs) {
  if (lhs.kind == Value::Kind::Int && rhs.kind == Value::Kind::Int) {
    return Value::intValue(lhs.int_value * rhs.int_value);
  }
  return {};
}

void badOperands(ThreadState &thread, const IrInst &inst,
                 AnalysisReport &report) {
  report.push(errorDiagnostic("invalid-input",
                              "invalid operands for '" + inst.text + "'",
                              inst.span, thread.tid));
  thread.done = true;
}

Value evalOperand(ThreadState &thread, const std::string &operand,
                  AnalysisReport &report, const IrInst &inst) {
  std::string op = trim(operand);
  if (auto value = parseInt(op)) {
    return Value::intValue(*value);
  }
  if (op == "true") {
    return Value::boolValue(true);
  }
  if (op == "false") {
    return Value::boolValue(false);
  }
  auto it = thread.regs.find(op);
  if (it != thread.regs.end()) {
    return it->second;
  }
  if (startsWith(op, "%")) {
    report.push(errorDiagnostic("uninitialized-read",
                                "read from undefined register '" + op + "'",
                                inst.span, thread.tid));
    return {};
  }
  return Value::symbolValue(op);
}

struct Address {
  std::string space;
  std::string base;
  int64_t offset = 0;
};

std::string sharedByteKey(const Address &addr, uint32_t byte) {
  return addr.base + "+" + std::to_string(addr.offset + byte);
}

void addAddrTerm(ThreadState &thread, const std::string &term,
                 std::optional<std::pair<std::string, std::string>> &base,
                 int64_t &offset, const IrInst &inst, AnalysisReport &report) {
  std::string clean = trim(term);
  if (auto value = parseInt(clean)) {
    offset += *value;
    return;
  }
  auto reg = thread.regs.find(clean);
  if (reg != thread.regs.end() && reg->second.kind == Value::Kind::Int) {
    offset += reg->second.int_value;
    return;
  }
  if (reg != thread.regs.end() && reg->second.kind == Value::Kind::Ptr) {
    base = std::make_pair(reg->second.space, reg->second.base);
    offset += reg->second.offset;
    return;
  }
  if (startsWith(clean, "%")) {
    report.push(errorDiagnostic("unsupported",
                                "address register '" + clean +
                                    "' is not a concrete pointer or integer",
                                inst.span, thread.tid));
    return;
  }
  base = std::make_pair(inst.opcode.find(".shared") != std::string::npos
                            ? std::string("shared")
                            : std::string("global"),
                        clean);
}

std::optional<Address> evalAddress(ThreadState &thread,
                                   const std::string &operand,
                                   const IrInst &inst, AnalysisReport &report) {
  std::string inner = stripOuter(operand, '[', ']');
  std::optional<std::pair<std::string, std::string>> base;
  int64_t offset = 0;
  for (const std::string &term : splitOperands(inner)) {
    std::istringstream plus_stream(term);
    std::string plus_term;
    while (std::getline(plus_stream, plus_term, '+')) {
      addAddrTerm(thread, plus_term, base, offset, inst, report);
    }
  }
  if (!base) {
    report.push(errorDiagnostic(
        "unsupported", "address is not statically resolvable: " + operand,
        inst.span, thread.tid));
    return std::nullopt;
  }
  return Address{base->first, base->second, offset};
}

void checkOOB(const std::string &base, int64_t offset, uint32_t width,
              const std::unordered_map<std::string, uint64_t> &extents,
              const IrInst &inst, AnalysisReport &report, uint32_t tid) {
  auto extent = extents.find(base);
  if (extent == extents.end()) {
    return;
  }
  if (offset < 0 ||
      static_cast<uint64_t>(offset) + static_cast<uint64_t>(width) >
          extent->second) {
    report.push(errorDiagnostic("out-of-bounds",
                                inst.opcode + " accesses " + base + "+" +
                                    std::to_string(offset) + " width " +
                                    std::to_string(width) + ", outside " +
                                    std::to_string(extent->second) + " bytes",
                                inst.span, tid));
  }
}

std::unordered_map<std::string, Value> builtinRegs(uint32_t tid,
                                                   const KernelConfig &config) {
  std::unordered_map<std::string, Value> regs;
  uint32_t x = tid % config.block_dim[0];
  uint32_t y = (tid / config.block_dim[0]) % config.block_dim[1];
  uint32_t z = tid / (config.block_dim[0] * config.block_dim[1]);
  regs["%tid.x"] = Value::intValue(x);
  regs["%tid.y"] = Value::intValue(y);
  regs["%tid.z"] = Value::intValue(z);
  regs["%ntid.x"] = Value::intValue(config.block_dim[0]);
  regs["%ntid.y"] = Value::intValue(config.block_dim[1]);
  regs["%ntid.z"] = Value::intValue(config.block_dim[2]);
  regs["%ctaid.x"] = Value::intValue(config.block_idx[0]);
  regs["%ctaid.y"] = Value::intValue(config.block_idx[1]);
  regs["%ctaid.z"] = Value::intValue(config.block_idx[2]);
  return regs;
}

bool predicateEnabled(const ThreadState &thread, const IrInst &inst) {
  if (!inst.predicate) {
    return true;
  }
  std::string name = *inst.predicate;
  bool negated = startsWith(name, "!");
  if (negated) {
    name.erase(name.begin());
  }
  auto it = thread.regs.find(name);
  bool value = it != thread.regs.end() &&
               it->second.kind == Value::Kind::Bool && it->second.bool_value;
  return negated ? !value : value;
}

void assignUnary(ThreadState &thread, const IrInst &inst,
                 AnalysisReport &report) {
  if (inst.operands.size() != 2) {
    badOperands(thread, inst, report);
    return;
  }
  thread.regs[inst.operands[0]] =
      evalOperand(thread, inst.operands[1], report, inst);
  ++thread.pc;
}

void assignBinary(ThreadState &thread, const IrInst &inst,
                  AnalysisReport &report,
                  Value (*op)(const Value &, const Value &)) {
  if (inst.operands.size() != 3) {
    badOperands(thread, inst, report);
    return;
  }
  Value lhs = evalOperand(thread, inst.operands[1], report, inst);
  Value rhs = evalOperand(thread, inst.operands[2], report, inst);
  thread.regs[inst.operands[0]] = op(lhs, rhs);
  ++thread.pc;
}

void assignTernary(ThreadState &thread, const IrInst &inst,
                   AnalysisReport &report) {
  if (inst.operands.size() != 4) {
    badOperands(thread, inst, report);
    return;
  }
  Value a = evalOperand(thread, inst.operands[1], report, inst);
  Value b = evalOperand(thread, inst.operands[2], report, inst);
  Value c = evalOperand(thread, inst.operands[3], report, inst);
  thread.regs[inst.operands[0]] = addValues(mulValues(a, b), c);
  ++thread.pc;
}

void setPredicate(ThreadState &thread, const IrInst &inst,
                  AnalysisReport &report) {
  if (inst.operands.size() < 3) {
    badOperands(thread, inst, report);
    return;
  }
  Value lhs = evalOperand(thread, inst.operands[1], report, inst);
  Value rhs = evalOperand(thread, inst.operands[2], report, inst);
  std::optional<bool> result;
  if (lhs.kind == Value::Kind::Int && rhs.kind == Value::Kind::Int) {
    std::string kind = conditionKind(inst.opcode);
    if (kind == "eq") {
      result = lhs.int_value == rhs.int_value;
    } else if (kind == "ne") {
      result = lhs.int_value != rhs.int_value;
    } else if (kind == "lt") {
      result = lhs.int_value < rhs.int_value;
    } else if (kind == "le") {
      result = lhs.int_value <= rhs.int_value;
    } else if (kind == "gt") {
      result = lhs.int_value > rhs.int_value;
    } else if (kind == "ge") {
      result = lhs.int_value >= rhs.int_value;
    }
  }
  if (result) {
    thread.regs[inst.operands[0]] = Value::boolValue(*result);
  } else {
    report.push(errorDiagnostic("unsupported",
                                "predicate '" + inst.text +
                                    "' is not statically resolvable",
                                inst.span, thread.tid));
  }
  ++thread.pc;
}

void selp(ThreadState &thread, const IrInst &inst, AnalysisReport &report) {
  if (inst.operands.size() != 4) {
    badOperands(thread, inst, report);
    return;
  }
  auto pred = thread.regs.find(inst.operands[3]);
  bool enabled = pred != thread.regs.end() &&
                 pred->second.kind == Value::Kind::Bool &&
                 pred->second.bool_value;
  thread.regs[inst.operands[0]] = evalOperand(
      thread, enabled ? inst.operands[1] : inst.operands[2], report, inst);
  ++thread.pc;
}

void branch(const KernelIr &ir, ThreadState &thread, const IrInst &inst,
            AnalysisReport &report) {
  if (inst.operands.empty()) {
    badOperands(thread, inst, report);
    return;
  }
  auto target = ir.labels.find(inst.operands.front());
  if (target == ir.labels.end()) {
    report.push(errorDiagnostic(
        "invalid-input", "unknown branch label '" + inst.operands.front() + "'",
        inst.span, thread.tid));
    thread.done = true;
    return;
  }
  thread.pc = target->second;
}

void blockBarrier(ThreadState &thread, const IrInst &inst,
                  const KernelConfig &config, AnalysisReport &report) {
  SyncSet set;
  if (startsWith(inst.opcode, "bar.warp.sync")) {
    if (inst.operands.empty()) {
      report.push(errorDiagnostic(
          "unsupported", "bar.warp.sync requires a statically known mask in v1",
          inst.span, thread.tid));
      return;
    }
    auto mask = parseInt(inst.operands.front());
    if (!mask) {
      report.push(errorDiagnostic(
          "unsupported", "bar.warp.sync requires a statically known mask in v1",
          inst.span, thread.tid));
      return;
    }
    uint32_t warp_start = (thread.tid / 32) * 32;
    for (uint32_t lane = 0; lane < 32; ++lane) {
      uint32_t tid = warp_start + lane;
      if (tid < config.threadCount() && ((*mask >> lane) & 1) == 1) {
        set.threads.push_back(tid);
      }
    }
  } else {
    for (uint32_t tid = 0; tid < config.threadCount(); ++tid) {
      set.threads.push_back(tid);
    }
  }
  if (!set.threads.empty()) {
    thread.blocked = std::move(set);
  }
}

void load(ThreadState &thread, const IrInst &inst,
          std::unordered_map<std::string, MemoryCell> &shared,
          const KernelConfig &config, AnalysisReport &report) {
  if (inst.operands.size() != 2) {
    badOperands(thread, inst, report);
    return;
  }
  if (inst.opcode.find(".param") != std::string::npos) {
    thread.regs[inst.operands[0]] =
        Value::ptrValue("global", memoryBase(inst.operands[1]), 0);
    ++thread.pc;
    return;
  }
  uint32_t width = accessWidth(inst.opcode);
  std::optional<Address> addr =
      evalAddress(thread, inst.operands[1], inst, report);
  if (!addr) {
    ++thread.pc;
    return;
  }
  if (addr->space == "shared") {
    checkOOB(addr->base, addr->offset, width, config.pointer_extents, inst,
             report, thread.tid);
    std::optional<Value> loaded;
    bool has_uninitialized_byte = false;
    for (uint32_t byte = 0; byte < width; ++byte) {
      std::string key = sharedByteKey(*addr, byte);
      MemoryCell &cell = shared[key];
      if (cell.events.writer && cell.events.writer->first != thread.tid &&
          cell.events.writer->second.count(thread.tid) != 0) {
        report.push(
            errorDiagnostic("data-race",
                            "unsynchronized read of shared address " + key +
                                " after write by thread " +
                                std::to_string(cell.events.writer->first),
                            inst.span, thread.tid));
      }
      cell.events.readers[thread.tid] = allPending(thread.tid, config);
      if (cell.value && !loaded) {
        loaded = cell.value;
      }
      has_uninitialized_byte = has_uninitialized_byte || !cell.value;
    }
    if (has_uninitialized_byte) {
      report.push(errorDiagnostic("uninitialized-read",
                                  "read from uninitialized shared address " +
                                      addr->base + "+" +
                                      std::to_string(addr->offset),
                                  inst.span, thread.tid));
    }
    thread.regs[inst.operands[0]] = loaded.value_or(Value{});
  } else if (addr->space == "global") {
    checkOOB(addr->base, addr->offset, width, config.pointer_extents, inst,
             report, thread.tid);
    report.footprints.push_back(MemoryFootprint{
        "global", addr->base, addr->offset, width, thread.tid, false});
    thread.regs[inst.operands[0]] = Value::symbolValue(
        addr->base + "[" + std::to_string(addr->offset) + "]");
  } else {
    report.push(errorDiagnostic("unsupported",
                                "unsupported load space '" + addr->space + "'",
                                inst.span, thread.tid));
  }
  ++thread.pc;
}

void store(ThreadState &thread, const IrInst &inst,
           std::unordered_map<std::string, MemoryCell> &shared,
           const KernelConfig &config, AnalysisReport &report) {
  if (inst.operands.size() != 2) {
    badOperands(thread, inst, report);
    return;
  }
  uint32_t width = accessWidth(inst.opcode);
  Value value = evalOperand(thread, inst.operands[1], report, inst);
  std::optional<Address> addr =
      evalAddress(thread, inst.operands[0], inst, report);
  if (!addr) {
    ++thread.pc;
    return;
  }
  if (addr->space == "shared") {
    checkOOB(addr->base, addr->offset, width, config.pointer_extents, inst,
             report, thread.tid);
    for (uint32_t byte = 0; byte < width; ++byte) {
      std::string key = sharedByteKey(*addr, byte);
      MemoryCell &cell = shared[key];
      for (const auto &[reader, pending] : cell.events.readers) {
        if (reader != thread.tid && pending.count(thread.tid) != 0) {
          report.push(errorDiagnostic(
              "data-race",
              "unsynchronized write to shared address " + key +
                  " after read by thread " + std::to_string(reader),
              inst.span, thread.tid));
        }
      }
      if (cell.events.writer && cell.events.writer->first != thread.tid &&
          cell.events.writer->second.count(thread.tid) != 0) {
        report.push(
            errorDiagnostic("data-race",
                            "unsynchronized write to shared address " + key +
                                " after write by thread " +
                                std::to_string(cell.events.writer->first),
                            inst.span, thread.tid));
      }
      cell.value = value;
      cell.events.writer =
          std::make_pair(thread.tid, allThreads(config.threadCount()));
    }
  } else if (addr->space == "global") {
    checkOOB(addr->base, addr->offset, width, config.pointer_extents, inst,
             report, thread.tid);
    report.footprints.push_back(MemoryFootprint{
        "global", addr->base, addr->offset, width, thread.tid, true});
  } else {
    report.push(errorDiagnostic("unsupported",
                                "unsupported store space '" + addr->space + "'",
                                inst.span, thread.tid));
  }
  ++thread.pc;
}

bool releaseBarriers(std::vector<ThreadState> &threads,
                     std::unordered_map<std::string, MemoryCell> &shared) {
  bool progressed = false;
  std::vector<SyncSet> blocked;
  for (const ThreadState &thread : threads) {
    if (thread.blocked) {
      blocked.push_back(*thread.blocked);
    }
  }
  for (const SyncSet &set : blocked) {
    bool releasable = true;
    for (uint32_t tid : set.threads) {
      auto it =
          std::find_if(threads.begin(), threads.end(),
                       [&](const ThreadState &t) { return t.tid == tid; });
      if (it != threads.end() && !it->done &&
          (!it->blocked || !(*it->blocked == set))) {
        releasable = false;
        break;
      }
    }
    if (!releasable) {
      continue;
    }
    std::set<uint32_t> release(set.threads.begin(), set.threads.end());
    for (ThreadState &thread : threads) {
      if (thread.blocked && *thread.blocked == set) {
        thread.blocked = std::nullopt;
        ++thread.pc;
        progressed = true;
      }
    }
    for (auto &[_, cell] : shared) {
      for (auto &[reader, pending] : cell.events.readers) {
        if (release.count(reader) != 0) {
          for (uint32_t tid : release) {
            pending.erase(tid);
          }
        }
      }
      if (cell.events.writer && release.count(cell.events.writer->first) != 0) {
        for (uint32_t tid : release) {
          cell.events.writer->second.erase(tid);
        }
      }
    }
  }
  for (auto &[_, cell] : shared) {
    for (auto it = cell.events.readers.begin();
         it != cell.events.readers.end();) {
      if (it->second.empty()) {
        it = cell.events.readers.erase(it);
      } else {
        ++it;
      }
    }
  }
  return progressed;
}

void stepThread(const KernelIr &ir, const KernelConfig &config,
                ThreadState &thread,
                std::unordered_map<std::string, MemoryCell> &shared,
                AnalysisReport &report) {
  const IrInst &inst = ir.instructions[thread.pc];
  if (!predicateEnabled(thread, inst)) {
    ++thread.pc;
    return;
  }
  std::string klass = opcodeClass(inst.opcode);
  if (klass == "ret" || klass == "exit") {
    thread.done = true;
    ++thread.pc;
  } else if (klass == "mov" || klass == "cvta") {
    assignUnary(thread, inst, report);
  } else if (klass == "add") {
    assignBinary(thread, inst, report, addValues);
  } else if (klass == "sub") {
    assignBinary(thread, inst, report, subValues);
  } else if (klass == "mul") {
    assignBinary(thread, inst, report, mulValues);
  } else if (klass == "mad") {
    assignTernary(thread, inst, report);
  } else if (klass == "setp") {
    setPredicate(thread, inst, report);
  } else if (klass == "selp") {
    selp(thread, inst, report);
  } else if (klass == "bra") {
    branch(ir, thread, inst, report);
  } else if (klass == "bar") {
    blockBarrier(thread, inst, config, report);
  } else if (klass == "ld") {
    load(thread, inst, shared, config, report);
  } else if (klass == "st") {
    store(thread, inst, shared, config, report);
  } else {
    report.push(errorDiagnostic(
        "unsupported", "unsupported PTX instruction '" + inst.opcode + "'",
        inst.span, thread.tid));
    thread.done = true;
  }
}

std::array<uint32_t, 3> parseVec3(const std::string &value) {
  std::string inner = stripOuter(value, '[', ']');
  std::vector<std::string> parts = splitOperands(inner);
  if (parts.size() != 3) {
    throw std::runtime_error("expected three entries: " + inner);
  }
  std::array<uint32_t, 3> out{};
  for (size_t i = 0; i < 3; ++i) {
    auto parsed = parseInt(parts[i]);
    if (!parsed || *parsed < 0) {
      throw std::runtime_error("invalid vec3: " + inner);
    }
    out[i] = static_cast<uint32_t>(*parsed);
  }
  return out;
}

std::string escapeJson(const std::string &input) {
  std::string out;
  for (char ch : input) {
    if (ch == '\\') {
      out += "\\\\";
    } else if (ch == '"') {
      out += "\\\"";
    } else if (ch == '\n') {
      out += "\\n";
    } else {
      out += ch;
    }
  }
  return out;
}

} // namespace

const char *toString(Severity severity) {
  switch (severity) {
  case Severity::Error:
    return "error";
  case Severity::Warning:
    return "warning";
  case Severity::Note:
    return "note";
  }
  return "error";
}

const char *toString(ReportStatus status) {
  switch (status) {
  case ReportStatus::Safe:
    return "safe";
  case ReportStatus::Unsafe:
    return "unsafe";
  case ReportStatus::Unsupported:
    return "unsupported";
  case ReportStatus::InvalidInput:
    return "invalid-input";
  case ReportStatus::Unknown:
    return "unknown";
  }
  return "unknown";
}

void AnalysisReport::push(Diagnostic diagnostic) {
  diagnostics.push_back(std::move(diagnostic));
  refreshStatus();
}

void AnalysisReport::refreshStatus() {
  if (std::any_of(
          diagnostics.begin(), diagnostics.end(),
          [](const Diagnostic &d) { return d.code == "unsupported"; })) {
    status = ReportStatus::Unsupported;
  } else if (std::any_of(diagnostics.begin(), diagnostics.end(),
                         [](const Diagnostic &d) {
                           return d.severity == Severity::Error;
                         })) {
    status = ReportStatus::Unsafe;
  } else {
    status = ReportStatus::Safe;
  }
}

std::string AnalysisReport::toText() const {
  std::ostringstream out;
  out << "status: " << toString(status) << "\n";
  for (const Diagnostic &diagnostic : diagnostics) {
    out << toString(diagnostic.severity) << " [" << diagnostic.code << "] ";
    if (diagnostic.span) {
      out << diagnostic.span->line << ":" << diagnostic.span->column;
    } else {
      out << "-";
    }
    if (diagnostic.thread) {
      out << " thread=" << *diagnostic.thread;
    }
    out << ": " << diagnostic.message << "\n";
  }
  if (!footprints.empty()) {
    out << "footprints:\n";
    for (const MemoryFootprint &fp : footprints) {
      out << "  " << (fp.is_write ? "write" : "read") << " " << fp.space << " "
          << fp.base << "+" << fp.offset << " width=" << fp.width
          << " thread=" << fp.thread << "\n";
    }
  }
  return out.str();
}

std::string AnalysisReport::toJson() const {
  std::ostringstream out;
  out << "{\"status\":\"" << toString(status) << "\",\"diagnostics\":[";
  for (size_t i = 0; i < diagnostics.size(); ++i) {
    const Diagnostic &d = diagnostics[i];
    if (i != 0) {
      out << ",";
    }
    out << "{\"code\":\"" << escapeJson(d.code) << "\",\"severity\":\""
        << toString(d.severity) << "\",\"message\":\"" << escapeJson(d.message)
        << "\",\"span\":";
    if (d.span) {
      out << "{\"line\":" << d.span->line << ",\"column\":" << d.span->column
          << "}";
    } else {
      out << "null";
    }
    out << ",\"thread\":";
    if (d.thread) {
      out << *d.thread;
    } else {
      out << "null";
    }
    out << "}";
  }
  out << "],\"footprints\":[";
  for (size_t i = 0; i < footprints.size(); ++i) {
    const MemoryFootprint &fp = footprints[i];
    if (i != 0) {
      out << ",";
    }
    out << "{\"space\":\"" << escapeJson(fp.space) << "\",\"base\":\""
        << escapeJson(fp.base) << "\",\"offset\":" << fp.offset
        << ",\"width\":" << fp.width << ",\"thread\":" << fp.thread
        << ",\"is_write\":" << (fp.is_write ? "true" : "false") << "}";
  }
  out << "]}";
  return out.str();
}

uint32_t KernelConfig::threadCount() const {
  return block_dim[0] * block_dim[1] * block_dim[2];
}

KernelConfig KernelConfig::fromConfigText(const std::string &text) {
  KernelConfig config;
  std::string section;
  std::istringstream stream(text);
  std::string raw;
  while (std::getline(stream, raw)) {
    size_t comment = raw.find('#');
    std::string line = trim(raw.substr(0, comment));
    if (line.empty()) {
      continue;
    }
    if (line.front() == '[' && line.back() == ']') {
      section = stripOuter(line, '[', ']');
      continue;
    }
    size_t equals = line.find('=');
    if (equals == std::string::npos) {
      throw std::runtime_error("invalid config line: " + line);
    }
    std::string key = stripOuter(trim(line.substr(0, equals)), '"', '"');
    std::string value = trim(line.substr(equals + 1));
    if ((section.empty() || section == "kernel") && key == "entry") {
      config.entry = stripOuter(value, '"', '"');
    } else if ((section.empty() || section == "kernel") && key == "block_dim") {
      config.block_dim = parseVec3(value);
    } else if ((section.empty() || section == "kernel") && key == "block_idx") {
      config.block_idx = parseVec3(value);
    } else if (section == "pointers") {
      auto bytes = parseInt(stripOuter(value, '"', '"'));
      if (!bytes || *bytes < 0) {
        throw std::runtime_error("invalid pointer extent for " + key + ": " +
                                 value);
      }
      config.pointer_extents[key] = static_cast<uint64_t>(*bytes);
    } else {
      throw std::runtime_error("unknown config key " + key + " in section [" +
                               section + "]");
    }
  }
  return config;
}

ModuleAst parseModule(const std::string &source) {
  ModuleAst module;
  std::vector<std::pair<size_t, std::string>> lines = logicalLines(source);
  size_t i = 0;
  while (i < lines.size()) {
    const auto &[line_no, line] = lines[i];
    if (startsWith(line, ".version")) {
      std::vector<std::string> parts = splitWhitespace(line);
      if (parts.size() > 1) {
        module.version = parts[1];
      }
      ++i;
    } else if (startsWith(line, ".shared")) {
      if (auto decl = parseDecl(line, line_no)) {
        module.declarations.push_back(*decl);
      }
      ++i;
    } else if (startsWith(line, ".entry") ||
               startsWith(line, ".visible .entry")) {
      auto [entry, next] = parseEntry(lines, i);
      module.entries.push_back(std::move(entry));
      i = next;
    } else {
      ++i;
    }
  }
  return module;
}

KernelIr lowerKernel(const ModuleAst &module,
                     const std::optional<std::string> &entry_name) {
  const EntryAst &entry = selectEntry(module, entry_name);
  KernelIr ir;
  ir.name = entry.name;
  ir.params = entry.params;
  for (const DeclAst &decl : module.declarations) {
    if (decl.space == ".shared") {
      ir.shared[decl.name] = decl.bytes;
    }
  }
  for (const StmtAst &stmt : entry.body) {
    if (stmt.kind == StmtKind::Label) {
      ir.labels[stmt.name] = ir.instructions.size();
    } else if (stmt.kind == StmtKind::Directive) {
      if (auto shared = parseSharedDirective(stmt.text)) {
        ir.shared[shared->first] = shared->second;
      } else if (startsWith(stmt.text, ".reg") ||
                 startsWith(stmt.text, ".local")) {
        continue;
      } else {
        ir.instructions.push_back(IrInst{std::nullopt,
                                         "unsupported.directive",
                                         {stmt.text},
                                         stmt.text,
                                         stmt.span});
      }
    } else {
      ir.instructions.push_back(
          IrInst{stmt.instruction.predicate, stmt.instruction.opcode,
                 stmt.instruction.operands, stmt.instruction.text,
                 stmt.instruction.span});
    }
  }
  return ir;
}

AnalysisReport analyzeKernel(const ModuleAst &module,
                             const KernelConfig &config) {
  AnalysisReport report;
  KernelIr ir;
  try {
    ir = lowerKernel(module, config.entry);
  } catch (const std::exception &err) {
    report.push(errorDiagnostic("invalid-input", err.what(), std::nullopt));
    report.status = ReportStatus::InvalidInput;
    return report;
  }

  if (config.threadCount() == 0) {
    report.push(errorDiagnostic("invalid-input",
                                "block_dim must contain at least one thread",
                                std::nullopt));
    return report;
  }

  std::unordered_map<std::string, MemoryCell> shared;
  for (const auto &[name, bytes] : ir.shared) {
    for (uint64_t offset = 0; offset < bytes; ++offset) {
      shared[name + "+" + std::to_string(offset)] = MemoryCell{};
    }
  }

  std::vector<ThreadState> threads;
  for (uint32_t tid = 0; tid < config.threadCount(); ++tid) {
    threads.push_back(
        ThreadState{tid, 0, false, std::nullopt, builtinRegs(tid, config)});
  }

  size_t max_steps =
      std::max<size_t>(ir.instructions.size() * config.threadCount(), 1) * 20;
  size_t steps = 0;
  while (true) {
    if (std::all_of(threads.begin(), threads.end(),
                    [](const ThreadState &t) { return t.done; })) {
      break;
    }
    if (steps > max_steps) {
      report.push(errorDiagnostic("unknown",
                                  "execution step bound exceeded; likely "
                                  "unsupported loop or unresolved control flow",
                                  std::nullopt));
      break;
    }
    ++steps;

    bool progressed = releaseBarriers(threads, shared);
    for (ThreadState &thread : threads) {
      if (thread.done || thread.blocked) {
        continue;
      }
      if (thread.pc >= ir.instructions.size()) {
        thread.done = true;
        progressed = true;
        continue;
      }
      stepThread(ir, config, thread, shared, report);
      progressed = true;
    }
    if (!progressed) {
      std::ostringstream blocked;
      bool first = true;
      for (const ThreadState &thread : threads) {
        if (!thread.done) {
          if (!first) {
            blocked << ", ";
          }
          first = false;
          blocked << thread.tid;
        }
      }
      report.push(errorDiagnostic(
          "deadlock",
          "threads are blocked with incompatible synchronization sets: " +
              blocked.str(),
          std::nullopt));
      break;
    }
  }
  report.refreshStatus();
  return report;
}

} // namespace concurrency::cuda::ptx
