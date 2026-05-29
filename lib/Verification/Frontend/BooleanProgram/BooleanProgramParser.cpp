#include "Verification/Frontend/BooleanProgramParser.h"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace lotus {
namespace verification {
namespace frontend {

namespace {

class Parser {
public:
  explicit Parser(std::string input) : input_(std::move(input)) {}

  BooleanProgram parseProgram() {
    BooleanProgram program;
    skipTrivia();
    while (!eof()) {
      if (consumeKeyword("decl")) {
        program.globals.push_back(parseDecl());
      } else if (peekKeyword("dfs") || peekKeyword("void") ||
                 peekKeyword("bool")) {
        program.procedures.push_back(parseProcedure());
      } else {
        throw error("expected 'decl', 'dfs', 'void', or 'bool'");
      }
      skipTrivia();
    }
    return program;
  }

private:
  PredicateDecl parseDecl() {
    PredicateDecl decl;
    decl.name = parseIdentifierLike();
    expect(";");
    return decl;
  }

  Procedure parseProcedure() {
    Procedure procedure;
    procedure.dfs = consumeKeyword("dfs");
    procedure.returns_bool = consumeKeyword("bool");
    if (!procedure.returns_bool)
      expectKeyword("void");
    if (procedure.returns_bool && consume("<")) {
      skipTrivia();
      procedure.bool_width = parseUnsignedLiteral();
      expect(">");
    }
    procedure.name = parseIdentifierLike();
    expect("(");
    if (!consume(")")) {
      do {
        procedure.parameters.push_back(parseIdentifierLike());
      } while (consume(","));
      expect(")");
    }
    expectKeyword("begin");

    while (consumeKeyword("decl")) {
      procedure.locals.push_back(parseIdentifierLike());
      while (consume(","))
        procedure.locals.push_back(parseIdentifierLike());
      expect(";");
    }

    if (consumeKeyword("enforce")) {
      procedure.enforce = parseExpr();
      expect(";");
    }
    if (consumeKeyword("abortif")) {
      procedure.abortif = parseExpr();
      expect(";");
    }

    skipTrivia();
    while (!consumeKeyword("end")) {
      procedure.statements.push_back(parseStatement());
      skipTrivia();
    }
    return procedure;
  }

