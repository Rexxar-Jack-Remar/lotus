//===-- Verification/Sifa/Statistics/RegexStatUtils.tpp -------------------===//
//
// Template implementation for RegexStatUtils.
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_STATISTICS_REGEXSTATUTILS_TPP
#define LOTUS_VERIFICATION_SIFA_STATISTICS_REGEXSTATUTILS_TPP

#include "Verification/Sifa/Statistics/RegexStatUtils.h"

namespace lotus {
namespace sifa {

template <typename N, typename L>
lotus::pathexpressions::PathExpressionComputer<N, L>
createPEComputer(SifaStats &stats,
                 const lotus::pathexpressions::ILabeledGraph<N, L> &graph) {
  stats.start(SifaStats::Key::PATH_EXPR_TIME);
  lotus::pathexpressions::PathExpressionComputer<N, L> result(graph);
  stats.stop(SifaStats::Key::PATH_EXPR_TIME);
  return result;
}

template <typename N, typename L>
lotus::pathexpressions::RegexRef<L> exprBetween(
    SifaStats &stats, lotus::pathexpressions::PathExpressionComputer<N, L> &peComputer,
    const N &source, const N &target) {
  stats.start(SifaStats::Key::PATH_EXPR_TIME);
  auto result = peComputer.exprBetween(source, target);
  stats.stop(SifaStats::Key::PATH_EXPR_TIME);
  return result;
}

template <typename L>
RegexToDag<L> createRegexToDag(SifaStats &stats) {
  stats.start(SifaStats::Key::REGEX_TO_DAG_TIME);
  RegexToDag<L> result;
  stats.stop(SifaStats::Key::REGEX_TO_DAG_TIME);
  return result;
}

template <typename L>
RegexDagNode<L> *addToDag(SifaStats &stats, RegexToDag<L> &regexToDag,
                         const lotus::pathexpressions::RegexRef<L> &regex) {
  stats.start(SifaStats::Key::REGEX_TO_DAG_TIME);
  RegexDagNode<L> *result = regexToDag.add(regex);
  stats.stop(SifaStats::Key::REGEX_TO_DAG_TIME);
  return result;
}

template <typename L>
RegexDag<L> getDagAndReset(SifaStats &stats, RegexToDag<L> &regexToDag) {
  stats.start(SifaStats::Key::REGEX_TO_DAG_TIME);
  RegexDag<L> result = regexToDag.getDagAndReset();
  stats.stop(SifaStats::Key::REGEX_TO_DAG_TIME);
  return result;
}

template <typename L>
void compress(SifaStats &stats, RegexDag<L> &dag) {
  const std::size_t processed = dag.nodes().size();
  stats.add(SifaStats::Key::DAG_COMPRESSION_PROCESSED_NODES, processed);
  stats.start(SifaStats::Key::DAG_COMPRESSION_TIME);
  RegexDagCompressor<L> comp;
  comp.compress(dag);
  stats.stop(SifaStats::Key::DAG_COMPRESSION_TIME);
  stats.add(SifaStats::Key::DAG_COMPRESSION_RETAINED_NODES, dag.nodes().size());
}

template <typename L>
RegexDag<L> regexToDag(SifaStats &stats,
                       const lotus::pathexpressions::RegexRef<L> &regex) {
  RegexToDag<L> toDag = createRegexToDag<L>(stats);
  addToDag(stats, toDag, regex);
  return getDagAndReset(stats, toDag);
}

} // namespace sifa
} // namespace lotus

#endif // LOTUS_VERIFICATION_SIFA_STATISTICS_REGEXSTATUTILS_TPP
