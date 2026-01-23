#include "IR/PDG/CypherQuery.h"
#include "IR/PDG/DebugInfoUtils.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <sstream>

#include "llvm/IR/Instructions.h"
#include "llvm/Support/raw_ostream.h"

namespace pdg {

static std::string toLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

static const char *graphNodeTypeName(GraphNodeType t) {
  switch (t) {
  case GraphNodeType::INST_FUNCALL:
    return "INST_FUNCALL";
  case GraphNodeType::INST_RET:
    return "INST_RET";
  case GraphNodeType::INST_BR:
    return "INST_BR";
  case GraphNodeType::INST_OTHER:
    return "INST_OTHER";
  case GraphNodeType::FUNC_ENTRY:
    return "FUNC_ENTRY";
  case GraphNodeType::PARAM_FORMALIN:
    return "PARAM_FORMALIN";
  case GraphNodeType::PARAM_FORMALOUT:
    return "PARAM_FORMALOUT";
  case GraphNodeType::PARAM_ACTUALIN:
    return "PARAM_ACTUALIN";
  case GraphNodeType::PARAM_ACTUALOUT:
    return "PARAM_ACTUALOUT";
  case GraphNodeType::VAR_STATICALLOCGLOBALSCOPE:
    return "VAR_STATICALLOCGLOBALSCOPE";
  case GraphNodeType::VAR_STATICALLOCMODULESCOPE:
    return "VAR_STATICALLOCMODULESCOPE";
  case GraphNodeType::VAR_STATICALLOCFUNCTIONSCOPE:
    return "VAR_STATICALLOCFUNCTIONSCOPE";
  case GraphNodeType::VAR_OTHER:
    return "VAR_OTHER";
  case GraphNodeType::FUNC:
    return "FUNC";
  case GraphNodeType::CLASS:
    return "CLASS";
  case GraphNodeType::ANNO_VAR:
    return "ANNO_VAR";
  case GraphNodeType::ANNO_GLOBAL:
    return "ANNO_GLOBAL";
  case GraphNodeType::ANNO_OTHER:
    return "ANNO_OTHER";
  }
  return "UNKNOWN";
}

static const char *edgeTypeName(EdgeType t) {
  switch (t) {
  case EdgeType::IND_CALL:
    return "IND_CALL";
  case EdgeType::CONTROLDEP_CALLINV:
    return "CONTROLDEP_CALLINV";
  case EdgeType::CONTROLDEP_CALLRET:
    return "CONTROLDEP_CALLRET";
  case EdgeType::CONTROLDEP_ENTRY:
    return "CONTROLDEP_ENTRY";
  case EdgeType::CONTROLDEP_BR:
    return "CONTROLDEP_BR";
  case EdgeType::CONTROLDEP_IND_BR:
    return "CONTROLDEP_IND_BR";
  case EdgeType::DATA_DEF_USE:
    return "DATA_DEF_USE";
  case EdgeType::DATA_RAW:
    return "DATA_RAW";
  case EdgeType::DATA_READ:
    return "DATA_READ";
  case EdgeType::DATA_ALIAS:
    return "DATA_ALIAS";
  case EdgeType::DATA_RET:
    return "DATA_RET";
  case EdgeType::PARAMETER_IN:
    return "PARAMETER_IN";
  case EdgeType::PARAMETER_OUT:
    return "PARAMETER_OUT";
  case EdgeType::PARAMETER_FIELD:
    return "PARAMETER_FIELD";
  case EdgeType::GLOBAL_DEP:
    return "GLOBAL_DEP";
  case EdgeType::VAL_DEP:
    return "VAL_DEP";
  case EdgeType::CLS_MTH:
    return "CLS_MTH";
  case EdgeType::ANNO_VAR:
    return "ANNO_VAR";
  case EdgeType::ANNO_GLOBAL:
    return "ANNO_GLOBAL";
  case EdgeType::ANNO_OTHER:
    return "ANNO_OTHER";
  case EdgeType::TYPE_OTHEREDGE:
    return "TYPE_OTHEREDGE";
  }
  return "UNKNOWN";
}

// ============================================================================
// CypherParser implementation
// ============================================================================

std::unique_ptr<CypherQuery> CypherParser::parse(const std::string &query) {
  clearError();
  trim(const_cast<std::string &>(query));

  if (query.empty()) {
    setError(CypherErrorCode::PARSE_ERROR, "Empty query", 1, 1);
    return nullptr;
  }

  size_t pos = 0;
  std::vector<std::string> tokens = tokenize(query);

  if (tokens.empty()) {
    setError(CypherErrorCode::PARSE_ERROR, "No valid tokens found", 1, 1);
    return nullptr;
  }

  return parseQuery(tokens, pos, CypherQueryParameters{});
}

void CypherParser::trim(std::string &s) {
  if (s.empty())
    return;

  size_t start = 0;
  while (start < s.size() &&
         std::isspace(static_cast<unsigned char>(s[start]))) {
    start++;
  }

  size_t end = s.size();
  while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
    end--;
  }

  s = s.substr(start, end - start);
}

bool CypherParser::isAlpha(char c) {
  return std::isalpha(static_cast<unsigned char>(c));
}

bool CypherParser::isDigit(char c) {
  return std::isdigit(static_cast<unsigned char>(c));
}

bool CypherParser::isAlphaNumeric(char c) {
  return isAlpha(c) || isDigit(c) || c == '_' || c == '$';
}

std::vector<std::string> CypherParser::tokenize(const std::string &s) {
  std::vector<std::string> tokens;
  std::string current;
  bool inString = false;
  char stringChar = '"';

  for (size_t i = 0; i < s.size(); ++i) {
    char c = s[i];

    if (inString) {
      if (c == '\\' && i + 1 < s.size()) {
        current += c;
        current += s[++i];
      } else if (c == stringChar) {
        current += c;
        tokens.push_back(current);
        current.clear();
        inString = false;
      } else {
        current += c;
      }
      continue;
    }

    if (c == '"' || c == '\'') {
      if (!current.empty()) {
        tokens.push_back(current);
        current.clear();
      }
      current += c;
      stringChar = c;
      inString = true;
      continue;
    }

    if (std::isspace(static_cast<unsigned char>(c))) {
      if (!current.empty()) {
        tokens.push_back(current);
        current.clear();
      }
      continue;
    }

    if (c == '(' || c == ')' || c == '[' || c == ']' || c == '{' || c == '}' ||
        c == ',' || c == ';' || c == ':' || c == '-' || c == '>' || c == '<' ||
        c == '=' || c == '!' || c == '.' || c == '@' || c == '#') {
      if (!current.empty()) {
        tokens.push_back(current);
        current.clear();
      }
      if (c == '-' && i + 1 < s.size() && s[i + 1] == '>') {
        tokens.push_back("->");
        i++;
      } else if (c == '<' && i + 1 < s.size() && s[i + 1] == '-') {
        tokens.push_back("<-");
        i++;
      } else {
        std::string single(1, c);
        tokens.push_back(single);
      }
      continue;
    }

    current += c;
  }

  if (!current.empty()) {
    tokens.push_back(current);
  }

  return tokens;
}