  Statement parseStatement() {
    Statement stmt;
    stmt.source = pending_source_;
    pending_source_ = SourceNote{};

    while (true) {
      const size_t saved = pos_;
      if (peekIdentifierLike() && peekCharAfterIdentifier() == ':') {
        std::string label = parseIdentifierLike();
        expect(":");
        if (stmt.label.empty())
          stmt.label = label;
        else
          stmt.aliases.push_back(label);
        skipTrivia();
        continue;
      }
      pos_ = saved;
      break;
    }

    if (consumeKeyword("skip")) {
      stmt.kind = StatementKind::Skip;
      expect(";");
      return stmt;
    }
    if (consumeKeyword("assume")) {
      stmt.kind = StatementKind::Assume;
      stmt.expr = parseExpr();
      expect(";");
      return stmt;
    }
    if (consumeKeyword("assert")) {
      stmt.kind = StatementKind::Assert;
      stmt.expr = parseExpr();
      expect(";");
      return stmt;
    }
    if (consumeKeyword("print")) {
      stmt.kind = StatementKind::Print;
      expect("(");
      if (!consume(")")) {
        stmt.expressions.push_back(parseExpr());
        while (consume(","))
          stmt.expressions.push_back(parseExpr());
        expect(")");
      }
      expect(";");
      return stmt;
    }
    if (consumeKeyword("goto")) {
      stmt.kind = StatementKind::Goto;
      stmt.targets.push_back(parseIdentifierLike());
      while (consume(","))
        stmt.targets.push_back(parseIdentifierLike());
      expect(";");
      return stmt;
    }
    if (consumeKeyword("if")) {
      stmt.expr = parseExpr();
      expectKeyword("then");

      {
        const size_t saved_pos = pos_;
        const unsigned saved_line = line_;
        const unsigned saved_column = column_;
        if (consumeKeyword("goto")) {
          const std::string target = parseIdentifierLike();
          expect(";");
          if (consumeKeyword("fi")) {
            expect(";");
            stmt.kind = StatementKind::Branch;
            stmt.targets.push_back(target);
            return stmt;
          }
        }
        pos_ = saved_pos;
        line_ = saved_line;
        column_ = saved_column;
      }

      stmt.kind = StatementKind::If;
      stmt.then_statements = parseNestedStatementList({"elsif", "else", "fi"});
      while (consumeKeyword("elsif")) {
        BooleanExpr guard = parseExpr();
        expectKeyword("then");
        stmt.elsif_branches.emplace_back(
            std::move(guard), parseNestedStatementList({"elsif", "else", "fi"}));
      }
      if (consumeKeyword("else"))
        stmt.else_statements = parseNestedStatementList({"fi"});
      expectKeyword("fi");
      expect(";");
      return stmt;
    }
    if (consumeKeyword("while")) {
      stmt.kind = StatementKind::While;
      stmt.expr = parseExpr();
      expectKeyword("do");
      stmt.body_statements = parseNestedStatementList({"od"});
      expectKeyword("od");
      expect(";");
      return stmt;
    }
    if (consumeKeyword("return")) {
      stmt.kind = StatementKind::Return;
      if (!consume(";")) {
        stmt.expressions.push_back(parseExpr());
        while (consume(","))
          stmt.expressions.push_back(parseExpr());
        expect(";");
      }
      return stmt;
    }
    if (consumeKeyword("start_thread")) {
      stmt.kind = StatementKind::StartThread;
      if (consumeKeyword("goto"))
        stmt.thread_target = parseIdentifierLike();
      else {
        stmt.thread_target = parseIdentifierLike();
        expect("(");
        expect(")");
      }
      expect(";");
      return stmt;
    }
    if (consumeKeyword("end_thread")) {
      stmt.kind = StatementKind::EndThread;
      expect(";");
      return stmt;
    }
    if (consumeKeyword("sync")) {
      stmt.kind = StatementKind::Sync;
      stmt.thread_target = parseIdentifierLike();
      expect(";");
      return stmt;
    }
    if (consumeKeyword("atomic_begin")) {
      stmt.kind = StatementKind::AtomicBegin;
      expect(";");
      return stmt;
    }
    if (consumeKeyword("atomic_end")) {
      stmt.kind = StatementKind::AtomicEnd;
      expect(";");
      return stmt;
    }
    if (consumeKeyword("dead")) {
      stmt.kind = StatementKind::Dead;
      stmt.dead_variables.push_back(parseIdentifierLike());
      while (consume(","))
        stmt.dead_variables.push_back(parseIdentifierLike());
      expect(";");
      return stmt;
    }

    if (peekIdentifierLike()) {
      const size_t saved = pos_;
      const unsigned saved_line = line_;
      const unsigned saved_column = column_;
      std::string callee = parseIdentifierLike();
      if (consume("(")) {
        stmt.kind = StatementKind::Call;
        stmt.callee = std::move(callee);
        if (!consume(")")) {
          stmt.expressions.push_back(parseExpr());
          while (consume(","))
            stmt.expressions.push_back(parseExpr());
          expect(")");
        }
        expect(";");
        return stmt;
      }
      pos_ = saved;
      line_ = saved_line;
      column_ = saved_column;
    }

    stmt.kind = StatementKind::Assign;
    stmt.assignment.lhs.push_back({parseIdentifierLike()});
    while (consume(","))
      stmt.assignment.lhs.push_back({parseIdentifierLike()});
    expect(":=");
    if (peekIdentifierLike()) {
      const size_t saved = pos_;
      const unsigned saved_line = line_;
      const unsigned saved_column = column_;
      std::string callee = parseIdentifierLike();
      if (consume("(")) {
        stmt.assignment.call_callee = std::move(callee);
        if (!consume(")")) {
          stmt.assignment.call_args.push_back(parseExpr());
          while (consume(","))
            stmt.assignment.call_args.push_back(parseExpr());
          expect(")");
        }
        expect(";");
        return stmt;
      }
      pos_ = saved;
      line_ = saved_line;
      column_ = saved_column;
    }
    stmt.assignment.rhs.push_back({parseExpr()});
    while (consume(","))
      stmt.assignment.rhs.push_back({parseExpr()});
    if (consumeKeyword("constrain"))
      stmt.assignment.constraint = parseExpr();
    expect(";");
    return stmt;
  }

  BooleanExpr parseExpr() { return parseTernary(); }

  BooleanExpr parseTernary() {
    auto cond = parseImplies();
    if (!consume("?"))
      return cond;
    auto then_expr = parseExpr();
    expect(":");
    auto else_expr = parseImplies();
    return BooleanExpr::makeTernary(std::move(cond), std::move(then_expr),
                                    std::move(else_expr));
  }

  BooleanExpr parseImplies() {
    auto lhs = parseOr();
    while (consume("=>"))
      lhs =
          BooleanExpr::makeBinary(ExprKind::Implies, std::move(lhs), parseOr());
    return lhs;
  }

