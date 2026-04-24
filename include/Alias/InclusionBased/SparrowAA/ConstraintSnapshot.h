#pragma once

#include <cstdint>

namespace sparrow_aa {

constexpr uint32_t kSnapshotVersion = 1;
constexpr uint32_t kInvalidContextId = UINT32_MAX;

enum class SnapshotPhase : uint32_t {
  Collect = 1,
  Optimize = 2,
};

enum class SnapshotSectionKind : uint32_t {
  Contexts = 1,
  Nodes = 2,
  Strings = 3,
  AddrOfRowOffsets = 4,
  AddrOfColumns = 5,
  CopyRowOffsets = 6,
  CopyColumns = 7,
  LoadRowOffsets = 8,
  LoadColumns = 9,
  StoreRowOffsets = 10,
  StoreColumns = 11,
};

#pragma pack(push, 1)

struct SnapshotHeader {
  char magic[8];
  uint32_t version;
  uint32_t phase;
  uint32_t node_count;
  uint32_t context_count;
  uint32_t string_bytes;
  uint32_t section_count;
  uint64_t section_table_offset;
};

struct SnapshotSectionHeader {
  uint32_t kind;
  uint32_t reserved;
  uint64_t offset;
  uint64_t byte_size;
  uint64_t element_count;
};

struct SnapshotContextRecord {
  uint32_t context_id;
  uint32_t label_offset;
};

struct SnapshotNodeRecord {
  uint32_t node_id;
  uint32_t representative;
  uint32_t context_id;
  uint16_t node_type;
  uint16_t node_role;
  uint32_t value_offset;
};

#pragma pack(pop)

} // namespace sparrow_aa