std::unique_ptr<CypherQuery>
CypherParser::parseQuery(std::vector<std::string> &tokens, size_t &pos,
                         const CypherQueryParameters &params) {
  auto query = std::make_unique<CypherQuery>();

  while (hasMore(tokens, pos)) {
    std::string token = peek(tokens, pos);
    std::string upperToken = token;
    std::transform(upperToken.begin(), upperToken.end(), upperToken.begin(),
                   ::toupper);

    if (upperToken == "MATCH") {
      consume(tokens, pos);
      // Helper lambda to get uppercased peek
      auto peekUpper = [&tokens, &pos]() -> std::string {
        if (pos < tokens.size()) {
          std::string t = tokens[pos];
          std::transform(t.begin(), t.end(), t.begin(), ::toupper);
          return t;
        }
        return "";
      };
      while (hasMore(tokens, pos) && peekUpper() != "WHERE" &&
             peekUpper() != "RETURN" && peekUpper() != "WITH" &&
             peekUpper() != ";") {
        auto pattern = parsePattern(tokens, pos);
        if (!pattern) {
          return nullptr;
        }
        query->addPattern(std::move(pattern));
        // Handle comma-separated patterns
        if (hasMore(tokens, pos) && peek(tokens, pos) == ",") {
          consume(tokens, pos);
        }
      }
    } else if (upperToken == "WHERE") {
      consume(tokens, pos);
      auto whereClause = parseWhereClause(tokens, pos);
      if (!whereClause) {
        return nullptr;
      }
      query->setWhereClause(std::move(whereClause));
    } else if (upperToken == "RETURN") {
      consume(tokens, pos);
      // Helper lambda to get uppercased peek
      auto peekUpper = [&tokens, &pos]() -> std::string {
        if (pos < tokens.size()) {
          std::string t = tokens[pos];
          std::transform(t.begin(), t.end(), t.begin(), ::toupper);
          return t;
        }
        return "";
      };
      while (hasMore(tokens, pos) && peekUpper() != "ORDER" &&
             peekUpper() != "LIMIT" && peekUpper() != ";") {
        auto item = parseReturnItem(tokens, pos);
        if (!item) {
          return nullptr;
        }
        query->addReturnItem(std::move(item));
        if (hasMore(tokens, pos) && peek(tokens, pos) == ",") {
          consume(tokens, pos);
        }
      }
    } else if (upperToken == "ORDER") {
      consume(tokens, pos);
      if (hasMore(tokens, pos) &&
          (peek(tokens, pos) == "BY" || peek(tokens, pos) == "by")) {
        consume(tokens, pos);
        auto orderBy = parseOrderBy(tokens, pos);
        if (orderBy) {
          query->setOrderBy(std::move(orderBy));
        }
      } else {
        setError(CypherErrorCode::SYNTAX_ERROR, "Expected BY after ORDER", 0,
                 0);
        return nullptr;
      }
    } else if (upperToken == "LIMIT") {
      consume(tokens, pos);
      if (hasMore(tokens, pos)) {
        std::string limitStr = consume(tokens, pos);
        try {
          int limit = std::stoi(limitStr);
          query->setLimit(limit);
        } catch (...) {
          setError(CypherErrorCode::SYNTAX_ERROR,
                   "Invalid LIMIT value: " + limitStr, 0, 0);
          return nullptr;
        }
      } else {
        setError(CypherErrorCode::SYNTAX_ERROR, "Expected value after LIMIT", 0,
                 0);
        return nullptr;
      }
    } else if (upperToken == "WITH") {
      consume(tokens, pos);
      // Helper lambda to get uppercased peek
      auto peekUpper = [&tokens, &pos]() -> std::string {
        if (pos < tokens.size()) {
          std::string t = tokens[pos];
          std::transform(t.begin(), t.end(), t.begin(), ::toupper);
          return t;
        }
        return "";
      };
      while (hasMore(tokens, pos) && peekUpper() != "MATCH" &&
             peekUpper() != "RETURN" && peekUpper() != ";") {
        auto item = parseReturnItem(tokens, pos);
        if (item) {
          query->addWithItem(std::move(item));
        }
        if (hasMore(tokens, pos) && peek(tokens, pos) == ",") {
          consume(tokens, pos);
        }
      }
    } else if (token == ";") {
      break;
    } else {
      setError(CypherErrorCode::SYNTAX_ERROR, "Unexpected token: " + token, 0,
               0);
      return nullptr;
    }
  }

  if (query->getPatterns().empty()) {
    setError(CypherErrorCode::SYNTAX_ERROR, "MATCH clause is required", 0, 0);
    return nullptr;
  }

  return query;
}

std::unique_ptr<CypherPatternElement>
CypherParser::parsePattern(std::vector<std::string> &tokens, size_t &pos) {
  if (!hasMore(tokens, pos)) {
    setError(CypherErrorCode::SYNTAX_ERROR, "Expected pattern", 0, 0);
    return nullptr;
  }

  auto pattern =
      std::make_unique<CypherPatternElement>(parseNodePattern(tokens, pos));

  if (!pattern->getStartNode()) {
    return nullptr;
  }

  if (hasMore(tokens, pos)) {
    std::string token = peek(tokens, pos);
    if (token == "[" || token == "-" || token == "<-") {
      auto rel = parseRelationshipPattern(tokens, pos);
      if (!rel) {
        return nullptr;
      }
      pattern->setRelationship(std::move(rel));
    }
  }

  if (hasMore(tokens, pos) && peek(tokens, pos) == "(") {
    pattern->setEndNode(parseNodePattern(tokens, pos));
  }

  return pattern;
}