  BooleanExpr parseOr() {
    auto lhs = parseXor();
    while (true) {
      if (consume("||") || consume("|")) {
        lhs = BooleanExpr::makeBinary(ExprKind::Or, std::move(lhs), parseXor());
        continue;
      }
      break;
    }
    return lhs;
  }

  BooleanExpr parseXor() {
    auto lhs = parseAnd();
    while (consume("^"))
      lhs = BooleanExpr::makeBinary(ExprKind::Xor, std::move(lhs), parseAnd());
    return lhs;
  }

  BooleanExpr parseAnd() {
    auto lhs = parseEq();
    while (true) {
      if (consume("&&") || consume("&")) {
        lhs = BooleanExpr::makeBinary(ExprKind::And, std::move(lhs), parseEq());
        continue;
      }
      break;
    }
    return lhs;
  }

  BooleanExpr parseEq() {
    auto lhs = parseUnary();
    while (true) {
      if (consume("=")) {
        lhs = BooleanExpr::makeBinary(ExprKind::Eq, std::move(lhs), parseUnary());
      } else if (consume("!=") || consume("<>")) {
        lhs =
            BooleanExpr::makeBinary(ExprKind::Neq, std::move(lhs), parseUnary());
      } else {
        break;
      }
    }
    return lhs;
  }

  BooleanExpr parseUnary() {
    if (consume("!"))
      return BooleanExpr::makeUnary(ExprKind::Not, parseUnary());
    if (consume("~"))
      return BooleanExpr::makeUnary(ExprKind::Not, parseUnary());
    if (consume("'"))
      return BooleanExpr::makeVariable(parseIdentifierLike(), true);
    return parsePrimary();
  }

  BooleanExpr parsePrimary() {
    if (consume("(")) {
      auto expr = parseExpr();
      expect(")");
      return expr;
    }
    if (consume("*"))
      return BooleanExpr::makeNondet();
    if (consumeKeyword("schoose")) {
      expect("[");
      auto lhs = parseExpr();
      expect(",");
      auto rhs = parseExpr();
      expect("]");
      return BooleanExpr::makeChoose(std::move(lhs), std::move(rhs));
    }
    if (consume("0"))
      return BooleanExpr::makeConstant(false);
    if (consume("1"))
      return BooleanExpr::makeConstant(true);
    return BooleanExpr::makeVariable(parseIdentifierLike());
  }

  std::vector<Statement>
  parseNestedStatementList(std::initializer_list<const char *> terminators) {
    std::vector<Statement> statements;
    skipTrivia();
    while (!atAnyKeyword(terminators)) {
      statements.push_back(parseStatement());
      skipTrivia();
    }
    return statements;
  }

  bool eof() const { return pos_ >= input_.size(); }

  void skipTrivia() {
    while (!eof()) {
      if (std::isspace(static_cast<unsigned char>(input_[pos_]))) {
        advance(input_[pos_]);
        continue;
      }
      if (startsWith(input_.substr(pos_), "//")) {
        parseLineComment();
        continue;
      }
      break;
    }
  }

  void parseLineComment() {
    const std::string comment = readUntil('\n');
    parseSourceComment(comment);
    if (!eof() && input_[pos_] == '\n')
      advance('\n');
  }

  void parseSourceComment(const std::string &comment) {
    std::string trimmed = trim(comment.substr(2));
    if (!startsWith(trimmed, "file "))
      return;

    SourceNote note;
    const std::string line_marker = " line ";
    const std::string function_marker = " function ";
    const size_t line_pos = trimmed.find(line_marker);
    if (line_pos == std::string::npos)
      return;
    note.file = trimmed.substr(5, line_pos - 5);
    size_t function_pos =
        trimmed.find(function_marker, line_pos + line_marker.size());
    std::string line_text;
    if (function_pos == std::string::npos) {
      line_text = trimmed.substr(line_pos + line_marker.size());
    } else {
      line_text =
          trimmed.substr(line_pos + line_marker.size(),
                         function_pos - (line_pos + line_marker.size()));
      note.function = trimmed.substr(function_pos + function_marker.size());
    }
    note.line =
        static_cast<unsigned>(std::strtoul(line_text.c_str(), nullptr, 10));
    pending_source_ = std::move(note);
  }

  bool peekKeyword(const std::string &keyword) {
    skipTrivia();
    if (!startsWith(input_.substr(pos_), keyword))
      return false;
    const size_t end = pos_ + keyword.size();
    if (end < input_.size() && isIdentifierChar(input_[end]))
      return false;
    return true;
  }

