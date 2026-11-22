/** @file
  Shared ABI between SnapshotDxe, SmmSnapshotDriver, and the host extractor.

  The structures below are packed and little-endian.  Keep them in sync with
  scripts/snapshot_format.py.

  Buffer layout (all offsets are physical addresses unless stated otherwise):

    +0x0000  SNAPSHOT_HEADER (4096 bytes, includes the slot directory)
    +0x1000  SNAPSHOT_MEM_RANGE[RangeCount]
    ...      padding to 4 KiB
    DataStart
      FullSlot:  SNAPSHOT_SLOT_HEADER (4096) | page map | page data
      DeltaStart: N delta slots, same layout

  Copyright (c) 2026
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef SNAPSHOT_H_
#define SNAPSHOT_H_

#include <Base.h>

#define SNAPSHOT_HEADER_MAGIC   0x31534D4D50414E53ULL  // "SNAPMMS1"
#define SNAPSHOT_SLOT_MAGIC     0x544F4C5350414E53ULL  // "SNAPSLOT"
#define SNAPSHOT_CONFIG_MAGIC   0x3147464350414E53ULL  // "SNAPFCG1"
#define SNAPSHOT_VERSION        1

#define SNAPSHOT_HEADER_SIZE    0x1000
#define SNAPSHOT_PAGE_SIZE      0x1000
#define SNAPSHOT_MAX_RANGES     64
#define SNAPSHOT_MAX_DIR_ENTRIES 32
#define SNAPSHOT_FULL_DIR_INDEX 0

#define SNAPSHOT_MODE_A         0   // full copy
#define SNAPSHOT_MODE_B         1   // hash-based incremental
#define SNAPSHOT_MODE_C         2   // dirty-bit / page-table walk
#define SNAPSHOT_MODE_D         3   // sliced full copy

#define SNAPSHOT_STATE_EMPTY    0
#define SNAPSHOT_STATE_READY    1
#define SNAPSHOT_STATE_CAPTURING 2
#define SNAPSHOT_STATE_ERROR    3

#define SNAPSHOT_SLOT_COMPLETE  0x00000001
#define SNAPSHOT_SLOT_FULL      0x00000002
#define SNAPSHOT_SLOT_TRUNCATED 0x00000004
#define SNAPSHOT_SLOT_CAPTURING 0x00000008
#define SNAPSHOT_SLOT_PARTIAL   0x00000010

#define SNAPSHOT_PAGE_FULL      0x00000001
#define SNAPSHOT_PAGE_DELTA     0x00000002
#define SNAPSHOT_PAGE_DIRTY     0x00000004
#define SNAPSHOT_PAGE_SLICED    0x00000008

#define SNAPSHOT_CONFIG_INCLUDE_RECLAIMABLE 0x00000001

#pragma pack(push, 1)

typedef struct {
  UINT64    Base;
  UINT64    Length;
  UINT32    Type;      // EFI_MEMORY_TYPE of the source descriptor
  UINT32    Flags;     // reserved, 0
} SNAPSHOT_MEM_RANGE;

typedef struct {
  UINT64    Magic;
  UINT64    Offset;    // byte offset from the start of the snapshot buffer
  UINT64    Length;    // total bytes used by this slot
  UINT64    Sequence;  // monotonically increasing snapshot sequence
  UINT64    Tick;      // periodic SMI tick at slot start
  UINT64    Tsc;       // TSC at slot start
  UINT32    Mode;
  UINT32    Flags;
  UINT32    PageCount;
  UINT32    Reserved0;
  UINT64    DataBytes;
} SNAPSHOT_SLOT_ENTRY;

typedef struct {
  UINT64    Magic;
  UINT32    Version;
  UINT32    Mode;
  UINT64    Sequence;
  UINT64    Tick;
  UINT64    Tsc;
  UINT64    SmiTsc;          // duration of the SMI that completed this slot
  UINT32    PageCount;
  UINT32    CapacityPages;
  UINT32    Flags;
  UINT32    Reserved0;
  UINT64    DataOffset;      // from slot start, 4 KiB aligned
  UINT64    PageMapOffset;   // from slot start (normally 4096)
  UINT64    DataBytes;
  UINT64    Reserved[4];
} SNAPSHOT_SLOT_HEADER;

typedef struct {
  UINT64    PhysAddr;
  UINT32    Hash;            // FNV-1a 32-bit of the captured page
  UINT32    Flags;
} SNAPSHOT_PAGE_ENTRY;

typedef struct {
  UINT64    Magic;
  UINT32    Version;
  UINT32    HeaderSize;
  UINT32    Mode;
  UINT32    State;

  UINT64    BufferBase;      // physical address of this buffer
  UINT64    BufferSize;
  UINT64    DataStart;       // physical address of the full slot
  UINT64    DataEnd;         // BufferBase + BufferSize
  UINT64    FullSlotOffset;  // offset from BufferBase
  UINT64    FullSlotCapacity;
  UINT64    DeltaStart;      // offset from BufferBase
  UINT64    DeltaSlotSize;
  UINT32    DeltaSlotCount;
  UINT32    DeltaWriteIndex;

  UINT64    Sequence;        // last started sequence
  UINT64    FullSequence;    // sequence of the full slot (0 = none)
  UINT64    Tick;            // periodic timer ticks seen
  UINT64    TscHz;

  UINT64    SmiCount;
  UINT64    SmiTscTotal;
  UINT64    SmiTscMax;
  UINT64    SmiTscLast;
  UINT64    PagesTotal;      // total pages in the validated ranges
  UINT64    PagesLast;       // pages captured in the last completed snapshot
  UINT64    BytesCopied;
  UINT64    GuestRamTop;     // top of guest physical RAM (raw image size)

  UINT32    RangeCount;
  UINT32    RangeEntrySize;
  UINT64    RangesOffset;    // from BufferBase, normally 0x1000
  UINT32    Flags;
  UINT32    Reserved0;
  UINT64    Reserved[6];    // [0] = configured period in seconds

  SNAPSHOT_SLOT_ENTRY Slots[SNAPSHOT_MAX_DIR_ENTRIES];
} SNAPSHOT_HEADER;

typedef struct {
  UINT64    Signature;
  UINT32    Version;
  UINT32    Mode;
  UINT64    BufferBase;
  UINT64    BufferSize;
  UINT32    MaxPagesPerSmi;
  UINT32    PeriodSeconds;
  UINT64    GuestRamTop;
  UINT64    TscHz;
  UINT32    RangeCount;
  UINT32    RangeEntrySize;
  UINT64    Flags;
  SNAPSHOT_MEM_RANGE Ranges[SNAPSHOT_MAX_RANGES];
} SNAPSHOT_CONFIG;

#pragma pack(pop)

#endif // SNAPSHOT_H_