std::unique_ptr<CypherNodePattern>
CypherParser::parseNodePattern(std::vector<std::string> &tokens, size_t &pos) {
  if (!hasMore(tokens, pos)) {
    setError(CypherErrorCode::SYNTAX_ERROR, "Expected '(' for node pattern", 0,
             0);
    return nullptr;
  }

  if (peek(tokens, pos) != "(") {
    setError(CypherErrorCode::SYNTAX_ERROR,
             "Expected '(' but found: " + peek(tokens, pos), 0, 0);
    return nullptr;
  }
  consume(tokens, pos);

  std::string variable;
  std::string label;
  std::unordered_map<std::string, std::string> properties;

  if (hasMore(tokens, pos)) {
    std::string next = peek(tokens, pos);
    if (next != ":" && next != ")" && next != "{") {
      variable = consume(tokens, pos);
      if (variable.empty()) {
        return nullptr;
      }
    }
  }

  if (hasMore(tokens, pos) && peek(tokens, pos) == ":") {
    consume(tokens, pos);
    if (hasMore(tokens, pos)) {
      label = consume(tokens, pos);
      if (label.empty()) {
        setError(CypherErrorCode::SYNTAX_ERROR, "Expected label after ':'", 0,
                 0);
        return nullptr;
      }
    }
  }

  if (hasMore(tokens, pos) && peek(tokens, pos) == "{") {
    consume(tokens, pos);
    while (hasMore(tokens, pos) && peek(tokens, pos) != "}") {
      std::string key = consume(tokens, pos);
      std::string value;
      if (hasMore(tokens, pos) && peek(tokens, pos) == ":") {
        consume(tokens, pos);
        value = consume(tokens, pos);
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
          value = value.substr(1, value.size() - 2);
        }
        properties[key] = value;
      }
      if (hasMore(tokens, pos) && peek(tokens, pos) == ",") {
        consume(tokens, pos);
      }
    }
    if (hasMore(tokens, pos) && peek(tokens, pos) == "}") {
      consume(tokens, pos);
    }
  }

  if (!hasMore(tokens, pos) || peek(tokens, pos) != ")") {
    setError(CypherErrorCode::SYNTAX_ERROR,
             "Expected ')' to close node pattern", 0, 0);
    return nullptr;
  }
  consume(tokens, pos);

  auto pat = std::make_unique<CypherNodePattern>(variable, label);
  for (const auto &kv : properties) {
    pat->addProperty(kv.first, kv.second);
  }
  return pat;
}

std::unique_ptr<CypherRelationshipPattern>
CypherParser::parseRelationshipPattern(std::vector<std::string> &tokens,
                                       size_t &pos) {
  bool bidirectional = false;

  // Handle leading direction
  if (hasMore(tokens, pos)) {
    std::string token = peek(tokens, pos);
    if (token == "<-") {
      bidirectional = true;
      consume(tokens, pos);
    } else if (token == "-") {
      consume(tokens, pos);
    }
  }

  // Check for relationship pattern in brackets
  if (hasMore(tokens, pos) && peek(tokens, pos) == "[") {
    consume(tokens, pos); // consume '['

    std::string variable;
    std::string type;
    int minHops = 1;
    int maxHops = 1;
    std::unordered_map<std::string, std::string> properties;

    // Check for variable-length pattern [*] or [*1..5]
    if (hasMore(tokens, pos) && peek(tokens, pos) == "*") {
      consume(tokens, pos); // consume '*'
      minHops = 1;
      maxHops = -1; // -1 means unlimited

      // Standard Cypher-style range: *min..max (both optional)
      if (hasMore(tokens, pos)) {
        const std::string &t0 = peek(tokens, pos);
        if (!t0.empty() && std::isdigit(static_cast<unsigned char>(t0[0]))) {
          std::string minStr = consume(tokens, pos);
          try {
            minHops = std::stoi(minStr);
          } catch (...) {
            minHops = 1;
          }
        }

        if (hasMore(tokens, pos) && peek(tokens, pos) == ".") {
          consume(tokens, pos); // '.'
          if (hasMore(tokens, pos) && peek(tokens, pos) == ".") {
            consume(tokens, pos); // '.'
            if (hasMore(tokens, pos)) {
              const std::string &t1 = peek(tokens, pos);
              if (!t1.empty() &&
                  std::isdigit(static_cast<unsigned char>(t1[0]))) {
                std::string maxStr = consume(tokens, pos);
                try {
                  maxHops = std::stoi(maxStr);
                } catch (...) {
                  maxHops = -1;
                }
              } else {
                maxHops = -1;
              }
            }
          }
        } else {
          // "*N" means exactly N hops.
          if (maxHops == -1 && minHops > 1)
            maxHops = minHops;
        }
      }

      if (hasMore(tokens, pos) && peek(tokens, pos) == "]") {
        consume(tokens, pos); // consume ']'

        // Handle trailing direction
        if (hasMore(tokens, pos)) {
          std::string next = peek(tokens, pos);
          if (next == "<-") {
            consume(tokens, pos);
            bidirectional = true;
          } else if (next == "->") {
            consume(tokens, pos);
          } else if (next == "-") {
            consume(tokens, pos);
            if (hasMore(tokens, pos) && peek(tokens, pos) == ">") {
              consume(tokens, pos);
            }
          }
        }

        auto rel = std::make_unique<CypherRelationshipPattern>(variable, type,
                                                               bidirectional);
        rel->setMinHops(minHops);
        rel->setMaxHops(maxHops);
        return rel;
      }
    }

    if (hasMore(tokens, pos)) {
      std::string next = peek(tokens, pos);
      if (next != ":" && next != "]") {
        variable = consume(tokens, pos);
      }
    }

    if (hasMore(tokens, pos) && peek(tokens, pos) == ":") {
      consume(tokens, pos);
      if (hasMore(tokens, pos) && peek(tokens, pos) != "]") {
        type = consume(tokens, pos);
      }
    }

    // Handle properties in braces
    if (hasMore(tokens, pos) && peek(tokens, pos) == "{") {
      consume(tokens, pos);
      while (hasMore(tokens, pos) && peek(tokens, pos) != "}") {
        std::string key = consume(tokens, pos);
        if (hasMore(tokens, pos) && peek(tokens, pos) == ":") {
          consume(tokens, pos);
          std::string value = consume(tokens, pos);
          if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.size() - 2);
          }
          properties[key] = value;
        }
        if (hasMore(tokens, pos) && peek(tokens, pos) == ",") {
          consume(tokens, pos);
        }
      }
      if (hasMore(tokens, pos) && peek(tokens, pos) == "}") {
        consume(tokens, pos);
      }
    }

    if (!hasMore(tokens, pos) || peek(tokens, pos) != "]") {
      setError(CypherErrorCode::SYNTAX_ERROR,
               "Expected ']' to close relationship pattern", 0, 0);
      return nullptr;
    }
    consume(tokens, pos); // consume ']'

    // Handle trailing direction
    if (hasMore(tokens, pos)) {
      std::string next = peek(tokens, pos);
      if (next == "<-") {
        consume(tokens, pos);
        bidirectional = true;
      } else if (next == "->") {
        consume(tokens, pos);
      } else if (next == "-") {
        consume(tokens, pos);
        if (hasMore(tokens, pos) && peek(tokens, pos) == ">") {
          consume(tokens, pos);
        }
      }
    }

    auto rel = std::make_unique<CypherRelationshipPattern>(variable, type,
                                                           bidirectional);
    rel->setMinHops(1);
    rel->setMaxHops(1);
    for (const auto &kv : properties) {
      rel->addProperty(kv.first, kv.second);
    }
    return rel;
  }

  // No bracket pattern, just direction
  if (hasMore(tokens, pos)) {
    std::string token = peek(tokens, pos);
    if (token == "<-") {
      consume(tokens, pos);
      bidirectional = true;
    } else if (token == "->") {
      consume(tokens, pos);
    } else if (token == "-") {
      consume(tokens, pos);
      if (hasMore(tokens, pos) && peek(tokens, pos) == ">") {
        consume(tokens, pos);
      }
    }
  }

  return std::make_unique<CypherRelationshipPattern>("", "", bidirectional);
}