  bool atAnyKeyword(std::initializer_list<const char *> keywords) {
    for (const char *keyword : keywords) {
      if (peekKeyword(keyword))
        return true;
    }
    return false;
  }

  bool consumeKeyword(const std::string &keyword) {
    if (!peekKeyword(keyword))
      return false;
    pos_ += keyword.size();
    column_ += static_cast<unsigned>(keyword.size());
    return true;
  }

  void expectKeyword(const std::string &keyword) {
    if (!consumeKeyword(keyword))
      throw error("expected keyword '" + keyword + "'");
  }

  bool consume(const std::string &token) {
    skipTrivia();
    if (!startsWith(input_.substr(pos_), token))
      return false;
    for (char ch : token)
      advance(ch);
    return true;
  }

  void expect(const std::string &token) {
    if (!consume(token))
      throw error("expected '" + token + "'");
  }

  bool peekIdentifierLike() {
    skipTrivia();
    if (eof())
      return false;
    const char ch = input_[pos_];
    return isIdentifierStart(ch) || ch == '{' || ch == '_';
  }

  char peekCharAfterIdentifier() {
    size_t scan = pos_;
    if (input_[scan] == '{') {
      while (scan < input_.size() && input_[scan] != '}')
        ++scan;
      if (scan < input_.size())
        ++scan;
      return scan < input_.size() ? input_[scan] : '\0';
    }
    while (scan < input_.size() && isIdentifierChar(input_[scan]))
      ++scan;
    return scan < input_.size() ? input_[scan] : '\0';
  }

  std::string parseIdentifierLike() {
    skipTrivia();
    if (eof())
      throw error("unexpected end of input");

    if (input_[pos_] == '{') {
      const size_t start = pos_;
      advance('{');
      while (!eof() && input_[pos_] != '}')
        advance(input_[pos_]);
      expect("}");
      return input_.substr(start, pos_ - start);
    }

    if (!isIdentifierStart(input_[pos_]) && input_[pos_] != '_')
      throw error("expected identifier");

    const size_t start = pos_;
    advance(input_[pos_]);
    while (!eof() && isIdentifierChar(input_[pos_]))
      advance(input_[pos_]);
    return input_.substr(start, pos_ - start);
  }

  unsigned parseUnsignedLiteral() {
    skipTrivia();
    if (eof() || !std::isdigit(static_cast<unsigned char>(input_[pos_])))
      throw error("expected integer");
    const size_t start = pos_;
    while (!eof() && std::isdigit(static_cast<unsigned char>(input_[pos_])))
      advance(input_[pos_]);
    return static_cast<unsigned>(
        std::strtoul(input_.substr(start, pos_ - start).c_str(), nullptr, 10));
  }

  FrontendException error(const std::string &message) const {
    return FrontendException({message, line_, column_});
  }

  void advance(char ch) {
    ++pos_;
    if (ch == '\n') {
      ++line_;
      column_ = 1;
    } else {
      ++column_;
    }
  }

  std::string readUntil(char terminal) {
    const size_t start = pos_;
    while (!eof() && input_[pos_] != terminal)
      ++pos_;
    column_ += static_cast<unsigned>(pos_ - start);
    return input_.substr(start, pos_ - start);
  }

  static bool startsWith(const std::string &text, const std::string &prefix) {
    return text.compare(0, prefix.size(), prefix) == 0;
  }

  static bool isIdentifierStart(char ch) {
    return std::isalpha(static_cast<unsigned char>(ch)) != 0 || ch == '$';
  }

  static bool isIdentifierChar(char ch) {
    return std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '_' ||
           ch == '$' || ch == '.';
  }

  static std::string trim(std::string text) {
    while (!text.empty() &&
           std::isspace(static_cast<unsigned char>(text.front())) != 0)
      text.erase(text.begin());
    while (!text.empty() &&
           std::isspace(static_cast<unsigned char>(text.back())) != 0)
      text.pop_back();
    return text;
  }

  std::string input_;
  size_t pos_ = 0;
  unsigned line_ = 1;
  unsigned column_ = 1;
  SourceNote pending_source_;
};

} // namespace

BooleanProgram parseBooleanProgram(const std::string &text) {
  return Parser(text).parseProgram();
}

BooleanProgram parseBooleanProgramFile(const std::string &path) {
  std::ifstream input(path);
  if (!input)
    throw std::runtime_error("failed to open Boolean program: " + path);
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return parseBooleanProgram(buffer.str());
}

} // namespace frontend
} // namespace verification
} // namespace lotus
