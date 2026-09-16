#include "CFL/DynamicDyck/Graph.h"

#include <algorithm>
#include <charconv>
#include <istream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace lotus::cfl::dynamic_dyck {
namespace {

std::string_view trim(std::string_view text) {
  const auto first = text.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos)
    return {};
  const auto last = text.find_last_not_of(" \t\r\n");
  return text.substr(first, last - first + 1);
}

std::string record(std::string_view text) {
  const auto comment = std::min(text.find('#'), text.find("//"));
  return std::string(trim(text.substr(0, comment)));
}

template <typename T> T number(std::string_view text) {
  text = trim(text);
  if (text.size() >= 2 && text.front() == '"' && text.back() == '"')
    text = text.substr(1, text.size() - 2);
  T result = 0;
  const auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), result);
  if (text.empty() || parsed.ec != std::errc{} ||
      parsed.ptr != text.data() + text.size())
    throw std::invalid_argument("invalid integer '" + std::string(text) + "'");
  return result;
}

Edge labeledEdge(std::string_view source, std::string_view target,
                 std::string_view label) {
  label = trim(label);
  if (label.size() < 5 ||
      (label.substr(0, 4) != "op--" && label.substr(0, 4) != "cp--"))
    throw std::invalid_argument("expected op--N or cp--N label");
  return {number<Vertex>(source), number<Vertex>(target),
          number<Label>(label.substr(4)),
          label.substr(0, 2) == "op" ? Parenthesis::Open : Parenthesis::Close};
}

template <typename Function>
void readRecords(std::istream &input, const char *format, Function parse) {
  std::string line;
  std::size_t line_number = 0;
  while (std::getline(input, line)) {
    ++line_number;
    const auto text = record(line);
    if (text.empty())
      continue;
    try {
      parse(text);
    } catch (const std::invalid_argument &error) {
      throw std::invalid_argument(std::string(format) + " line " +
                                  std::to_string(line_number) + ": " +
                                  error.what());
    }
  }
  if (input.bad() || (input.fail() && !input.eof()))
    throw std::runtime_error(std::string("failed reading ") + format);
}

} // namespace

Edge Edge::opening() const {
  switch (kind) {
  case Parenthesis::Open:
    return *this;
  case Parenthesis::Close:
    return {target, source, label, Parenthesis::Open};
  }
  throw std::invalid_argument("invalid dynamic-Dyck parenthesis kind");
}

Graph parseDot(std::istream &input) {
  static const std::regex edge_pattern(
      R"re(^\s*("?-?[0-9]+"?)\s*->\s*("?-?[0-9]+"?)\s*\[\s*label\s*=\s*"([^"]*)"\s*\]\s*;?\s*$)re");
  static const std::regex vertex_pattern(R"re(^\s*("?-?[0-9]+"?)\s*;?\s*$)re");
  static const std::regex wrapper_pattern(
      R"re(^digraph(\s+[A-Za-z_][A-Za-z_0-9]*)?\s*\{\s*\}?\s*;?$)re");
  Graph graph;
  std::unordered_set<Vertex> seen;
  auto addVertex = [&](Vertex vertex) {
    if (seen.insert(vertex).second)
      graph.vertices.push_back(vertex);
  };
  readRecords(input, "DOT", [&](const std::string &text) {
    std::smatch match;
    if (text == "{" || text == "}" || text == "};" ||
        std::regex_match(text, wrapper_pattern))
      return;
    if (std::regex_match(text, match, edge_pattern)) {
      const Edge edge =
          labeledEdge(match[1].str(), match[2].str(), match[3].str());
      addVertex(edge.source);
      addVertex(edge.target);
      graph.edges.push_back(edge);
    } else if (std::regex_match(text, match, vertex_pattern)) {
      addVertex(number<Vertex>(match[1].str()));
    } else {
      throw std::invalid_argument(
          "expected a numeric node or labeled edge record");
    }
  });
  return graph;
}

std::vector<Update> parseUpdates(std::istream &input) {
  std::vector<Update> result;
  readRecords(input, "update", [&](const std::string &text) {
    std::istringstream fields(text);
    std::string operation, source, target, label, extra;
    if (!(fields >> operation >> source >> target >> label) ||
        (fields >> extra))
      throw std::invalid_argument("expected A|D SOURCE TARGET op--N|cp--N");
    if (operation != "A" && operation != "D")
      throw std::invalid_argument("expected A or D operation");
    result.push_back(
        {operation == "A" ? UpdateKind::Insert : UpdateKind::Delete,
         labeledEdge(source, target, label)});
  });
  return result;
}

} // namespace lotus::cfl::dynamic_dyck