std::unique_ptr<CypherWhereClause>
CypherParser::parseWhereClause(std::vector<std::string> &tokens, size_t &pos) {
  return parseBooleanExpression(tokens, pos);
}

std::unique_ptr<CypherWhereClause>
CypherParser::parseBooleanExpression(std::vector<std::string> &tokens,
                                     size_t &pos) {
  if (!hasMore(tokens, pos)) {
    setError(CypherErrorCode::SYNTAX_ERROR,
             "Expected expression in WHERE clause", 0, 0);
    return nullptr;
  }

  std::string token = peek(tokens, pos);

  if (token == "NOT" || token == "not") {
    consume(tokens, pos);
    auto expr = parseBooleanExpression(tokens, pos);
    return CypherWhereClause::makeNot(std::move(expr));
  }

  if (token == "(") {
    consume(tokens, pos);
    auto left = parseBooleanExpression(tokens, pos);
    if (!left)
      return nullptr;

    if (hasMore(tokens, pos)) {
      std::string op = peek(tokens, pos);
      if (op == "AND" || op == "and" || op == "OR" || op == "or") {
        consume(tokens, pos);
        auto right = parseBooleanExpression(tokens, pos);
        if (!right)
          return nullptr;

        if (op == "AND" || op == "and") {
          return CypherWhereClause::makeAnd(std::move(left), std::move(right));
        } else {
          return CypherWhereClause::makeOr(std::move(left), std::move(right));
        }
      }
    }

    if (hasMore(tokens, pos) && peek(tokens, pos) == ")") {
      consume(tokens, pos);
    }
    return left;
  }

  return parseComparison(tokens, pos);
}

std::unique_ptr<CypherWhereClause>
CypherParser::parseComparison(std::vector<std::string> &tokens, size_t &pos) {
  if (!hasMore(tokens, pos)) {
    setError(CypherErrorCode::SYNTAX_ERROR, "Expected comparison expression", 0,
             0);
    return nullptr;
  }

  std::string variable = consume(tokens, pos);
  std::string property;

  if (hasMore(tokens, pos) && peek(tokens, pos) == ".") {
    consume(tokens, pos);
    if (hasMore(tokens, pos)) {
      property = consume(tokens, pos);
    }
  }

  if (!hasMore(tokens, pos)) {
    return CypherWhereClause::makeExists(variable, property);
  }

  std::string op = consume(tokens, pos);
  std::string upperOp = op;
  std::transform(upperOp.begin(), upperOp.end(), upperOp.begin(), ::toupper);

  // Handle "IS NULL" / "IS NOT NULL"
  if (upperOp == "IS") {
    if (!hasMore(tokens, pos)) {
      setError(CypherErrorCode::SYNTAX_ERROR, "Expected NULL after IS", 0, 0);
      return nullptr;
    }
    std::string t1 = consume(tokens, pos);
    std::string upperT1 = t1;
    std::transform(upperT1.begin(), upperT1.end(), upperT1.begin(), ::toupper);

    if (upperT1 == "NOT") {
      if (!hasMore(tokens, pos)) {
        setError(CypherErrorCode::SYNTAX_ERROR, "Expected NULL after IS NOT", 0,
                 0);
        return nullptr;
      }
      std::string t2 = consume(tokens, pos);
      std::string upperT2 = t2;
      std::transform(upperT2.begin(), upperT2.end(), upperT2.begin(),
                     ::toupper);
      if (upperT2 != "NULL") {
        setError(CypherErrorCode::SYNTAX_ERROR, "Expected NULL after IS NOT", 0,
                 0);
        return nullptr;
      }
      return CypherWhereClause::makeComparison(
          variable, property, CypherComparisonOp::IS_NOT_NULL, "");
    }

    if (upperT1 != "NULL") {
      setError(CypherErrorCode::SYNTAX_ERROR, "Expected NULL after IS", 0, 0);
      return nullptr;
    }
    return CypherWhereClause::makeComparison(variable, property,
                                             CypherComparisonOp::IS_NULL, "");
  }

  // Handle "STARTS WITH" / "ENDS WITH"
  if (upperOp == "STARTS" || upperOp == "ENDS") {
    if (!hasMore(tokens, pos)) {
      setError(CypherErrorCode::SYNTAX_ERROR,
               "Expected WITH after " + op, 0, 0);
      return nullptr;
    }
    std::string maybeWith = consume(tokens, pos);
    std::string upperWith = maybeWith;
    std::transform(upperWith.begin(), upperWith.end(), upperWith.begin(),
                   ::toupper);
    if (upperWith != "WITH") {
      setError(CypherErrorCode::SYNTAX_ERROR,
               "Expected WITH after " + op, 0, 0);
      return nullptr;
    }

    std::string value;
    if (hasMore(tokens, pos)) {
      value = consume(tokens, pos);
      if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value = value.substr(1, value.size() - 2);
      }
    }

    return CypherWhereClause::makeComparison(
        variable, property,
        (upperOp == "STARTS") ? CypherComparisonOp::STARTS_WITH
                              : CypherComparisonOp::ENDS_WITH,
        value);
  }

  std::string value;
  if (hasMore(tokens, pos)) {
    value = consume(tokens, pos);
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
      value = value.substr(1, value.size() - 2);
    }
  }

  CypherComparisonOp comparisonOp = CypherComparisonOp::EQUALS;

  if (upperOp == "=" || upperOp == "==") {
    comparisonOp = CypherComparisonOp::EQUALS;
  } else if (upperOp == "!=" || upperOp == "<>") {
    comparisonOp = CypherComparisonOp::NOT_EQUALS;
  } else if (upperOp == "<") {
    comparisonOp = CypherComparisonOp::LESS_THAN;
  } else if (upperOp == "<=") {
    comparisonOp = CypherComparisonOp::LESS_THAN_OR_EQUAL;
  } else if (upperOp == ">") {
    comparisonOp = CypherComparisonOp::GREATER_THAN;
  } else if (upperOp == ">=") {
    comparisonOp = CypherComparisonOp::GREATER_THAN_OR_EQUAL;
  } else if (upperOp == "CONTAINS") {
    comparisonOp = CypherComparisonOp::CONTAINS;
  }

  return CypherWhereClause::makeComparison(variable, property, comparisonOp,
                                           value);
}

