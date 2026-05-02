#include "Dataflow/NPA/Domains/AffineRelationDomain.h"

#include <algorithm>
#include <unordered_set>

#include <llvm/IR/Value.h>

namespace npa {

AffineRelationVocabulary AffineRelationDomain::Vocabulary{};
bool AffineRelationDomain::HasVocabulary = false;

namespace {

using Row = std::vector<llvm::APInt>;
using Matrix = std::vector<Row>;

unsigned numVarsFor(unsigned bitWidth) {
  auto *vocab = AffineRelationDomain::getVocabulary();
  return (!vocab || bitWidth != AffineRelationDomain::componentBitWidth())
             ? 0u
             : static_cast<unsigned>(vocab->values.size());
}

Row zeroRow(unsigned bitWidth, unsigned cols) {
  return Row(cols, llvm::APInt(bitWidth, 0));
}

bool isZeroRow(const Row &row) {
  return std::all_of(row.begin(), row.end(),
                     [](const llvm::APInt &entry) { return entry.isZero(); });
}

int leadingIndex(const Row &row) {
  for (size_t i = 0; i < row.size(); ++i) {
    if (!row[i].isZero())
      return static_cast<int>(i);
  }
  return -1;
}

unsigned rankOf(const llvm::APInt &value) {
  return value.isZero() ? value.getBitWidth() : value.countTrailingZeros();
}

llvm::APInt oddInverse(const llvm::APInt &odd) {
  unsigned bitWidth = odd.getBitWidth();
  llvm::APInt inv(bitWidth, 1);
  llvm::APInt two(bitWidth, 2);
  for (unsigned bits = 1; bits < bitWidth; bits <<= 1)
    inv *= (two - odd * inv);
  return inv;
}

void scaleRow(Row &row, const llvm::APInt &factor) {
  for (auto &entry : row)
    entry *= factor;
}

void subtractScaledRow(Row &row, const Row &pivot, const llvm::APInt &factor) {
  for (size_t i = 0; i < row.size(); ++i)
    row[i] -= factor * pivot[i];
}

Matrix howellize(Matrix rows, unsigned bitWidth) {
  if (rows.empty())
    return rows;
  const size_t cols = rows.front().size();
  size_t nextRow = 0;

  for (size_t col = 0; col < cols; ++col) {
    std::vector<size_t> candidates;
    for (size_t r = nextRow; r < rows.size(); ++r) {
      if (leadingIndex(rows[r]) == static_cast<int>(col))
        candidates.push_back(r);
    }
    if (candidates.empty())
      continue;

    size_t pivotPos = candidates.front();
    for (size_t idx : candidates) {
      if (rankOf(rows[idx][col]) < rankOf(rows[pivotPos][col]))
        pivotPos = idx;
    }

    unsigned pivotRank = rankOf(rows[pivotPos][col]);
    llvm::APInt oddPart = rows[pivotPos][col].lshr(pivotRank);
    scaleRow(rows[pivotPos], oddInverse(oddPart));

    for (size_t idx : candidates) {
      if (idx == pivotPos)
        continue;
      unsigned curRank = rankOf(rows[idx][col]);
      llvm::APInt factor(bitWidth, 1);
      factor <<= (curRank - pivotRank);
      factor *= rows[idx][col].lshr(curRank);
      subtractScaledRow(rows[idx], rows[pivotPos], factor);
    }

    rows.erase(std::remove_if(rows.begin(), rows.end(), isZeroRow), rows.end());
    auto it =
        std::find_if(rows.begin() + nextRow, rows.end(), [col](const Row &row) {
          return leadingIndex(row) == static_cast<int>(col);
        });
    if (it == rows.end())
      continue;
    std::iter_swap(rows.begin() + nextRow, it);

    const Row pivot = rows[nextRow];
    for (size_t upper = 0; upper < nextRow; ++upper) {
      llvm::APInt factor = rows[upper][col].lshr(pivotRank);
      subtractScaledRow(rows[upper], pivot, factor);
    }

    if (!rows[nextRow][col].isOne()) {
      llvm::APInt factor(bitWidth, 1);
      factor <<= (bitWidth - pivotRank);
      Row implied = rows[nextRow];
      scaleRow(implied, factor);
      if (!isZeroRow(implied))
        rows.push_back(std::move(implied));
    }

    ++nextRow;
  }

  rows.erase(std::remove_if(rows.begin(), rows.end(), isZeroRow), rows.end());
  return rows;
}

AffineRelationComponent makeIdentityComponent(unsigned bitWidth) {
  AffineRelationComponent component;
  component.bitWidth = bitWidth;
  const unsigned vars = numVarsFor(bitWidth);
  for (unsigned i = 0; i < vars; ++i) {
    Row row = zeroRow(bitWidth, 2 * vars + 1);
    row[i] = llvm::APInt(bitWidth, 1);
    row[vars + i] = llvm::APInt(bitWidth, -1, true);
    component.constraints.push_back(std::move(row));
  }
  component.constraints = howellize(std::move(component.constraints), bitWidth);
  return component;
}

AffineRelationComponent bottomComponent(unsigned bitWidth) {
  AffineRelationComponent component;
  component.bitWidth = bitWidth;
  Row row = zeroRow(bitWidth, 2 * numVarsFor(bitWidth) + 1);
  row.back() = llvm::APInt(bitWidth, 1);
  component.constraints.push_back(std::move(row));
  return component;
}

bool componentIsBottom(const AffineRelationComponent &component) {
  if (component.constraints.size() != 1)
    return false;
  const Row &row = component.constraints.front();
  return std::all_of(row.begin(), row.end() - 1,
                     [](const llvm::APInt &entry) { return entry.isZero(); }) &&
         row.back().isOne();
}

AffineRelationComponent normalizeComponent(AffineRelationComponent component) {
  component.constraints =
      howellize(std::move(component.constraints), component.bitWidth);
  const unsigned vars = numVarsFor(component.bitWidth);
  for (const Row &row : component.constraints) {
    if (leadingIndex(row) == static_cast<int>(2 * vars) && row.back().isOne()) {
      return bottomComponent(component.bitWidth);
    }
  }
  return component;
}

AffineRelationComponent projectSuffix(const AffineRelationComponent &component,
                                      unsigned keepCols) {
  if (component.constraints.empty())
    return component;
  const size_t totalCols = component.constraints.front().size();
  const size_t dropCols = totalCols - keepCols;
  Matrix rows = howellize(component.constraints, component.bitWidth);
  Matrix projected;
  for (const Row &row : rows) {
    int lead = leadingIndex(row);
    if (lead < 0 || static_cast<size_t>(lead) < dropCols)
      continue;
    Row keep(row.begin() + dropCols, row.end());
    if (!isZeroRow(keep))
      projected.push_back(std::move(keep));
  }
  AffineRelationComponent out = component;
  out.constraints = howellize(std::move(projected), component.bitWidth);
  return out;
}

AffineRelationComponent composeComponent(const AffineRelationComponent &outer,
                                         const AffineRelationComponent &inner) {
  if (componentIsBottom(outer) || componentIsBottom(inner))
    return bottomComponent(outer.bitWidth);
  const unsigned vars = numVarsFor(outer.bitWidth);
  Matrix rows;
  for (const Row &row : inner.constraints) {
    Row lifted = zeroRow(outer.bitWidth, 3 * vars + 1);
    std::copy(row.begin() + vars, row.begin() + 2 * vars, lifted.begin());
    std::copy(row.begin(), row.begin() + vars, lifted.begin() + vars);
    lifted.back() = row.back();
    rows.push_back(std::move(lifted));
  }
  for (const Row &row : outer.constraints) {
    Row lifted = zeroRow(outer.bitWidth, 3 * vars + 1);
    std::copy(row.begin(), row.begin() + vars, lifted.begin());
    std::copy(row.begin() + vars, row.begin() + 2 * vars,
              lifted.begin() + 2 * vars);
    lifted.back() = row.back();
    rows.push_back(std::move(lifted));
  }
  AffineRelationComponent tmp;
  tmp.bitWidth = outer.bitWidth;
  tmp.constraints = std::move(rows);
  tmp = normalizeComponent(std::move(tmp));
  return projectSuffix(tmp, 2 * vars + 1);
}

AffineRelationComponent joinComponent(const AffineRelationComponent &lhs,
                                      const AffineRelationComponent &rhs) {
  if (componentIsBottom(lhs))
    return rhs;
  if (componentIsBottom(rhs))
    return lhs;
  const unsigned vars = numVarsFor(lhs.bitWidth);
  Matrix rows;
  for (const Row &row : lhs.constraints) {
    Row lifted = zeroRow(lhs.bitWidth, 4 * vars + 2);
    for (unsigned i = 0; i < 2 * vars + 1; ++i)
      lifted[i] = -row[i];
    std::copy(row.begin(), row.end(), lifted.begin() + 2 * vars + 1);
    rows.push_back(std::move(lifted));
  }
  for (const Row &row : rhs.constraints) {
    Row lifted = zeroRow(lhs.bitWidth, 4 * vars + 2);
    std::copy(row.begin(), row.end(), lifted.begin());
    rows.push_back(std::move(lifted));
  }
  AffineRelationComponent tmp;
  tmp.bitWidth = lhs.bitWidth;
  tmp.constraints = std::move(rows);
  tmp = normalizeComponent(std::move(tmp));
  return projectSuffix(tmp, 2 * vars + 1);
}

AffineRelationComponent meetComponent(const AffineRelationComponent &lhs,
                                      const AffineRelationComponent &rhs) {
  if (componentIsBottom(lhs) || componentIsBottom(rhs))
    return bottomComponent(lhs.bitWidth);
  AffineRelationComponent out;
  out.bitWidth = lhs.bitWidth;
  out.constraints = lhs.constraints;
  out.constraints.insert(out.constraints.end(), rhs.constraints.begin(),
                         rhs.constraints.end());
  return normalizeComponent(std::move(out));
}

AffineRelationComponent
projectAwayColumns(const AffineRelationComponent &component,
                   const std::vector<unsigned> &dropCols, unsigned totalCols) {
  if (componentIsBottom(component) || dropCols.empty())
    return component;

  std::vector<bool> drop(totalCols, false);
  for (unsigned col : dropCols) {
    if (col < totalCols)
      drop[col] = true;
  }

  std::vector<unsigned> order;
  order.reserve(totalCols);
  for (unsigned col = 0; col < totalCols; ++col) {
    if (drop[col])
      order.push_back(col);
  }
  const unsigned dropCount = static_cast<unsigned>(order.size());
  for (unsigned col = 0; col < totalCols; ++col) {
    if (!drop[col])
      order.push_back(col);
  }

  Matrix permuted;
  permuted.reserve(component.constraints.size());
  for (const Row &row : component.constraints) {
    Row out = zeroRow(component.bitWidth, totalCols);
    for (unsigned newCol = 0; newCol < totalCols; ++newCol)
      out[newCol] = row[order[newCol]];
    permuted.push_back(std::move(out));
  }

  Matrix rows = howellize(std::move(permuted), component.bitWidth);
  Matrix projected;
  for (const Row &row : rows) {
    int lead = leadingIndex(row);
    if (lead < 0 || static_cast<unsigned>(lead) < dropCount)
      continue;

    Row expanded = zeroRow(component.bitWidth, totalCols);
    for (unsigned newCol = dropCount; newCol < totalCols; ++newCol)
      expanded[order[newCol]] = row[newCol];
    if (!isZeroRow(expanded))
      projected.push_back(std::move(expanded));
  }

  AffineRelationComponent out = component;
  out.constraints = howellize(std::move(projected), component.bitWidth);
  return out;
}

Matrix identityRows(unsigned bitWidth, unsigned cols) {
  Matrix rows;
  rows.reserve(cols);
  for (unsigned i = 0; i < cols; ++i) {
    Row row = zeroRow(bitWidth, cols);
    row[i] = llvm::APInt(bitWidth, 1);
    rows.push_back(std::move(row));
  }
  return rows;
}

Matrix zeroMatrix(unsigned bitWidth, unsigned rows, unsigned cols) {
  return Matrix(rows, zeroRow(bitWidth, cols));
}

Matrix identityMatrix(unsigned bitWidth, unsigned size) {
  return identityRows(bitWidth, size);
}

Matrix transposeMatrix(const Matrix &matrix, unsigned bitWidth, unsigned rows,
                       unsigned cols) {
  Matrix out = zeroMatrix(bitWidth, cols, rows);
  for (unsigned r = 0; r < rows; ++r) {
    for (unsigned c = 0; c < cols; ++c)
      out[c][r] = matrix[r][c];
  }
  return out;
}

Matrix multiplyMatrices(const Matrix &lhs, const Matrix &rhs,
                        unsigned bitWidth) {
  if (lhs.empty() || rhs.empty())
    return {};
  const unsigned rows = static_cast<unsigned>(lhs.size());
  const unsigned inner = static_cast<unsigned>(rhs.size());
  const unsigned cols = static_cast<unsigned>(rhs.front().size());
  Matrix out = zeroMatrix(bitWidth, rows, cols);
  for (unsigned r = 0; r < rows; ++r) {
    for (unsigned k = 0; k < inner; ++k) {
      if (lhs[r][k].isZero())
        continue;
      for (unsigned c = 0; c < cols; ++c)
        out[r][c] += lhs[r][k] * rhs[k][c];
    }
  }
  return out;
}

void swapRows(Matrix &matrix, unsigned lhs, unsigned rhs) {
  if (lhs != rhs)
    std::swap(matrix[lhs], matrix[rhs]);
}

void swapColumns(Matrix &matrix, unsigned lhs, unsigned rhs) {
  if (lhs == rhs)
    return;
  for (Row &row : matrix)
    std::swap(row[lhs], row[rhs]);
}

void scaleMatrixRow(Matrix &matrix, unsigned row, const llvm::APInt &factor) {
  scaleRow(matrix[row], factor);
}

void scaleMatrixColumn(Matrix &matrix, unsigned column,
                       const llvm::APInt &factor) {
  for (Row &row : matrix)
    row[column] *= factor;
}

void subtractMatrixRows(Matrix &matrix, unsigned row, unsigned pivot,
                        const llvm::APInt &factor) {
  subtractScaledRow(matrix[row], matrix[pivot], factor);
}

void subtractMatrixColumns(Matrix &matrix, unsigned column, unsigned pivot,
                           const llvm::APInt &factor) {
  for (Row &row : matrix)
    row[column] -= factor * row[pivot];
}

Matrix diagonalTorsionComplement(const Matrix &diagonal, unsigned bitWidth,
                                 unsigned rows, unsigned cols) {
  Matrix out;
  const unsigned pivots = std::min(rows, cols);
  for (unsigned i = 0; i < pivots; ++i) {
    unsigned pivotRank = rankOf(diagonal[i][i]);
    Row row = zeroRow(bitWidth, cols);
    if (pivotRank == bitWidth)
      row[i] = llvm::APInt(bitWidth, 1);
    else
      row[i] = llvm::APInt(bitWidth, 1) << (bitWidth - pivotRank);
    if (!isZeroRow(row))
      out.push_back(std::move(row));
  }
  for (unsigned c = rows; c < cols; ++c) {
    Row row = zeroRow(bitWidth, cols);
    row[c] = llvm::APInt(bitWidth, 1);
    out.push_back(std::move(row));
  }
  return howellize(std::move(out), bitWidth);
}

struct InternalDiagonalDecomposition {
  Matrix left;
  Matrix leftInverse;
  Matrix diagonal;
  Matrix right;
  Matrix rightInverse;
};

InternalDiagonalDecomposition diagonalize(Matrix input, unsigned bitWidth,
                                          unsigned rows, unsigned cols) {
  InternalDiagonalDecomposition out;
  Matrix rowOps = identityMatrix(bitWidth, rows);
  Matrix colOps = identityMatrix(bitWidth, cols);
  Matrix rowOpsInverse = identityMatrix(bitWidth, rows);
  Matrix colOpsInverse = identityMatrix(bitWidth, cols);
  out.diagonal = std::move(input);

  unsigned pivot = 0;
  while (pivot < rows && pivot < cols) {
    int bestRow = -1;
    int bestCol = -1;
    for (unsigned r = pivot; r < rows; ++r) {
      for (unsigned c = pivot; c < cols; ++c) {
        if (out.diagonal[r][c].isZero())
          continue;
        if (bestRow < 0 || rankOf(out.diagonal[r][c]) <
                               rankOf(out.diagonal[bestRow][bestCol])) {
          bestRow = static_cast<int>(r);
          bestCol = static_cast<int>(c);
        }
      }
    }
    if (bestRow < 0)
      break;

    swapRows(out.diagonal, pivot, static_cast<unsigned>(bestRow));
    swapRows(rowOps, pivot, static_cast<unsigned>(bestRow));
    swapColumns(rowOpsInverse, pivot, static_cast<unsigned>(bestRow));
    swapColumns(out.diagonal, pivot, static_cast<unsigned>(bestCol));
    swapColumns(colOps, pivot, static_cast<unsigned>(bestCol));
    swapRows(colOpsInverse, pivot, static_cast<unsigned>(bestCol));

    unsigned pivotRank = rankOf(out.diagonal[pivot][pivot]);
    llvm::APInt oddPart = out.diagonal[pivot][pivot].lshr(pivotRank);
    llvm::APInt invOdd = oddInverse(oddPart);
    scaleMatrixRow(out.diagonal, pivot, invOdd);
    scaleMatrixRow(rowOps, pivot, invOdd);
    scaleMatrixColumn(rowOpsInverse, pivot, oddPart);

    for (unsigned r = 0; r < rows; ++r) {
      if (r == pivot || out.diagonal[r][pivot].isZero())
        continue;
      llvm::APInt factor = out.diagonal[r][pivot].lshr(pivotRank);
      subtractMatrixRows(out.diagonal, r, pivot, factor);
      subtractMatrixRows(rowOps, r, pivot, factor);
      for (unsigned invRow = 0; invRow < rows; ++invRow)
        rowOpsInverse[invRow][pivot] += factor * rowOpsInverse[invRow][r];
    }

    for (unsigned c = 0; c < cols; ++c) {
      if (c == pivot || out.diagonal[pivot][c].isZero())
        continue;
      llvm::APInt factor = out.diagonal[pivot][c].lshr(pivotRank);
      subtractMatrixColumns(out.diagonal, c, pivot, factor);
      subtractMatrixColumns(colOps, c, pivot, factor);
      for (unsigned invCol = 0; invCol < cols; ++invCol)
        colOpsInverse[pivot][invCol] += factor * colOpsInverse[c][invCol];
    }

    ++pivot;
  }

  out.left = rowOpsInverse;
  out.leftInverse = rowOps;
  out.right = colOpsInverse;
  out.rightInverse = colOps;
  return out;
}

Row ksToAgOrder(const Row &row, unsigned vars) {
  Row out;
  out.reserve(row.size());
  out.push_back(row.back());
  out.insert(out.end(), row.begin(), row.begin() + 2 * vars);
  return out;
}

Row agToKsOrder(const Row &row, unsigned vars) {
  Row out;
  out.reserve(row.size());
  out.insert(out.end(), row.begin() + 1, row.begin() + 1 + 2 * vars);
  out.push_back(row.front());
  return out;
}

Matrix generatorsFromKSComponent(const AffineRelationComponent &component) {
  const unsigned bitWidth = component.bitWidth;
  const unsigned vars = numVarsFor(bitWidth);
  const unsigned cols = 2 * vars + 1;
  Matrix kernel =
      AffineRelationDomain::dualizePerp(component.constraints, bitWidth, cols);
  Matrix generators;
  generators.reserve(kernel.size());
  for (const Row &row : kernel) {
    Row agRow = ksToAgOrder(row, vars);
    if (!isZeroRow(agRow))
      generators.push_back(std::move(agRow));
  }
  return howellize(std::move(generators), bitWidth);
}

AffineRelationComponent constraintsFromAGRows(const Matrix &generators,
                                              unsigned bitWidth) {
  const unsigned vars = numVarsFor(bitWidth);
  const unsigned cols = 2 * vars + 1;
  Matrix ksRows;
  ksRows.reserve(generators.size());
  for (const Row &row : generators)
    ksRows.push_back(agToKsOrder(row, vars));

  AffineRelationComponent out;
  out.bitWidth = bitWidth;
  out.constraints = AffineRelationDomain::dualizePerp(ksRows, bitWidth, cols);
  return normalizeComponent(std::move(out));
}

Matrix havocPreStateAGRows(unsigned bitWidth) {
  const unsigned vars = numVarsFor(bitWidth);
  Matrix rows;
  Row constant = zeroRow(bitWidth, 2 * vars + 1);
  constant[0] = llvm::APInt(bitWidth, 1);
  rows.push_back(std::move(constant));
  for (unsigned i = 0; i < vars; ++i) {
    Row row = zeroRow(bitWidth, 2 * vars + 1);
    row[1 + i] = llvm::APInt(bitWidth, 1);
    rows.push_back(std::move(row));
  }
  return rows;
}

Matrix makeExplicitAGRows(Matrix rows, unsigned bitWidth) {
  const unsigned vars = numVarsFor(bitWidth);
  rows = howellize(std::move(rows), bitWidth);
  for (unsigned col = 1; col <= vars; ++col) {
    auto it = std::find_if(rows.begin(), rows.end(), [col](const Row &row) {
      return leadingIndex(row) == static_cast<int>(col);
    });
    if (it == rows.end())
      continue;
    unsigned leadRank = rankOf((*it)[col]);
    if (leadRank == 0 || leadRank >= bitWidth)
      continue;
    Row high = zeroRow(bitWidth, static_cast<unsigned>(it->size()));
    for (unsigned j = 0; j < it->size(); ++j)
      high[j] = (*it)[j].lshr(leadRank);
    rows.push_back(std::move(high));
    rows = howellize(std::move(rows), bitWidth);
  }

  Matrix padded;
  padded.reserve(std::max<size_t>(rows.size(), vars + 1));
  for (unsigned col = 0; col <= vars; ++col) {
    auto it = std::find_if(rows.begin(), rows.end(), [col](const Row &row) {
      return leadingIndex(row) == static_cast<int>(col);
    });
    if (it != rows.end())
      padded.push_back(*it);
    else
      padded.push_back(zeroRow(bitWidth, 2 * vars + 1));
  }
  for (const Row &row : rows) {
    int lead = leadingIndex(row);
    if (lead > static_cast<int>(vars))
      padded.push_back(row);
  }
  return howellize(std::move(padded), bitWidth);
}

std::vector<Matrix> shatterAGRows(Matrix rows, unsigned bitWidth) {
  const unsigned vars = numVarsFor(bitWidth);
  rows = howellize(std::move(rows), bitWidth);
  Matrix explicitRows = rows;
  if (explicitRows.size() < vars + 1)
    explicitRows = makeExplicitAGRows(std::move(explicitRows), bitWidth);

  Row baseAffine = zeroRow(bitWidth, 2 * vars + 1);
  for (const Row &row : explicitRows) {
    if (leadingIndex(row) == 0) {
      baseAffine = row;
      break;
    }
  }

  Matrix base(vars + 1, zeroRow(bitWidth, vars + 1));
  base[0][0] = llvm::APInt(bitWidth, 1);
  for (unsigned post = 0; post < vars; ++post)
    base[0][1 + post] = baseAffine[1 + vars + post];

  for (unsigned pre = 0; pre < vars; ++pre) {
    auto it = std::find_if(
        explicitRows.begin(), explicitRows.end(), [pre](const Row &row) {
          return leadingIndex(row) == static_cast<int>(1 + pre);
        });
    if (it == explicitRows.end())
      continue;
    for (unsigned post = 0; post < vars; ++post)
      base[1 + pre][1 + post] = (*it)[1 + vars + post];
  }

  std::vector<Matrix> transformers;
  transformers.push_back(std::move(base));
  for (const Row &row : explicitRows) {
    int lead = leadingIndex(row);
    if (lead < static_cast<int>(1 + vars))
      continue;
    Matrix extra(vars + 1, zeroRow(bitWidth, vars + 1));
    for (unsigned post = 0; post < vars; ++post)
      extra[0][1 + post] = row[1 + vars + post];
    if (!std::all_of(extra.front().begin(), extra.front().end(),
                     [](const llvm::APInt &entry) { return entry.isZero(); }))
      transformers.push_back(std::move(extra));
  }
  return transformers;
}

Matrix agRowsFromMOSTransformer(const Matrix &transformer, unsigned bitWidth) {
  const unsigned vars = numVarsFor(bitWidth);
  Matrix rows;
  Row constant = zeroRow(bitWidth, 2 * vars + 1);
  constant[0] = llvm::APInt(bitWidth, 1);
  for (unsigned post = 0; post < vars; ++post)
    constant[1 + vars + post] = transformer[0][1 + post];
  rows.push_back(std::move(constant));

  for (unsigned pre = 0; pre < vars; ++pre) {
    Row row = zeroRow(bitWidth, 2 * vars + 1);
    row[1 + pre] = llvm::APInt(bitWidth, 1);
    for (unsigned post = 0; post < vars; ++post)
      row[1 + vars + post] = transformer[1 + pre][1 + post];
    rows.push_back(std::move(row));
  }
  return rows;
}

} // namespace

bool AffineRelationComponent::operator==(
    const AffineRelationComponent &other) const {
  return bitWidth == other.bitWidth && constraints == other.constraints;
}

bool AffineRelation::operator==(const AffineRelation &other) const {
  return bottom == other.bottom && components == other.components;
}

bool AffineGeneratorRelation::operator==(
    const AffineGeneratorRelation &other) const {
  return relation == other.relation && generators == other.generators &&
         bottom == other.bottom && exact == other.exact;
}

bool AffineDiagonalDecomposition::operator==(
    const AffineDiagonalDecomposition &other) const {
  return bitWidth == other.bitWidth && left == other.left &&
         leftInverse == other.leftInverse && diagonal == other.diagonal &&
         right == other.right && rightInverse == other.rightInverse &&
         dual == other.dual && exact == other.exact;
}

bool MOSRelation::operator==(const MOSRelation &other) const {
  return relation == other.relation && transformers == other.transformers &&
         kind == other.kind && exact == other.exact;
}

void AffineRelationDomain::configure(
    const AffineRelationVocabulary *vocabulary) {
  if (vocabulary) {
    Vocabulary = *vocabulary;
    HasVocabulary = true;
  } else {
    Vocabulary = {};
    HasVocabulary = false;
  }
}

const AffineRelationVocabulary *AffineRelationDomain::getVocabulary() {
  return HasVocabulary ? &Vocabulary : nullptr;
}

bool AffineRelationDomain::isTrackedValue(const llvm::Value *value) {
  return HasVocabulary && Vocabulary.indices.count(value);
}

unsigned AffineRelationDomain::bitWidthOf(const llvm::Value *value) {
  auto it = Vocabulary.actualBitWidths.find(value);
  return it == Vocabulary.actualBitWidths.end() ? 0u : it->second;
}

unsigned AffineRelationDomain::componentBitWidth() {
  if (!HasVocabulary || Vocabulary.actualBitWidths.empty())
    return 64;
  auto it = Vocabulary.actualBitWidths.begin();
  unsigned width = it->second;
  for (++it; it != Vocabulary.actualBitWidths.end(); ++it) {
    if (it->second != width)
      return 64;
  }
  return std::max(1u, width);
}

unsigned AffineRelationDomain::indexOf(const llvm::Value *value) {
  auto it = Vocabulary.indices.find(value);
  return it == Vocabulary.indices.end() ? 0u : it->second;
}

AffineRelationDomain::value_type AffineRelationDomain::zero() {
  value_type relation;
  relation.bottom = true;
  if (!HasVocabulary)
    return relation;
  relation.components.emplace(componentBitWidth(),
                              bottomComponent(componentBitWidth()));
  return relation;
}

AffineRelationDomain::value_type AffineRelationDomain::top() {
  value_type relation;
  if (!HasVocabulary)
    return relation;
  AffineRelationComponent component;
  component.bitWidth = componentBitWidth();
  relation.components.emplace(component.bitWidth, std::move(component));
  return relation;
}

AffineRelationDomain::value_type AffineRelationDomain::one() {
  return identity();
}

AffineRelationDomain::value_type AffineRelationDomain::identity() {
  value_type relation;
  if (!HasVocabulary)
    return relation;
  relation.components.emplace(componentBitWidth(),
                              makeIdentityComponent(componentBitWidth()));
  return relation;
}

AffineRelationDomain::value_type AffineRelationDomain::addStateConstraint(
    const value_type &relation, int64_t constant,
    const std::vector<std::pair<const llvm::Value *, int64_t>> &terms) {
  value_type out = relation;
  unsigned bitWidth = componentBitWidth();
  unsigned vars = numVarsFor(bitWidth);
  Row preRow = zeroRow(bitWidth, 2 * vars + 1);
  Row postRow = zeroRow(bitWidth, 2 * vars + 1);
  preRow.back() = llvm::APInt(bitWidth, static_cast<uint64_t>(-constant), true);
  postRow.back() =
      llvm::APInt(bitWidth, static_cast<uint64_t>(-constant), true);
  for (const auto &term : terms) {
    if (!isTrackedValue(term.first))
      return relation;
    unsigned idx = indexOf(term.first);
    llvm::APInt coeff(bitWidth, static_cast<uint64_t>(term.second), true);
    preRow[idx] += coeff;
    postRow[vars + idx] += coeff;
  }
  out.components[bitWidth].constraints.push_back(std::move(preRow));
  out.components[bitWidth].constraints.push_back(std::move(postRow));
  out.components[bitWidth] =
      normalizeComponent(std::move(out.components[bitWidth]));
  return out;
}

AffineRelationDomain::value_type AffineRelationDomain::addPrecondition(
    const value_type &relation, const llvm::Value *value, int64_t constant) {
  if (!isTrackedValue(value))
    return relation;
  return addStateConstraint(relation, constant, {{value, 1}});
}

bool AffineRelationDomain::equal(const value_type &lhs, const value_type &rhs) {
  return lhs == rhs;
}

bool AffineRelationDomain::isBottom(const value_type &relation) {
  if (relation.bottom)
    return true;
  unsigned width = componentBitWidth();
  auto componentIt = relation.components.find(width);
  return componentIt != relation.components.end() &&
         componentIsBottom(componentIt->second);
}

bool AffineRelationDomain::contains(const value_type &lhs,
                                    const value_type &rhs) {
  if (isBottom(rhs))
    return true;
  if (isBottom(lhs))
    return isBottom(rhs);
  return equal(meet(lhs, rhs), rhs);
}

AffineRelationDomain::value_type
AffineRelationDomain::meet(const value_type &lhs, const value_type &rhs) {
  if (lhs.bottom || rhs.bottom)
    return zero();
  value_type out;
  unsigned width = componentBitWidth();
  out.components.emplace(
      width, meetComponent(lhs.components.at(width), rhs.components.at(width)));
  return out;
}

AffineRelationDomain::value_type
AffineRelationDomain::combine(const value_type &lhs, const value_type &rhs) {
  if (lhs.bottom)
    return rhs;
  if (rhs.bottom)
    return lhs;
  value_type out;
  unsigned width = componentBitWidth();
  out.components.emplace(
      width, joinComponent(lhs.components.at(width), rhs.components.at(width)));
  return out;
}

AffineRelationDomain::value_type
AffineRelationDomain::ndetCombine(const value_type &lhs,
                                  const value_type &rhs) {
  return combine(lhs, rhs);
}

AffineRelationDomain::value_type
AffineRelationDomain::condCombine(bool phi, const value_type &t,
                                  const value_type &e) {
  return phi ? t : e;
}

AffineRelationDomain::value_type
AffineRelationDomain::extend(const value_type &outer, const value_type &inner) {
  if (outer.bottom || inner.bottom)
    return zero();
  value_type out;
  unsigned width = componentBitWidth();
  out.components.emplace(width, composeComponent(outer.components.at(width),
                                                 inner.components.at(width)));
  return out;
}

AffineRelationDomain::value_type
AffineRelationDomain::extend_lin(const value_type &outer,
                                 const value_type &inner) {
  return extend(outer, inner);
}

AffineRelationDomain::value_type
AffineRelationDomain::subtract(const value_type &lhs,
                               const value_type & /*rhs*/) {
  return lhs;
}

AffineRelationDomain::value_type
AffineRelationDomain::project(const value_type &relation) {
  const auto *vocab = getVocabulary();
  if (!vocab || vocab->localValues.empty() || relation.bottom)
    return relation;

  std::unordered_set<const llvm::Value *> locals(vocab->localValues.begin(),
                                                 vocab->localValues.end());
  std::vector<const llvm::Value *> keepValues;
  keepValues.reserve(vocab->values.size());
  for (const llvm::Value *value : vocab->values) {
    if (!locals.count(value))
      keepValues.push_back(value);
  }
  return projectOnto(relation, keepValues);
}

AffineRelationDomain::value_type
AffineRelationDomain::makeForget(const llvm::Value *dest) {
  value_type relation = identity();
  if (!isTrackedValue(dest))
    return relation;
  unsigned bitWidth = componentBitWidth();
  unsigned idx = indexOf(dest);
  auto &rows = relation.components[bitWidth].constraints;
  rows.erase(
      std::remove_if(
          rows.begin(), rows.end(),
          [idx](const Row &row) {
            return row[idx].isOne() &&
                   row[idx + numVarsFor(row.front().getBitWidth())].isAllOnes();
          }),
      rows.end());
  relation.components[bitWidth] =
      normalizeComponent(std::move(relation.components[bitWidth]));
  return relation;
}

AffineRelationDomain::value_type
AffineRelationDomain::havoc(const value_type &relation,
                            const llvm::Value *value) {
  return havoc(relation, std::vector<const llvm::Value *>{value});
}

AffineRelationDomain::value_type
AffineRelationDomain::havoc(const value_type &relation,
                            const std::vector<const llvm::Value *> &values) {
  if (relation.bottom)
    return relation;
  unsigned bitWidth = componentBitWidth();
  unsigned vars = numVarsFor(bitWidth);
  std::vector<unsigned> dropCols;
  dropCols.reserve(values.size() * 2);
  for (const llvm::Value *value : values) {
    if (!isTrackedValue(value))
      continue;
    unsigned idx = indexOf(value);
    dropCols.push_back(idx);
    dropCols.push_back(vars + idx);
  }
  if (dropCols.empty())
    return relation;

  value_type out = relation;
  out.components[bitWidth] =
      projectAwayColumns(out.components.at(bitWidth), dropCols, 2 * vars + 1);
  out.components[bitWidth] =
      normalizeComponent(std::move(out.components[bitWidth]));
  return out;
}

AffineRelationDomain::value_type AffineRelationDomain::projectOnto(
    const value_type &relation,
    const std::vector<const llvm::Value *> &keepValues) {
  if (relation.bottom)
    return relation;
  unsigned bitWidth = componentBitWidth();
  unsigned vars = numVarsFor(bitWidth);
  std::vector<bool> keep(vars, false);
  for (const llvm::Value *value : keepValues) {
    if (isTrackedValue(value))
      keep[indexOf(value)] = true;
  }

  std::vector<unsigned> dropCols;
  for (unsigned idx = 0; idx < vars; ++idx) {
    if (keep[idx])
      continue;
    dropCols.push_back(idx);
    dropCols.push_back(vars + idx);
  }
  if (dropCols.empty())
    return relation;

  value_type out = relation;
  out.components[bitWidth] =
      projectAwayColumns(out.components.at(bitWidth), dropCols, 2 * vars + 1);
  out.components[bitWidth] =
      normalizeComponent(std::move(out.components[bitWidth]));
  return out;
}

AffineRelationDomain::value_type AffineRelationDomain::mergePreservingLocals(
    const value_type &callSite, const value_type &calleeExit,
    const std::vector<const llvm::Value *> &locals) {
  if (callSite.bottom || calleeExit.bottom)
    return zero();

  unsigned bitWidth = componentBitWidth();
  unsigned vars = numVarsFor(bitWidth);
  std::vector<unsigned> postLocalCols;
  postLocalCols.reserve(locals.size());
  for (const llvm::Value *local : locals) {
    if (isTrackedValue(local))
      postLocalCols.push_back(vars + indexOf(local));
  }

  value_type adjusted = calleeExit;
  if (!postLocalCols.empty()) {
    adjusted.components[bitWidth] = projectAwayColumns(
        adjusted.components.at(bitWidth), postLocalCols, 2 * vars + 1);
  }

  auto &component = adjusted.components[bitWidth];
  for (const llvm::Value *local : locals) {
    if (!isTrackedValue(local))
      continue;
    unsigned idx = indexOf(local);
    Row row = zeroRow(bitWidth, 2 * vars + 1);
    row[idx] = llvm::APInt(bitWidth, 1);
    row[vars + idx] = llvm::APInt(bitWidth, -1, true);
    component.constraints.push_back(std::move(row));
  }
  adjusted.components[bitWidth] =
      normalizeComponent(std::move(adjusted.components[bitWidth]));
  return extend(adjusted, callSite);
}

llvm::APInt AffineRelationDomain::size(const value_type &relation) {
  unsigned bitWidth = componentBitWidth();
  unsigned vars = numVarsFor(bitWidth);
  unsigned valueVars = 2 * vars;
  unsigned resultBits = std::max(1u, bitWidth * valueVars + 1);
  if (relation.bottom)
    return llvm::APInt(resultBits, 0);

  auto componentIt = relation.components.find(bitWidth);
  if (componentIt == relation.components.end())
    return llvm::APInt(resultBits, 0);
  if (componentIsBottom(componentIt->second))
    return llvm::APInt(resultBits, 0);

  Matrix rows = howellize(componentIt->second.constraints, bitWidth);
  std::vector<bool> hasLead(valueVars, false);
  unsigned exponent = 0;
  for (const Row &row : rows) {
    int lead = leadingIndex(row);
    if (lead < 0)
      continue;
    if (static_cast<unsigned>(lead) >= valueVars)
      continue;
    hasLead[lead] = true;
    exponent += rankOf(row[lead]);
  }
  for (unsigned col = 0; col < valueVars; ++col) {
    if (!hasLead[col])
      exponent += bitWidth;
  }

  llvm::APInt out(resultBits, 1);
  out <<= exponent;
  return out;
}

AffineRelationDomain::value_type AffineRelationDomain::makeAffineAssignment(
    const llvm::Value *dest, int64_t constant,
    const std::vector<std::pair<const llvm::Value *, int64_t>> &terms) {
  if (!isTrackedValue(dest))
    return identity();
  value_type relation = makeForget(dest);
  unsigned bitWidth = componentBitWidth();
  unsigned vars = numVarsFor(bitWidth);
  unsigned idx = indexOf(dest);
  Row row = zeroRow(bitWidth, 2 * vars + 1);
  row[vars + idx] = llvm::APInt(bitWidth, 1);
  row.back() = llvm::APInt(bitWidth, static_cast<uint64_t>(-constant), true);
  for (const auto &term : terms) {
    if (!isTrackedValue(term.first))
      return makeForget(dest);
    row[indexOf(term.first)] =
        llvm::APInt(bitWidth, static_cast<uint64_t>(-term.second), true);
  }
  relation.components[bitWidth].constraints.push_back(std::move(row));
  relation.components[bitWidth] =
      normalizeComponent(std::move(relation.components[bitWidth]));
  return relation;
}

AffineRelationDomain::value_type
AffineRelationDomain::makeAffineCongruenceAssignment(
    const llvm::Value *dest, unsigned modulusBits, int64_t constant,
    const std::vector<std::pair<const llvm::Value *, int64_t>> &terms) {
  if (!isTrackedValue(dest))
    return identity();

  unsigned bitWidth = componentBitWidth();
  if (modulusBits >= bitWidth)
    return makeAffineAssignment(dest, constant, terms);

  value_type relation = makeForget(dest);
  if (modulusBits == 0)
    return relation;

  unsigned vars = numVarsFor(bitWidth);
  unsigned idx = indexOf(dest);
  llvm::APInt scale(bitWidth, 1);
  scale <<= (bitWidth - modulusBits);

  Row row = zeroRow(bitWidth, 2 * vars + 1);
  row[vars + idx] = scale;
  row.back() =
      llvm::APInt(bitWidth, static_cast<uint64_t>(-constant), true) * scale;
  for (const auto &term : terms) {
    if (!isTrackedValue(term.first))
      return makeForget(dest);
    row[indexOf(term.first)] +=
        llvm::APInt(bitWidth, static_cast<uint64_t>(-term.second), true) *
        scale;
  }

  relation.components[bitWidth].constraints.push_back(std::move(row));
  relation.components[bitWidth] =
      normalizeComponent(std::move(relation.components[bitWidth]));
  return relation;
}

AffineGeneratorRelation
AffineRelationDomain::toAffineGenerator(const value_type &relation) {
  AffineGeneratorRelation out;
  out.relation = relation;
  out.bottom = isBottom(relation);
  out.exact = true;
  if (out.bottom)
    return out;

  for (const auto &component : relation.components) {
    out.generators[component.first] =
        generatorsFromKSComponent(component.second);
  }
  return out;
}

AffineRelationDomain::value_type AffineRelationDomain::fromAffineGenerator(
    const AffineGeneratorRelation &relation) {
  if (relation.bottom)
    return zero();
  if (relation.generators.empty())
    return relation.relation;

  value_type out;
  for (const auto &component : relation.generators) {
    out.components.emplace(
        component.first,
        constraintsFromAGRows(component.second, component.first));
  }
  return out;
}

AffineGeneratorRelation
AffineRelationDomain::joinAffineGenerators(const AffineGeneratorRelation &lhs,
                                           const AffineGeneratorRelation &rhs) {
  if (lhs.bottom)
    return rhs;
  if (rhs.bottom)
    return lhs;

  AffineGeneratorRelation out;
  out.generators = lhs.generators;
  for (const auto &component : rhs.generators) {
    auto &rows = out.generators[component.first];
    rows.insert(rows.end(), component.second.begin(), component.second.end());
    rows = howellize(std::move(rows), component.first);
  }
  out.relation = fromAffineGenerator(out);
  out.exact = lhs.exact && rhs.exact;
  return out;
}

AffineDiagonalDecomposition
AffineRelationDomain::diagonalDecompose(const AffineMatrix &matrix,
                                        unsigned bitWidth) {
  const unsigned rows = static_cast<unsigned>(matrix.size());
  const unsigned cols =
      matrix.empty() ? 0u : static_cast<unsigned>(matrix.front().size());
  AffineDiagonalDecomposition out;
  out.bitWidth = bitWidth;
  if (cols == 0) {
    out.left = identityMatrix(bitWidth, rows);
    out.leftInverse = identityMatrix(bitWidth, rows);
    out.right = {};
    out.rightInverse = {};
    out.diagonal = matrix;
    out.dual = {};
    out.exact = true;
    return out;
  }

  InternalDiagonalDecomposition decomposition =
      diagonalize(matrix, bitWidth, rows, cols);
  Matrix diagonalDual =
      diagonalTorsionComplement(decomposition.diagonal, bitWidth, rows, cols);
  Matrix rightInverseTranspose =
      transposeMatrix(decomposition.rightInverse, bitWidth, cols, cols);

  out.left = std::move(decomposition.left);
  out.leftInverse = std::move(decomposition.leftInverse);
  out.diagonal = std::move(decomposition.diagonal);
  out.right = std::move(decomposition.right);
  out.rightInverse = std::move(decomposition.rightInverse);
  out.dual =
      howellize(multiplyMatrices(diagonalDual, rightInverseTranspose, bitWidth),
                bitWidth);
  out.exact = true;
  return out;
}

AffineMatrix AffineRelationDomain::dualizePerp(const AffineMatrix &matrix,
                                               unsigned bitWidth,
                                               unsigned columns) {
  if (matrix.empty())
    return identityRows(bitWidth, columns);

  InternalDiagonalDecomposition decomposition = diagonalize(
      matrix, bitWidth, static_cast<unsigned>(matrix.size()), columns);
  Matrix diagonalDual =
      diagonalTorsionComplement(decomposition.diagonal, bitWidth,
                                static_cast<unsigned>(matrix.size()), columns);
  Matrix rightInverseTranspose =
      transposeMatrix(decomposition.rightInverse, bitWidth, columns, columns);
  Matrix dual = multiplyMatrices(diagonalDual, rightInverseTranspose, bitWidth);
  return howellize(std::move(dual), bitWidth);
}

MOSRelation AffineRelationDomain::toMOS(const value_type &relation) {
  MOSRelation out;
  out.kind = MOSRelation::ConversionKind::Direct;
  AffineGeneratorRelation ag = toAffineGenerator(relation);
  if (ag.bottom) {
    out.relation = zero();
    out.exact = true;
    return out;
  }

  for (const auto &component : ag.generators) {
    out.transformers[component.first] =
        shatterAGRows(component.second, component.first);
  }
  out.relation = fromMOS(out);
  out.exact = true;
  return out;
}

MOSRelation AffineRelationDomain::toMOSWithHavocedPreStateGuards(
    const value_type &relation) {
  MOSRelation out;
  out.kind = MOSRelation::ConversionKind::HavocPreStateGuards;
  AffineGeneratorRelation ag = toAffineGenerator(relation);
  if (ag.bottom) {
    out.relation = zero();
    out.exact = true;
    return out;
  }

  for (const auto &component : ag.generators) {
    Matrix rows = component.second;
    Matrix havocRows = havocPreStateAGRows(component.first);
    rows.insert(rows.end(), havocRows.begin(), havocRows.end());
    rows = howellize(std::move(rows), component.first);
    out.transformers[component.first] =
        shatterAGRows(std::move(rows), component.first);
  }
  out.relation = fromMOS(out);
  out.exact = true;
  return out;
}

MOSRelation
AffineRelationDomain::toMOSWithMakeExplicit(const value_type &relation) {
  MOSRelation out;
  out.kind = MOSRelation::ConversionKind::MakeExplicit;
  AffineGeneratorRelation ag = toAffineGenerator(relation);
  if (ag.bottom) {
    out.relation = zero();
    out.exact = true;
    return out;
  }

  for (const auto &component : ag.generators) {
    Matrix rows = makeExplicitAGRows(component.second, component.first);
    out.transformers[component.first] =
        shatterAGRows(std::move(rows), component.first);
  }
  out.relation = fromMOS(out);
  out.exact = true;
  return out;
}

AffineRelationDomain::value_type
AffineRelationDomain::fromMOS(const MOSRelation &relation) {
  if (relation.transformers.empty())
    return relation.relation;

  AffineGeneratorRelation ag;
  for (const auto &component : relation.transformers) {
    Matrix rows;
    for (const Matrix &transformer : component.second) {
      Matrix transformerRows =
          agRowsFromMOSTransformer(transformer, component.first);
      rows.insert(rows.end(), transformerRows.begin(), transformerRows.end());
    }
    ag.generators[component.first] =
        howellize(std::move(rows), component.first);
  }
  ag.relation = relation.relation;
  ag.exact = relation.exact;
  return fromAffineGenerator(ag);
}

MOSRelation AffineRelationDomain::joinMOS(const MOSRelation &lhs,
                                          const MOSRelation &rhs) {
  MOSRelation out;
  out.transformers = lhs.transformers;
  for (const auto &component : rhs.transformers) {
    auto &transformers = out.transformers[component.first];
    transformers.insert(transformers.end(), component.second.begin(),
                        component.second.end());
  }
  out.relation = out.transformers.empty() ? combine(lhs.relation, rhs.relation)
                                          : fromMOS(out);
  out.kind =
      lhs.kind == rhs.kind ? lhs.kind : MOSRelation::ConversionKind::Direct;
  out.exact = lhs.exact && rhs.exact;
  return out;
}

} // namespace npa
