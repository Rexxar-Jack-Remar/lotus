#pragma once

#include "Alias/Dynamic/LogRecord.h"

#include <fstream>
#include <optional>
#include <vector>

namespace dynamic {

class EagerLogReader {
public:
  EagerLogReader() = delete;

  static std::vector<LogRecord> readLogFromFile(const char *fileName);
};

class LazyLogReader {
private:
  std::ifstream ifs;

public:
  LazyLogReader(const char *fileName);

  std::optional<LogRecord> readLogRecord();
};

} // namespace dynamic