std::unique_ptr<CypherReturnItem>
CypherParser::parseReturnItem(std::vector<std::string> &tokens, size_t &pos) {
  if (!hasMore(tokens, pos)) {
    setError(CypherErrorCode::SYNTAX_ERROR, "Expected return item", 0, 0);
    return nullptr;
  }

  std::string variable = consume(tokens, pos);
  std::string alias;

  // Handle property access (e.g., n.id -> consume . and property)
  if (hasMore(tokens, pos) && peek(tokens, pos) == ".") {
    consume(tokens, pos); // consume '.'
    if (hasMore(tokens, pos)) {
      std::string prop = consume(tokens, pos);
      variable += "." + prop; // Combine into single variable
    }
  }

  if (hasMore(tokens, pos) &&
      (peek(tokens, pos) == "AS" || peek(tokens, pos) == "as")) {
    consume(tokens, pos);
    if (hasMore(tokens, pos)) {
      alias = consume(tokens, pos);
    }
  }

  return std::make_unique<CypherReturnItem>(variable, alias);
}

std::unique_ptr<CypherOrderBy>
CypherParser::parseOrderBy(std::vector<std::string> &tokens, size_t &pos) {
  if (!hasMore(tokens, pos)) {
    setError(CypherErrorCode::SYNTAX_ERROR, "Expected variable for ORDER BY", 0,
             0);
    return nullptr;
  }

  std::string variable = consume(tokens, pos);

  // Handle property access (e.g., n.id -> consume . and property)
  if (hasMore(tokens, pos) && peek(tokens, pos) == ".") {
    consume(tokens, pos); // consume '.'
    if (hasMore(tokens, pos)) {
      consume(tokens, pos); // consume property name
    }
  }

  CypherOrderBy::Direction dir = CypherOrderBy::Direction::ASC;

  if (hasMore(tokens, pos)) {
    std::string next = peek(tokens, pos);
    std::string upperNext = next;
    std::transform(upperNext.begin(), upperNext.end(), upperNext.begin(),
                   ::toupper);
    if (upperNext == "DESC") {
      dir = CypherOrderBy::Direction::DESC;
      consume(tokens, pos);
    } else if (upperNext == "ASC") {
      consume(tokens, pos);
    }
  }

  return std::make_unique<CypherOrderBy>(variable, dir);
}

bool CypherParser::hasMore(const std::vector<std::string> &tokens, size_t pos) {
  return pos < tokens.size();
}

const std::string &CypherParser::peek(const std::vector<std::string> &tokens,
                                      size_t pos) {
  static const std::string empty = "";
  if (pos < tokens.size()) {
    return tokens[pos];
  }
  return empty;
}

const std::string &CypherParser::consume(std::vector<std::string> &tokens,
                                         size_t &pos) {
  static const std::string empty = "";
  if (pos < tokens.size()) {
    return tokens[pos++];
  }
  return empty;
}

// ============================================================================
// CypherResult implementation
// ============================================================================

std::string CypherResult::toString() const {
  std::ostringstream oss;

  switch (type_) {
  case ResultType::NODES:
    oss << "Result(" << nodes_.size() << " nodes)";
    break;
  case ResultType::RELATIONSHIPS:
    oss << "Result(" << relationships_.size() << " relationships)";
    break;
  case ResultType::PATHS:
    oss << "Result(paths)";
    break;
  case ResultType::SCALAR:
    oss << scalarValue_;
    break;
  case ResultType::INTEGER:
    oss << integerValue_;
    break;
  case ResultType::BOOLEAN:
    oss << (booleanValue_ ? "true" : "false");
    break;
  }

  return oss.str();
}

// ============================================================================
// CypherQueryExecutor implementation
// ============================================================================

std::unique_ptr<CypherResult>
CypherQueryExecutor::execute(const CypherQuery &query) {
  CypherQueryStats stats;
  return execute(query, stats);
}

std::unique_ptr<CypherResult>
CypherQueryExecutor::execute(const CypherQuery &query,
                             CypherQueryStats &stats) {
  auto startTime = std::chrono::high_resolution_clock::now();
  lastStats_ = CypherQueryStats();

  std::vector<Node *> resultNodes;
  std::vector<Edge *> resultEdges;

  for (const auto &pattern : query.getPatterns()) {
    auto patternResult = matchPattern(pattern.get());
    if (patternResult) {
      const auto &nodes = patternResult->getNodes();
      resultNodes.insert(resultNodes.end(), nodes.begin(), nodes.end());
      const auto &edges = patternResult->getRelationships();
      resultEdges.insert(resultEdges.end(), edges.begin(), edges.end());
      lastStats_.nodesVisited += nodes.size();
      lastStats_.edgesVisited += edges.size();
    }
  }

  std::sort(resultNodes.begin(), resultNodes.end());
  resultNodes.erase(std::unique(resultNodes.begin(), resultNodes.end()),
                    resultNodes.end());
  std::sort(resultEdges.begin(), resultEdges.end());
  resultEdges.erase(std::unique(resultEdges.begin(), resultEdges.end()),
                    resultEdges.end());

  if (query.getWhereClause() && !resultNodes.empty()) {
    auto filtered = filterByWhere(resultNodes, *query.getWhereClause());
    resultNodes = filtered->getNodes();
  }

  if (query.hasLimit() && query.getLimit() > 0 &&
      resultNodes.size() > static_cast<size_t>(query.getLimit())) {
    resultNodes.resize(static_cast<size_t>(query.getLimit()));
  }

  auto result = std::make_unique<CypherResult>(CypherResult::ResultType::NODES);
  for (auto *node : resultNodes) {
    result->addNode(node);
  }
  for (auto *edge : resultEdges) {
    result->addEdge(edge);
  }

  lastStats_.resultsReturned = result->getCount();

  auto endTime = std::chrono::high_resolution_clock::now();
  lastStats_.executionTime =
      std::chrono::duration_cast<std::chrono::microseconds>(endTime -
                                                            startTime);
  stats = lastStats_;

  return result;
}

std::unique_ptr<CypherResult>
CypherQueryExecutor::matchPattern(const CypherPatternElement *pattern) {
  if (!pattern)
    return nullptr;

  auto result = std::make_unique<CypherResult>(CypherResult::ResultType::NODES);

  const auto *startNodePattern = pattern->getStartNode();
  if (!startNodePattern)
    return result;

  auto startNodes = matchNodes(startNodePattern->getLabel(), "");
  if (startNodes) {
    std::vector<Node *> startMatches = startNodes->getNodes();
    const auto &startProps = startNodePattern->getProperties();
    if (!startProps.empty()) {
      std::vector<Node *> filtered;
      filtered.reserve(startMatches.size());
      for (auto *n : startMatches) {
        bool ok = true;
        for (const auto &kv : startProps) {
          if (getNodeProperty(n, kv.first) != kv.second) {
            ok = false;
            break;
          }
        }
        if (ok)
          filtered.push_back(n);
      }
      startMatches = std::move(filtered);
    }

    if (!startNodePattern->getVariable().empty()) {
      boundVariables_[startNodePattern->getVariable()] = startMatches;
    }

    for (auto *node : startMatches) {
      result->addNode(node);

      const auto *rel = pattern->getRelationship();
      if (rel) {
        int maxHops = rel->hasVariableLength() ? rel->getMaxHops() : 1;
        if (maxHops < 0)
          maxHops = 5;
        auto traversed = traverse(node, *rel, maxHops);
        if (traversed) {
          for (auto *e : traversed->getRelationships())
            result->addEdge(e);

          if (!rel->getVariable().empty()) {
            auto &bucket = boundRelationships_[rel->getVariable()];
            const auto &edges = traversed->getRelationships();
            bucket.insert(bucket.end(), edges.begin(), edges.end());
            std::sort(bucket.begin(), bucket.end());
            bucket.erase(std::unique(bucket.begin(), bucket.end()),
                         bucket.end());
          }

          const auto *endNodePattern = pattern->getEndNode();
          std::string endLabel = endNodePattern ? endNodePattern->getLabel() : "";
          std::unordered_map<std::string, std::string> endProps =
              endNodePattern ? endNodePattern->getProperties()
                             : std::unordered_map<std::string, std::string>{};

          std::unordered_set<Node *> labelAllowed;
          if (endNodePattern && !endLabel.empty()) {
            auto labelNodes = matchNodes(endLabel, "");
            for (auto *ln : labelNodes->getNodes())
              labelAllowed.insert(ln);
          }

          std::vector<Node *> endMatches;
          for (auto *tNode : traversed->getNodes()) {
            if (endNodePattern) {
              if (!endLabel.empty() && labelAllowed.find(tNode) == labelAllowed.end())
                continue;
              bool ok = true;
              for (const auto &kv : endProps) {
                if (getNodeProperty(tNode, kv.first) != kv.second) {
                  ok = false;
                  break;
                }
              }
              if (!ok)
                continue;
              endMatches.push_back(tNode);
              result->addNode(tNode);
            } else {
              result->addNode(tNode);
            }
          }
          if (endNodePattern && !endNodePattern->getVariable().empty()) {
            auto &bucket = boundVariables_[endNodePattern->getVariable()];
            bucket.insert(bucket.end(), endMatches.begin(), endMatches.end());
            std::sort(bucket.begin(), bucket.end());
            bucket.erase(std::unique(bucket.begin(), bucket.end()), bucket.end());
          }
        }
      }
    }
  }

  return result;
}

std::unique_ptr<CypherResult>
CypherQueryExecutor::matchNodes(const std::string &label,
                                const std::string &variable) {
  auto result = std::make_unique<CypherResult>(CypherResult::ResultType::NODES);

  static const std::unordered_map<std::string, GraphNodeType> labelMap = {
      {"INST_FUNCALL", GraphNodeType::INST_FUNCALL},
      {"INST_RET", GraphNodeType::INST_RET},
      {"INST_BR", GraphNodeType::INST_BR},
      {"INST_OTHER", GraphNodeType::INST_OTHER},
      {"FUNC_ENTRY", GraphNodeType::FUNC_ENTRY},
      {"PARAM_FORMALIN", GraphNodeType::PARAM_FORMALIN},
      {"PARAM_FORMALOUT", GraphNodeType::PARAM_FORMALOUT},
      {"PARAM_ACTUALIN", GraphNodeType::PARAM_ACTUALIN},
      {"PARAM_ACTUALOUT", GraphNodeType::PARAM_ACTUALOUT},
      {"VAR_OTHER", GraphNodeType::VAR_OTHER},
      {"VAR_STATICALLOCGLOBALSCOPE", GraphNodeType::VAR_STATICALLOCGLOBALSCOPE},
      {"VAR_STATICALLOCMODULESCOPE", GraphNodeType::VAR_STATICALLOCMODULESCOPE},
      {"VAR_STATICALLOCFUNCTIONSCOPE",
       GraphNodeType::VAR_STATICALLOCFUNCTIONSCOPE},
      {"FUNC", GraphNodeType::FUNC},
      {"CLASS", GraphNodeType::CLASS},
      {"ANNO_VAR", GraphNodeType::ANNO_VAR},
      {"ANNO_GLOBAL", GraphNodeType::ANNO_GLOBAL},
      {"ANNO_OTHER", GraphNodeType::ANNO_OTHER}};

  if (label.empty()) {
    for (auto it = pdg_.begin(); it != pdg_.end(); ++it) {
      result->addNode(*it);
    }
  } else {
    const std::string upperLabel = [&]() {
      std::string t = label;
      std::transform(t.begin(), t.end(), t.begin(), ::toupper);
      return t;
    }();

    auto matchesGroup = [&](GraphNodeType t) -> bool {
      if (upperLabel == "INST") {
        return t == GraphNodeType::INST_FUNCALL || t == GraphNodeType::INST_RET ||
               t == GraphNodeType::INST_BR || t == GraphNodeType::INST_OTHER;
      }
      if (upperLabel == "VAR") {
        return t == GraphNodeType::VAR_OTHER ||
               t == GraphNodeType::VAR_STATICALLOCGLOBALSCOPE ||
               t == GraphNodeType::VAR_STATICALLOCMODULESCOPE ||
               t == GraphNodeType::VAR_STATICALLOCFUNCTIONSCOPE;
      }
      if (upperLabel == "PARAM") {
        return t == GraphNodeType::PARAM_FORMALIN ||
               t == GraphNodeType::PARAM_FORMALOUT ||
               t == GraphNodeType::PARAM_ACTUALIN ||
               t == GraphNodeType::PARAM_ACTUALOUT;
      }
      if (upperLabel == "ANNO") {
        return t == GraphNodeType::ANNO_VAR || t == GraphNodeType::ANNO_GLOBAL ||
               t == GraphNodeType::ANNO_OTHER;
      }
      return false;
    };

    auto it = labelMap.find(upperLabel);
    for (auto iter = pdg_.begin(); iter != pdg_.end(); ++iter) {
      const auto t = (*iter)->getNodeType();
      if ((it != labelMap.end() && t == it->second) || matchesGroup(t)) {
        result->addNode(*iter);
      }
    }
  }

  if (!variable.empty()) {
    boundVariables_[variable] = result->getNodes();
  }

  return result;
}

std::unique_ptr<CypherResult>
CypherQueryExecutor::matchEdges(const std::string &type,
                                const std::string &variable) {
  auto result =
      std::make_unique<CypherResult>(CypherResult::ResultType::RELATIONSHIPS);

  static const std::unordered_map<std::string, EdgeType> typeMap = {
      {"DATA_DEF_USE", EdgeType::DATA_DEF_USE},
      {"DATA_RAW", EdgeType::DATA_RAW},
      {"DATA_READ", EdgeType::DATA_READ},
      {"DATA_ALIAS", EdgeType::DATA_ALIAS},
      {"DATA_RET", EdgeType::DATA_RET},
      {"CONTROLDEP_BR", EdgeType::CONTROLDEP_BR},
      {"CONTROLDEP_ENTRY", EdgeType::CONTROLDEP_ENTRY},
      {"CONTROLDEP_CALLINV", EdgeType::CONTROLDEP_CALLINV},
      {"CONTROLDEP_CALLRET", EdgeType::CONTROLDEP_CALLRET},
      {"IND_CALL", EdgeType::IND_CALL},
      {"PARAMETER_IN", EdgeType::PARAMETER_IN},
      {"PARAMETER_OUT", EdgeType::PARAMETER_OUT}};

  if (type.empty()) {
    for (auto it = pdg_.begin(); it != pdg_.end(); ++it) {
      for (auto *edge : (*it)->getOutEdgeSet()) {
        result->addEdge(edge);
      }
    }
  } else {
    auto it = typeMap.find(type);
    if (it != typeMap.end()) {
      for (auto iter = pdg_.begin(); iter != pdg_.end(); ++iter) {
        for (auto *edge : (*iter)->getOutEdgeSet()) {
          if (edge->getEdgeType() == it->second) {
            result->addEdge(edge);
          }
        }
      }
    }
  }

  if (!variable.empty()) {
    boundRelationships_[variable] = result->getRelationships();
  }

  return result;
}

std::unique_ptr<CypherResult>
CypherQueryExecutor::traverse(Node *start, const CypherRelationshipPattern &rel,
                              int maxHops) {
  auto result = std::make_unique<CypherResult>(CypherResult::ResultType::NODES);

  if (!start)
    return result;

  static const std::unordered_map<std::string, EdgeType> typeMap = {
      {"DATA_DEP", EdgeType::DATA_DEF_USE},
      {"CONTROL_DEP", EdgeType::CONTROLDEP_BR},
      {"CALL", EdgeType::CONTROLDEP_CALLINV},
      {"CALL_INV", EdgeType::CONTROLDEP_CALLINV},
      {"CALL_RET", EdgeType::CONTROLDEP_CALLRET},
      {"IND_CALL", EdgeType::IND_CALL},
      {"PARAM_IN", EdgeType::PARAMETER_IN},
      {"PARAM_OUT", EdgeType::PARAMETER_OUT},
      {"DATA_RAW", EdgeType::DATA_RAW},
      {"DATA_READ", EdgeType::DATA_READ},
      {"DATA_ALIAS", EdgeType::DATA_ALIAS}};

  std::vector<Node *> currentLevel = {start};
  std::unordered_set<Node *> visited;
  std::unordered_set<Edge *> visitedEdges;
  visited.insert(start);

  EdgeType edgeType = EdgeType::DATA_DEF_USE;
  if (!rel.getType().empty()) {
    auto it = typeMap.find(rel.getType());
    if (it != typeMap.end()) {
      edgeType = it->second;
    }
  }

  for (int hop = 0; hop < maxHops && !currentLevel.empty(); ++hop) {
    std::vector<Node *> nextLevel;

    for (auto *node : currentLevel) {
      for (auto *edge : node->getOutEdgeSet()) {
        if (edge->getEdgeType() == edgeType || rel.getType().empty()) {
          if (visitedEdges.insert(edge).second)
            result->addEdge(edge);
          auto *neighbor = edge->getDstNode();
          if (visited.find(neighbor) == visited.end()) {
            visited.insert(neighbor);
            result->addNode(neighbor);
            nextLevel.push_back(neighbor);
          }
        }
      }

      if (rel.isBidirectional()) {
        for (auto *edge : node->getInEdgeSet()) {
          if (edge->getEdgeType() == edgeType || rel.getType().empty()) {
            if (visitedEdges.insert(edge).second)
              result->addEdge(edge);
            auto *neighbor = edge->getSrcNode();
            if (visited.find(neighbor) == visited.end()) {
              visited.insert(neighbor);
              result->addNode(neighbor);
              nextLevel.push_back(neighbor);
            }
          }
        }
      }
    }

    currentLevel = std::move(nextLevel);
  }

  return result;
}

std::unique_ptr<CypherResult>
CypherQueryExecutor::filterByWhere(const std::vector<Node *> &nodes,
                                   const CypherWhereClause &where) {
  auto result = std::make_unique<CypherResult>(CypherResult::ResultType::NODES);

  for (auto *node : nodes) {
    if (evaluateCondition(where, node)) {
      result->addNode(node);
    }
  }

  return result;
}

std::unique_ptr<CypherResult>
CypherQueryExecutor::filterByWhere(const std::vector<Edge *> &edges,
                                   const CypherWhereClause &where) {
  auto result =
      std::make_unique<CypherResult>(CypherResult::ResultType::RELATIONSHIPS);

  for (auto *edge : edges) {
    if (evaluateCondition(where, edge)) {
      result->addEdge(edge);
    }
  }

  return result;
}

bool CypherQueryExecutor::evaluateCondition(const CypherWhereClause &condition,
                                            Node *node) {
  if (condition.isBooleanOp()) {
    if (condition.getBoolOp() == "NOT") {
      const auto *child = condition.getChild();
      return child && !evaluateCondition(*child, node);
    } else {
      const auto *left = condition.getLeft();
      const auto *right = condition.getRight();
      const std::string &op = condition.getBoolOp();

      bool leftResult = left ? evaluateCondition(*left, node) : true;
      bool rightResult = right ? evaluateCondition(*right, node) : true;

      if (op == "AND") {
        return leftResult && rightResult;
      } else if (op == "OR") {
        return leftResult || rightResult;
      }
    }
  }

  if (condition.isExists()) {
    if (condition.getProperty().empty())
      return true; // Variable existence (row semantics) not modeled yet.
    return !getNodeProperty(node, condition.getProperty()).empty();
  }

  const auto &property = condition.getProperty();
  const auto &value = condition.getValue();
  std::string nodeValue = getNodeProperty(node, property);

  return applyComparison(nodeValue, condition.getComparisonOp(), value);
}

bool CypherQueryExecutor::evaluateCondition(const CypherWhereClause &condition,
                                            Edge *edge) {
  if (condition.isBooleanOp()) {
    if (condition.getBoolOp() == "NOT") {
      const auto *child = condition.getChild();
      return child && !evaluateCondition(*child, edge);
    } else {
      const auto *left = condition.getLeft();
      const auto *right = condition.getRight();
      const std::string &op = condition.getBoolOp();

      bool leftResult = left ? evaluateCondition(*left, edge) : true;
      bool rightResult = right ? evaluateCondition(*right, edge) : true;

      if (op == "AND") {
        return leftResult && rightResult;
      } else if (op == "OR") {
        return leftResult || rightResult;
      }
    }
  }

  if (condition.isExists()) {
    if (condition.getProperty().empty())
      return true;
    return !getEdgeProperty(edge, condition.getProperty()).empty();
  }

  const auto &property = condition.getProperty();
  const auto &value = condition.getValue();
  std::string edgeValue = getEdgeProperty(edge, property);

  return applyComparison(edgeValue, condition.getComparisonOp(), value);
}

bool CypherQueryExecutor::applyComparison(const std::string &nodeValue,
                                          CypherComparisonOp op,
                                          const std::string &queryValue) {
  switch (op) {
  case CypherComparisonOp::EQUALS:
    return nodeValue == queryValue;
  case CypherComparisonOp::NOT_EQUALS:
    return nodeValue != queryValue;
  case CypherComparisonOp::LESS_THAN:
    try {
      return std::stoll(nodeValue) < std::stoll(queryValue);
    } catch (...) {
      return nodeValue < queryValue;
    }
  case CypherComparisonOp::LESS_THAN_OR_EQUAL:
    try {
      return std::stoll(nodeValue) <= std::stoll(queryValue);
    } catch (...) {
      return nodeValue <= queryValue;
    }
  case CypherComparisonOp::GREATER_THAN:
    try {
      return std::stoll(nodeValue) > std::stoll(queryValue);
    } catch (...) {
      return nodeValue > queryValue;
    }
  case CypherComparisonOp::GREATER_THAN_OR_EQUAL:
    try {
      return std::stoll(nodeValue) >= std::stoll(queryValue);
    } catch (...) {
      return nodeValue >= queryValue;
    }
  case CypherComparisonOp::IS_NULL:
    return nodeValue.empty();
  case CypherComparisonOp::IS_NOT_NULL:
    return !nodeValue.empty();
  case CypherComparisonOp::CONTAINS:
    return nodeValue.find(queryValue) != std::string::npos;
  case CypherComparisonOp::STARTS_WITH:
    return nodeValue.find(queryValue) == 0;
  case CypherComparisonOp::ENDS_WITH:
    return nodeValue.size() >= queryValue.size() &&
           nodeValue.substr(nodeValue.size() - queryValue.size()) == queryValue;
  case CypherComparisonOp::IN:
    return nodeValue == queryValue;
  }
  return true;
}

std::string CypherQueryExecutor::getNodeProperty(Node *node,
                                                 const std::string &property) {
  if (!node)
    return "";

  const std::string prop = toLower(property);
  if (prop.empty())
    return "";

  if (prop == "type" || prop == "type_id" || prop == "node_type" ||
      prop == "node_type_id") {
    return std::to_string(static_cast<int>(node->getNodeType()));
  }

  if (prop == "label" || prop == "kind") {
    return graphNodeTypeName(node->getNodeType());
  }

  if (prop == "func" || prop == "function") {
    if (auto *func = node->getFunc())
      return func->getName().str();
    if (auto *v = node->getValue())
      if (auto *f = llvm::dyn_cast<llvm::Function>(v))
        return f->getName().str();
    return "";
  }

  if (prop == "name") {
    if (auto *v = node->getValue()) {
      if (v->hasName())
        return v->getName().str();
      if (auto *i = llvm::dyn_cast<llvm::Instruction>(v)) {
        if (i->hasName())
          return i->getName().str();
      }
    }
    return "";
  }

  if (prop == "opcode") {
    if (auto *v = node->getValue()) {
      if (auto *i = llvm::dyn_cast<llvm::Instruction>(v)) {
        return i->getOpcodeName();
      }
    }
    return "";
  }

  if (prop == "callee") {
    if (auto *v = node->getValue()) {
      if (auto *cb = llvm::dyn_cast<llvm::CallBase>(v)) {
        if (auto *f = cb->getCalledFunction()) {
          return f->getName().str();
        }
        return "<indirect>";
      }
    }
    return "";
  }

  if (prop == "src_file" || prop == "source_file") {
    if (auto *v = node->getValue()) {
      if (auto *i = llvm::dyn_cast<llvm::Instruction>(v)) {
        if (const llvm::DebugLoc &dl = i->getDebugLoc()) {
          auto *loc = dl.get();
          if (!loc)
            return "";
          return loc->getFilename().str();
        }
      }
    }
    return "";
  }

  if (prop == "src_line" || prop == "source_line") {
    if (auto *v = node->getValue()) {
      if (auto *i = llvm::dyn_cast<llvm::Instruction>(v)) {
        if (const llvm::DebugLoc &dl = i->getDebugLoc()) {
          auto *loc = dl.get();
          if (!loc)
            return "";
          return std::to_string(loc->getLine());
        }
      }
    }
    return "";
  }

  if (prop == "src_col" || prop == "source_col" || prop == "source_column") {
    if (auto *v = node->getValue()) {
      if (auto *i = llvm::dyn_cast<llvm::Instruction>(v)) {
        if (const llvm::DebugLoc &dl = i->getDebugLoc()) {
          auto *loc = dl.get();
          if (!loc)
            return "";
          return std::to_string(loc->getColumn());
        }
      }
    }
    return "";
  }

  if (prop == "src" || prop == "source") {
    const std::string file = getNodeProperty(node, "src_file");
    const std::string line = getNodeProperty(node, "src_line");
    const std::string col = getNodeProperty(node, "src_col");
    if (file.empty() && line.empty() && col.empty())
      return "";
    std::ostringstream oss;
    oss << file;
    if (!line.empty())
      oss << ":" << line;
    if (!col.empty())
      oss << ":" << col;
    return oss.str();
  }

  if (prop == "di_type" || prop == "dtype" || prop == "type_name") {
    if (auto *dt = node->getDIType()) {
      return dbgutils::getSourceLevelTypeName(*dt);
    }
    return "";
  }

  if (prop == "llvm" || prop == "ir") {
    if (auto *v = node->getValue()) {
      std::string s;
      llvm::raw_string_ostream os(s);
      v->print(os);
      os.flush();
      return s;
    }
    return "";
  }

  return "";
}

std::string CypherQueryExecutor::getEdgeProperty(Edge *edge,
                                                 const std::string &property) {
  if (!edge)
    return "";

  const std::string prop = toLower(property);
  if (prop == "type" || prop == "type_id" || prop == "edge_type" ||
      prop == "edge_type_id") {
    return std::to_string(static_cast<int>(edge->getEdgeType()));
  }
  if (prop == "label" || prop == "kind") {
    return edgeTypeName(edge->getEdgeType());
  }

  if (prop == "src") {
    return getNodeProperty(edge->getSrcNode(), "label");
  }
  if (prop == "dst") {
    return getNodeProperty(edge->getDstNode(), "label");
  }

  if (prop.rfind("src_", 0) == 0) {
    return getNodeProperty(edge->getSrcNode(), prop.substr(4));
  }
  if (prop.rfind("dst_", 0) == 0) {
    return getNodeProperty(edge->getDstNode(), prop.substr(4));
  }

  return "";
}

} // namespace pdg
