/** @file
  SmmSnapshotDriver - periodic physical memory snapshot engine.

  Modes:
    A  full copy of every validated page
    B  FNV-1a hash per page, copy changed pages
    C  walk each CPU's CR3 page tables, copy active pages whose dirty bit is
       set and whose hash changed (dirty bit is never cleared)
    D  full copy sliced across periodic SMIs (MaxPagesPerSmi cursor); keeps
       the completed image and idles (one full slot)

  Hard SMM rules obeyed here:
    - no gBS/gRT calls
    - only SMRAM, the validated source ranges, and the reserved buffer are
      touched
    - no allocation in the SMI path (tracking arrays are allocated once when
      the configuration message is accepted)

  Copyright (c) 2026
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Snapshot.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/SmmServicesTableLib.h>
#include <Library/PcdLib.h>
#include <Protocol/SmmCpu.h>
#include <Protocol/SmmPeriodicTimerDispatch2.h>

#define SNAPSHOT_PAGE_MASK  0x000FFFFFFFFFF000ULL
#define SNAPSHOT_PDPTE_1G   0x000FFFFFC0000000ULL
#define SNAPSHOT_PDE_2M     0x000FFFFFFFE00000ULL

#define SNAPSHOT_PTE_PRESENT  BIT0
#define SNAPSHOT_PTE_DIRTY    BIT6
#define SNAPSHOT_PTE_PS       BIT7

#define SNAPSHOT_MAX_PFN      0x100000U   // 4 GiB / 4 KiB

typedef struct {
  UINT32  RangeIndex;
  UINT64  PageIndex;
  UINT32  Ordinal;
} SNAPSHOT_PAGE_CURSOR;

typedef struct {
  BOOLEAN               Active;
  UINT64                Sequence;
  UINT64                SlotOffset;
  UINT64                SlotCapacity;
  UINT32                CapacityPages;
  UINT32                PagesWritten;
  SNAPSHOT_PAGE_CURSOR Cursor;
} SNAPSHOT_FULL_SLICE;

typedef struct {
  SNAPSHOT_SLOT_HEADER  *Slot;
  UINT32                Added;
  UINT32                Max;
  UINT32                CapPages;
  BOOLEAN               Truncated;
} SNAPSHOT_DIRTY_CTX;

STATIC SNAPSHOT_CONFIG                          mConfig;
STATIC BOOLEAN                                  mConfigured;
STATIC BOOLEAN                                  mConfigLocked;
STATIC UINT32                                   mPageCount;
STATIC UINT32                                   mMaxPfn;
STATIC UINT32                                   *mHash;
STATIC UINT8                                    *mHashValid;
STATIC UINT8                                    *mValidPfn;
STATIC UINT32                                   *mSeenGen;
STATIC UINT32                                   mSeenGeneration;
STATIC EFI_SMM_CPU_PROTOCOL                     *mCpuProtocol;
STATIC EFI_SMM_PERIODIC_TIMER_DISPATCH2_PROTOCOL *mTimerProtocol;
STATIC EFI_HANDLE                               mTimerHandle;
STATIC EFI_HANDLE                               mConfigHandle;
STATIC SNAPSHOT_FULL_SLICE                      mSlice;
STATIC BOOLEAN                                  mForceFull;
STATIC UINT32                                   mFullEvery;
STATIC UINT64                                   mSmiStartTsc;
STATIC UINT64                                   mLastTickTsc;

STATIC
SNAPSHOT_HEADER *
GetHeader (
  VOID
  )
{
  return (SNAPSHOT_HEADER *)(UINTN)mConfig.BufferBase;
}

STATIC
BOOLEAN
IsValidPa (
  IN UINT64  Pa
  )
{
  UINT32  Pfn;

  if ((Pa & (SNAPSHOT_PAGE_SIZE - 1)) != 0) {
    return FALSE;
  }

  Pfn = (UINT32)(Pa >> 12);
  if (Pfn >= mMaxPfn) {
    return FALSE;
  }

  return (BOOLEAN)((mValidPfn[Pfn >> 3] & (UINT8)(1U << (Pfn & 7))) != 0);
}

STATIC
BOOLEAN
IsValidRange (
  IN UINT64  Pa,
  IN UINTN   Size
  )
{
  UINT64  End;
  UINT32  Pfn;
  UINT32  PfnEnd;

  End = Pa + Size;
  if ((End < Pa) || (Size == 0)) {
    return FALSE;
  }

  Pfn    = (UINT32)(Pa >> 12);
  PfnEnd = (UINT32)((End - 1) >> 12);
  if (PfnEnd >= mMaxPfn) {
    return FALSE;
  }

  for (; Pfn <= PfnEnd; Pfn++) {
    if ((mValidPfn[Pfn >> 3] & (UINT8)(1U << (Pfn & 7))) == 0) {
      return FALSE;
    }
  }

  return TRUE;
}

STATIC
UINT64
ReadPhys64 (
  IN UINT64  Pa
  )
{
  if (!IsValidRange (Pa, sizeof (UINT64))) {
    return 0;
  }

  return *(volatile UINT64 *)(UINTN)Pa;
}

STATIC
UINT32
HashPage (
  IN UINT64  Pa
  )
{
  volatile UINT64  *Data;
  UINT64           Hash;
  UINTN            Index;

  Data = (volatile UINT64 *)(UINTN)Pa;
  Hash = 14695981039346656037ULL;  // FNV-1a 64-bit offset basis
  for (Index = 0; Index < (SNAPSHOT_PAGE_SIZE / sizeof (UINT64)); Index++) {
    Hash ^= Data[Index];
    Hash *= 1099511628211ULL;
  }

  return (UINT32)(Hash ^ (Hash >> 32));
}

STATIC
VOID
SetHash (
  IN UINT32  Pfn,
  IN UINT32  Hash
  )
{
  mHash[Pfn] = Hash;
  mHashValid[Pfn >> 3] |= (UINT8)(1U << (Pfn & 7));
}

STATIC
BOOLEAN
HashMatches (
  IN UINT32  Pfn,
  IN UINT32  Hash
  )
{
  if ((mHashValid[Pfn >> 3] & (UINT8)(1U << (Pfn & 7))) == 0) {
    return FALSE;
  }

  return (BOOLEAN)(mHash[Pfn] == Hash);
}

STATIC
BOOLEAN
CursorNext (
  IN OUT SNAPSHOT_PAGE_CURSOR  *Cursor,
  OUT    UINT64                *Pa
  )
{
  SNAPSHOT_MEM_RANGE  *Range;
  UINT64              Pages;

  while (Cursor->RangeIndex < mConfig.RangeCount) {
    Range = &mConfig.Ranges[Cursor->RangeIndex];
    Pages = Range->Length >> 12;
    if (Cursor->PageIndex < Pages) {
      *Pa = Range->Base + (Cursor->PageIndex << 12);
      Cursor->PageIndex++;
      Cursor->Ordinal++;
      return TRUE;
    }

    Cursor->RangeIndex++;
    Cursor->PageIndex = 0;
  }

  return FALSE;
}

STATIC
UINT32
SlotCapacityPages (
  IN UINT64  Capacity
  )
{
  UINT64  MapBytes;
  UINT64  DataOffset;
  UINT64  Pages;

  if (Capacity <= (2 * SNAPSHOT_HEADER_SIZE)) {
    return 0;
  }

  MapBytes = (Capacity - SNAPSHOT_HEADER_SIZE) *
             sizeof (SNAPSHOT_PAGE_ENTRY) /
             (SNAPSHOT_PAGE_SIZE + sizeof (SNAPSHOT_PAGE_ENTRY));
  Pages = MapBytes / sizeof (SNAPSHOT_PAGE_ENTRY);
  DataOffset = ALIGN_VALUE (
                 SNAPSHOT_HEADER_SIZE + Pages * sizeof (SNAPSHOT_PAGE_ENTRY),
                 SNAPSHOT_PAGE_SIZE
                 );
  if (DataOffset >= Capacity) {
    return 0;
  }

  Pages = (Capacity - DataOffset) / SNAPSHOT_PAGE_SIZE;
  if (Pages > 0xFFFFFFFFULL) {
    return 0xFFFFFFFFU;
  }

  return (UINT32)Pages;
}

STATIC
SNAPSHOT_SLOT_HEADER *
InitSlot (
  IN UINT64   SlotOffset,
  IN UINT64   Capacity,
  IN UINT64   Sequence,
  IN BOOLEAN  Full,
  IN UINT32   Mode
  )
{
  SNAPSHOT_HEADER       *Header;
  SNAPSHOT_SLOT_HEADER  *Slot;
  UINT32                CapacityPages;

  Header = GetHeader ();
  Slot   = (SNAPSHOT_SLOT_HEADER *)(UINTN)(mConfig.BufferBase + SlotOffset);
  ZeroMem (Slot, SNAPSHOT_HEADER_SIZE);

  CapacityPages = SlotCapacityPages (Capacity);
  Slot->Magic          = SNAPSHOT_SLOT_MAGIC;
  Slot->Version        = SNAPSHOT_VERSION;
  Slot->Mode           = Mode;
  Slot->Sequence       = Sequence;
  Slot->Tick           = Header->Tick;
  Slot->Tsc            = mSmiStartTsc;
  Slot->CapacityPages  = CapacityPages;
  Slot->PageMapOffset  = SNAPSHOT_HEADER_SIZE;
  Slot->DataOffset     = ALIGN_VALUE (
                           SNAPSHOT_HEADER_SIZE +
                           (UINT64)CapacityPages * sizeof (SNAPSHOT_PAGE_ENTRY),
                           SNAPSHOT_PAGE_SIZE
                           );
  Slot->Flags          = SNAPSHOT_SLOT_CAPTURING | (Full ? SNAPSHOT_SLOT_FULL : 0);
  Slot->PageCount      = 0;
  Slot->DataBytes      = 0;
  return Slot;
}

STATIC
BOOLEAN
AddPage (
  IN OUT SNAPSHOT_SLOT_HEADER  *Slot,
  IN     UINT64                Pa,
  IN     UINT32                Flags
  )
{
  SNAPSHOT_PAGE_ENTRY  *Entry;
  UINT32               Pfn;

  if (Slot->PageCount >= Slot->CapacityPages) {
    return FALSE;
  }

  Entry = (SNAPSHOT_PAGE_ENTRY *)((UINT8 *)Slot + Slot->PageMapOffset);
  Entry = &Entry[Slot->PageCount];
  Entry->PhysAddr = Pa;
  Entry->Hash     = HashPage (Pa);
  Entry->Flags    = Flags;

  CopyMem (
    (UINT8 *)Slot + Slot->DataOffset + (UINT64)Slot->PageCount * SNAPSHOT_PAGE_SIZE,
    (VOID *)(UINTN)Pa,
    SNAPSHOT_PAGE_SIZE
    );

  Pfn = (UINT32)(Pa >> 12);
  SetHash (Pfn, Entry->Hash);

  Slot->PageCount++;
  Slot->DataBytes = (UINT64)Slot->PageCount * SNAPSHOT_PAGE_SIZE;
  return TRUE;
}

STATIC
VOID
PublishSlot (
  IN UINT64   SlotOffset,
  IN UINT64   Length,
  IN UINT64   Sequence,
  IN BOOLEAN  Full,
  IN UINT32   ExtraFlags
  )
{
  SNAPSHOT_HEADER       *Header;
  SNAPSHOT_SLOT_HEADER  *Slot;
  SNAPSHOT_SLOT_ENTRY   *Dir;
  UINT32                Index;

  Header = GetHeader ();
  Slot   = (SNAPSHOT_SLOT_HEADER *)(UINTN)(mConfig.BufferBase + SlotOffset);
  Slot->Flags &= ~(UINT32)SNAPSHOT_SLOT_CAPTURING;
  Slot->Flags |= SNAPSHOT_SLOT_COMPLETE | ExtraFlags;
  Slot->SmiTsc = AsmReadTsc () - mSmiStartTsc;

  if (Full) {
    Dir = &Header->Slots[SNAPSHOT_FULL_DIR_INDEX];
    Header->FullSequence = Sequence;
    Header->DeltaWriteIndex = 0;
    mForceFull = FALSE;
    for (Index = 1; Index < SNAPSHOT_MAX_DIR_ENTRIES; Index++) {
      ZeroMem (&Header->Slots[Index], sizeof (SNAPSHOT_SLOT_ENTRY));
    }
  } else {
    if (Header->DeltaWriteIndex >= SNAPSHOT_MAX_DIR_ENTRIES - 1) {
      mForceFull = TRUE;
      return;
    }

    Dir = &Header->Slots[1 + Header->DeltaWriteIndex];
    Header->DeltaWriteIndex++;
    if (Header->DeltaWriteIndex >= Header->DeltaSlotCount) {
      mForceFull = TRUE;
    }
  }

  Dir->Magic      = SNAPSHOT_SLOT_MAGIC;
  Dir->Offset     = SlotOffset;
  Dir->Length     = Length;
  Dir->Sequence   = Sequence;
  Dir->Tick       = Slot->Tick;
  Dir->Tsc        = Slot->Tsc;
  Dir->Mode       = Slot->Mode;
  Dir->Flags      = Slot->Flags;
  Dir->PageCount  = Slot->PageCount;
  Dir->DataBytes  = Slot->DataBytes;

  Header->Sequence = Sequence;
  Header->PagesLast = Slot->PageCount;
  Header->BytesCopied += Slot->DataBytes;
  Header->State = SNAPSHOT_STATE_READY;
}

STATIC
VOID
StartFullCapture (
  VOID
  )
{
  SNAPSHOT_HEADER  *Header;
  SNAPSHOT_SLOT_HEADER *Slot;
  UINT32           CapacityPages;
  UINT64           Sequence;

  Header = GetHeader ();
  CapacityPages = SlotCapacityPages (Header->FullSlotCapacity);
  if (CapacityPages == 0) {
    Header->State = SNAPSHOT_STATE_ERROR;
    return;
  }

  Sequence = Header->Sequence + 1;
  Slot = InitSlot (
           Header->FullSlotOffset,
           Header->FullSlotCapacity,
           Sequence,
           TRUE,
           mConfig.Mode
           );

  // Invalidate the previous base while the new one is being written.
  Header->Slots[SNAPSHOT_FULL_DIR_INDEX].Flags = 0;
  Header->FullSequence = 0;
  Header->Sequence = Sequence;
  Header->State = SNAPSHOT_STATE_CAPTURING;

  mSlice.Active        = TRUE;
  mSlice.Sequence      = Sequence;
  mSlice.SlotOffset    = Header->FullSlotOffset;
  mSlice.SlotCapacity  = Header->FullSlotCapacity;
  mSlice.CapacityPages = CapacityPages;
  mSlice.PagesWritten  = 0;
  mSlice.Cursor.RangeIndex = 0;
  mSlice.Cursor.PageIndex  = 0;
  mSlice.Cursor.Ordinal    = 0;
  (VOID)Slot;
}

STATIC
VOID
ContinueFullCapture (
  IN BOOLEAN  Sliced
  )
{
  SNAPSHOT_SLOT_HEADER  *Slot;
  UINT32                MaxPages;
  UINT32                Added;
  UINT64                Pa;
  BOOLEAN               Done;
  BOOLEAN               Truncated;
  UINT64                Length;

  Slot = (SNAPSHOT_SLOT_HEADER *)(UINTN)(mConfig.BufferBase + mSlice.SlotOffset);

  MaxPages = 0xFFFFFFFFU;
  if (Sliced && (mConfig.MaxPagesPerSmi != 0)) {
    MaxPages = mConfig.MaxPagesPerSmi;
  }

  Added = 0;
  while ((Slot->PageCount < mSlice.CapacityPages) && (Added < MaxPages)) {
    if (!CursorNext (&mSlice.Cursor, &Pa)) {
      break;
    }

    if (!AddPage (
           Slot,
           Pa,
           SNAPSHOT_PAGE_FULL | (Sliced ? SNAPSHOT_PAGE_SLICED : 0)
           ))
    {
      break;
    }

    mSlice.PagesWritten++;
    Added++;
  }

  Done = (BOOLEAN)(mSlice.Cursor.RangeIndex >= mConfig.RangeCount);
  Truncated = (BOOLEAN)(!Done && (Slot->PageCount >= mSlice.CapacityPages));

  if (Done || Truncated) {
    Length = Slot->DataOffset + (UINT64)Slot->PageCount * SNAPSHOT_PAGE_SIZE;
    PublishSlot (
      mSlice.SlotOffset,
      Length,
      mSlice.Sequence,
      TRUE,
      Truncated ? SNAPSHOT_SLOT_TRUNCATED : 0
      );
    mSlice.Active = FALSE;
  }
}

STATIC
SNAPSHOT_SLOT_HEADER *
StartDelta (
  OUT UINT64  *SlotOffset,
  OUT UINT32  *CapacityPages
  )
{
  SNAPSHOT_HEADER       *Header;
  SNAPSHOT_SLOT_HEADER  *Slot;
  UINT64                Offset;
  UINT64                Sequence;
  UINT32                DirIndex;

  Header = GetHeader ();
  if ((Header->DeltaSlotCount == 0) ||
      (Header->DeltaSlotSize < (2 * SNAPSHOT_HEADER_SIZE)))
  {
    mForceFull = TRUE;
    return NULL;
  }

  if (Header->DeltaWriteIndex >= Header->DeltaSlotCount) {
    mForceFull = TRUE;
    return NULL;
  }

  DirIndex = 1 + Header->DeltaWriteIndex;
  if (DirIndex >= SNAPSHOT_MAX_DIR_ENTRIES) {
    mForceFull = TRUE;
    return NULL;
  }

  Offset = Header->DeltaStart +
           (UINT64)Header->DeltaWriteIndex * Header->DeltaSlotSize;
  if ((Offset + Header->DeltaSlotSize) > Header->BufferSize) {
    mForceFull = TRUE;
    return NULL;
  }

  // Invalidate any stale directory entry before overwriting the slot.
  ZeroMem (&Header->Slots[DirIndex], sizeof (SNAPSHOT_SLOT_ENTRY));

  Sequence = Header->Sequence + 1;
  Slot = InitSlot (Offset, Header->DeltaSlotSize, Sequence, FALSE, mConfig.Mode);
  Header->Sequence = Sequence;

  *SlotOffset    = Offset;
  *CapacityPages = Slot->CapacityPages;
  return Slot;
}

STATIC
VOID
PublishDelta (
  IN UINT64                SlotOffset,
  IN SNAPSHOT_SLOT_HEADER  *Slot,
  IN BOOLEAN               Truncated
  )
{
  UINT64  Length;

  Length = Slot->DataOffset + (UINT64)Slot->PageCount * SNAPSHOT_PAGE_SIZE;
  PublishSlot (
    SlotOffset,
    Length,
    Slot->Sequence,
    FALSE,
    Truncated ? SNAPSHOT_SLOT_TRUNCATED : 0
    );
}

STATIC
VOID
DiscardDelta (
  IN SNAPSHOT_SLOT_HEADER  *Slot
  )
{
  Slot->Magic = 0;
}

STATIC
BOOLEAN
ShouldForceFull (
  VOID
  )
{
  SNAPSHOT_HEADER  *Header;

  Header = GetHeader ();
  if (Header->FullSequence == 0) {
    return TRUE;
  }

  if (mForceFull) {
    return TRUE;
  }

  if ((mFullEvery != 0) &&
      (((Header->Sequence + 1) % mFullEvery) == 0))
  {
    return TRUE;
  }

  return FALSE;
}

STATIC
VOID
CaptureModeA (
  VOID
  )
{
  if (mSlice.Active) {
    ContinueFullCapture (FALSE);
    return;
  }

  StartFullCapture ();
  ContinueFullCapture (FALSE);
}

STATIC
VOID
CaptureModeD (
  VOID
  )
{
  SNAPSHOT_HEADER  *Header;

  if (mSlice.Active) {
    ContinueFullCapture (TRUE);
    return;
  }

  // A sliced full snapshot is assembled across several SMIs in the single
  // full slot.  Starting another one would immediately invalidate the last
  // complete image while it is being overwritten, so keep the completed
  // snapshot and idle.  Modes A/B/C provide continuous periodic snapshots;
  // mode D is a bounded-latency acquisition of one consistent full image.
  Header = GetHeader ();
  if ((Header->FullSequence != 0) &&
      (Header->Slots[SNAPSHOT_FULL_DIR_INDEX].Magic == SNAPSHOT_SLOT_MAGIC) &&
      ((Header->Slots[SNAPSHOT_FULL_DIR_INDEX].Flags & SNAPSHOT_SLOT_COMPLETE) != 0))
  {
    return;
  }

  StartFullCapture ();
  ContinueFullCapture (TRUE);
}

STATIC
VOID
CaptureModeB (
  VOID
  )
{
  SNAPSHOT_SLOT_HEADER  *Slot;
  SNAPSHOT_PAGE_CURSOR  Cursor;
  UINT64                SlotOffset;
  UINT32                CapacityPages;
  UINT32                MaxPages;
  UINT32                Added;
  UINT64                Pa;
  UINT32                Pfn;
  UINT32                Hash;
  BOOLEAN               Truncated;

  if (mSlice.Active) {
    ContinueFullCapture (TRUE);
    return;
  }

  if (ShouldForceFull ()) {
    StartFullCapture ();
    ContinueFullCapture (TRUE);
    return;
  }

  Slot = StartDelta (&SlotOffset, &CapacityPages);
  if (Slot == NULL) {
    StartFullCapture ();
    ContinueFullCapture (TRUE);
    return;
  }

  MaxPages = (mConfig.MaxPagesPerSmi != 0) ? mConfig.MaxPagesPerSmi : 0xFFFFFFFFU;
  Added    = 0;
  Truncated = FALSE;
  Cursor.RangeIndex = 0;
  Cursor.PageIndex  = 0;
  Cursor.Ordinal    = 0;

  while (CursorNext (&Cursor, &Pa)) {
    Pfn  = (UINT32)(Pa >> 12);
    Hash = HashPage (Pa);
    if (HashMatches (Pfn, Hash)) {
      continue;
    }

    if ((Added >= MaxPages) || (Slot->PageCount >= CapacityPages)) {
      Truncated = TRUE;
      break;
    }

    if (!AddPage (Slot, Pa, SNAPSHOT_PAGE_DELTA)) {
      Truncated = TRUE;
      break;
    }

    Added++;
  }

  if (Slot->PageCount == 0) {
    DiscardDelta (Slot);
    return;
  }

  PublishDelta (SlotOffset, Slot, Truncated);
}

STATIC
VOID
AddDirtyPage (
  IN OUT SNAPSHOT_DIRTY_CTX  *Ctx,
  IN     UINT64              Pa
  )
{
  UINT32  Pfn;
  UINT32  Hash;

  if (!IsValidPa (Pa)) {
    return;
  }

  Pfn = (UINT32)(Pa >> 12);
  if (mSeenGen[Pfn] == mSeenGeneration) {
    return;
  }

  mSeenGen[Pfn] = mSeenGeneration;

  Hash = HashPage (Pa);
  if (HashMatches (Pfn, Hash)) {
    return;
  }

  if ((Ctx->Added >= Ctx->Max) || (Ctx->Slot->PageCount >= Ctx->CapPages)) {
    Ctx->Truncated = TRUE;
    return;
  }

  if (!AddPage (
         Ctx->Slot,
         Pa,
         SNAPSHOT_PAGE_DELTA | SNAPSHOT_PAGE_DIRTY
         ))
  {
    Ctx->Truncated = TRUE;
    return;
  }

  Ctx->Added++;
}

STATIC
VOID
AddHugePage (
  IN OUT SNAPSHOT_DIRTY_CTX  *Ctx,
  IN     UINT64              Base,
  IN     UINT64              Size
  )
{
  UINT64  Offset;

  for (Offset = 0; Offset < Size; Offset += SNAPSHOT_PAGE_SIZE) {
    AddDirtyPage (Ctx, Base + Offset);
    if (Ctx->Truncated) {
      return;
    }
  }
}

STATIC
VOID
WalkPageTables (
  IN OUT SNAPSHOT_DIRTY_CTX  *Ctx,
  IN     UINT64              Cr3
  )
{
  UINT64  Pml4Base;
  UINT64  PdptBase;
  UINT64  PdBase;
  UINT64  PtBase;
  UINT64  Entry;
  UINTN   I;
  UINTN   J;
  UINTN   K;
  UINTN   L;

  Pml4Base = Cr3 & SNAPSHOT_PAGE_MASK;
  if (!IsValidPa (Pml4Base)) {
    return;
  }

  for (I = 0; I < 512; I++) {
    Entry = ReadPhys64 (Pml4Base + I * sizeof (UINT64));
    if ((Entry & SNAPSHOT_PTE_PRESENT) == 0) {
      continue;
    }

    PdptBase = Entry & SNAPSHOT_PAGE_MASK;
    if (!IsValidPa (PdptBase)) {
      continue;
    }

    for (J = 0; J < 512; J++) {
      Entry = ReadPhys64 (PdptBase + J * sizeof (UINT64));
      if ((Entry & SNAPSHOT_PTE_PRESENT) == 0) {
        continue;
      }

      if ((Entry & SNAPSHOT_PTE_PS) != 0) {
        if ((Entry & SNAPSHOT_PTE_DIRTY) != 0) {
          AddHugePage (
            Ctx,
            Entry & SNAPSHOT_PDPTE_1G,
            SIZE_1GB
            );
        }

        continue;
      }

      PdBase = Entry & SNAPSHOT_PAGE_MASK;
      if (!IsValidPa (PdBase)) {
        continue;
      }

      for (K = 0; K < 512; K++) {
        Entry = ReadPhys64 (PdBase + K * sizeof (UINT64));
        if ((Entry & SNAPSHOT_PTE_PRESENT) == 0) {
          continue;
        }

        if ((Entry & SNAPSHOT_PTE_PS) != 0) {
          if ((Entry & SNAPSHOT_PTE_DIRTY) != 0) {
            AddHugePage (
              Ctx,
              Entry & SNAPSHOT_PDE_2M,
              SIZE_2MB
              );
          }

          continue;
        }

        PtBase = Entry & SNAPSHOT_PAGE_MASK;
        if (!IsValidPa (PtBase)) {
          continue;
        }

        for (L = 0; L < 512; L++) {
          Entry = ReadPhys64 (PtBase + L * sizeof (UINT64));
          if (((Entry & SNAPSHOT_PTE_PRESENT) == 0) ||
              ((Entry & SNAPSHOT_PTE_DIRTY) == 0))
          {
            continue;
          }

          AddDirtyPage (Ctx, Entry & SNAPSHOT_PAGE_MASK);
          if (Ctx->Truncated) {
            return;
          }
        }
      }
    }
  }
}

STATIC
VOID
CaptureModeC (
  VOID
  )
{
  SNAPSHOT_SLOT_HEADER  *Slot;
  SNAPSHOT_DIRTY_CTX    Ctx;
  UINT64                SlotOffset;
  UINT32                CapacityPages;
  UINT64                Cr3;
  UINTN                 Cpu;
  BOOLEAN               AnyCpu;

  if (mCpuProtocol == NULL) {
    // No CPU save-state protocol: fall back to hash-based incremental mode.
    CaptureModeB ();
    return;
  }

  if (mSlice.Active) {
    ContinueFullCapture (TRUE);
    return;
  }

  if (ShouldForceFull ()) {
    StartFullCapture ();
    ContinueFullCapture (TRUE);
    return;
  }

  Slot = StartDelta (&SlotOffset, &CapacityPages);
  if (Slot == NULL) {
    StartFullCapture ();
    ContinueFullCapture (TRUE);
    return;
  }

  mSeenGeneration++;
  if (mSeenGeneration == 0) {
    ZeroMem (mSeenGen, (UINTN)mMaxPfn * sizeof (UINT32));
    mSeenGeneration = 1;
  }

  Ctx.Slot      = Slot;
  Ctx.Added     = 0;
  Ctx.Max       = (mConfig.MaxPagesPerSmi != 0) ? mConfig.MaxPagesPerSmi : 0xFFFFFFFFU;
  Ctx.CapPages  = CapacityPages;
  Ctx.Truncated = FALSE;

  AnyCpu = FALSE;
  for (Cpu = 0; Cpu < gSmst->NumberOfCpus; Cpu++) {
    Cr3 = 0;
    if (EFI_ERROR (
          mCpuProtocol->ReadSaveState (
                          mCpuProtocol,
                          sizeof (UINT64),
                          EFI_SMM_SAVE_STATE_REGISTER_CR3,
                          Cpu,
                          &Cr3
                          )
          ))
    {
      continue;
    }

    if ((Cr3 == 0) || ((Cr3 & SNAPSHOT_PAGE_MASK) == 0)) {
      continue;
    }

    AnyCpu = TRUE;
    WalkPageTables (&Ctx, Cr3);
    if (Ctx.Truncated) {
      break;
    }
  }

  if (!AnyCpu) {
    DiscardDelta (Slot);
    return;
  }

  if (Slot->PageCount == 0) {
    DiscardDelta (Slot);
    return;
  }

  PublishDelta (SlotOffset, Slot, Ctx.Truncated);
}

STATIC
EFI_STATUS
EFIAPI
SnapshotTimerHandler (
  IN     EFI_HANDLE  DispatchHandle,
  IN     CONST VOID  *Context,
  IN OUT VOID        *CommBuffer,
  IN OUT UINTN       *CommBufferSize
  )
{
  SNAPSHOT_HEADER  *Header;
  UINT64           StartTsc;
  UINT64           EndTsc;
  UINT64           Delta;

  if (!mConfigured) {
    return EFI_SUCCESS;
  }

  Header = GetHeader ();
  StartTsc = AsmReadTsc ();

  // Derive the TSC frequency from consecutive periodic ticks.  QEMU's ICH9
  // periodic timer runs on the virtual clock, so the TSC delta between two
  // ticks divided by the configured period is a good estimate.
  if ((mLastTickTsc != 0) && (StartTsc > mLastTickTsc) &&
      (mConfig.PeriodSeconds != 0))
  {
    Header->TscHz = (StartTsc - mLastTickTsc) / mConfig.PeriodSeconds;
  }

  mLastTickTsc = StartTsc;
  mSmiStartTsc = StartTsc;

  Header->Tick++;
  Header->SmiCount++;

  switch (mConfig.Mode) {
    case SNAPSHOT_MODE_A:
      CaptureModeA ();
      break;
    case SNAPSHOT_MODE_B:
      CaptureModeB ();
      break;
    case SNAPSHOT_MODE_C:
      CaptureModeC ();
      break;
    case SNAPSHOT_MODE_D:
      CaptureModeD ();
      break;
    default:
      break;
  }

  EndTsc = AsmReadTsc ();
  Delta  = EndTsc - StartTsc;
  Header->SmiTscTotal += Delta;
  Header->SmiTscLast   = Delta;
  if (Delta > Header->SmiTscMax) {
    Header->SmiTscMax = Delta;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
ValidateConfig (
  IN SNAPSHOT_CONFIG  *Config
  )
{
  UINT32  Index;
  UINT64  BufferEnd;
  UINT64  RangeEnd;

  if ((Config->Signature != SNAPSHOT_CONFIG_MAGIC) ||
      (Config->Version != SNAPSHOT_VERSION))
  {
    return EFI_INVALID_PARAMETER;
  }

  if (Config->Mode > SNAPSHOT_MODE_D) {
    return EFI_INVALID_PARAMETER;
  }

  if ((Config->BufferBase & (SNAPSHOT_PAGE_SIZE - 1)) != 0) {
    return EFI_INVALID_PARAMETER;
  }

  if (Config->BufferSize < (64 * 1024)) {
    return EFI_INVALID_PARAMETER;
  }

  BufferEnd = Config->BufferBase + Config->BufferSize;
  if ((BufferEnd < Config->BufferBase) || (BufferEnd > SIZE_4GB)) {
    return EFI_INVALID_PARAMETER;
  }

  if ((Config->GuestRamTop == 0) || (BufferEnd > Config->GuestRamTop)) {
    return EFI_INVALID_PARAMETER;
  }

  if ((Config->RangeCount == 0) ||
      (Config->RangeCount > SNAPSHOT_MAX_RANGES) ||
      (Config->RangeEntrySize != sizeof (SNAPSHOT_MEM_RANGE)))
  {
    return EFI_INVALID_PARAMETER;
  }

  for (Index = 0; Index < Config->RangeCount; Index++) {
    SNAPSHOT_MEM_RANGE  *Range;

    Range = &Config->Ranges[Index];
    if ((Range->Base & (SNAPSHOT_PAGE_SIZE - 1)) != 0) {
      return EFI_INVALID_PARAMETER;
    }

    if ((Range->Length == 0) ||
        ((Range->Length & (SNAPSHOT_PAGE_SIZE - 1)) != 0))
    {
      return EFI_INVALID_PARAMETER;
    }

    RangeEnd = Range->Base + Range->Length;
    if ((RangeEnd < Range->Base) || (RangeEnd > Config->GuestRamTop)) {
      return EFI_INVALID_PARAMETER;
    }

    if (Index > 0) {
      SNAPSHOT_MEM_RANGE  *Prev;

      Prev = &Config->Ranges[Index - 1];
      if (Range->Base < (Prev->Base + Prev->Length)) {
        return EFI_INVALID_PARAMETER;
      }
    }

    // The destination must not overlap a source range.
    if ((Range->Base < BufferEnd) && (RangeEnd > Config->BufferBase)) {
      return EFI_INVALID_PARAMETER;
    }
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
AllocateTracking (
  VOID
  )
{
  UINT32  Index;
  UINT64  Pfn;
  UINT64  PfnEnd;

  mMaxPfn = (UINT32)(mConfig.GuestRamTop >> 12);
  if (mMaxPfn > SNAPSHOT_MAX_PFN) {
    mMaxPfn = SNAPSHOT_MAX_PFN;
  }

  if (mMaxPfn == 0) {
    return EFI_INVALID_PARAMETER;
  }

  mHash = AllocateZeroPool ((UINTN)mMaxPfn * sizeof (UINT32));
  if (mHash == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  mHashValid = AllocateZeroPool ((UINTN)mMaxPfn / 8 + 1);
  if (mHashValid == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  mValidPfn = AllocateZeroPool ((UINTN)mMaxPfn / 8 + 1);
  if (mValidPfn == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  mSeenGen = AllocateZeroPool ((UINTN)mMaxPfn * sizeof (UINT32));
  if (mSeenGen == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  mPageCount = 0;
  for (Index = 0; Index < mConfig.RangeCount; Index++) {
    Pfn    = mConfig.Ranges[Index].Base >> 12;
    PfnEnd = (mConfig.Ranges[Index].Base + mConfig.Ranges[Index].Length) >> 12;
    for (; Pfn < PfnEnd; Pfn++) {
      mValidPfn[Pfn >> 3] |= (UINT8)(1U << (Pfn & 7));
      mPageCount++;
    }
  }

  mSeenGeneration = 1;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
InitializeHeader (
  VOID
  )
{
  SNAPSHOT_HEADER  *Header;
  UINT64           DataStartOffset;
  UINT64           Available;
  UINT64           FullNeeded;
  UINT64           FullCapacity;
  UINT64           DeltaSpace;
  UINT64           DeltaSlotSize;
  UINT32           DeltaCount;
  UINT64           MinDelta;

  Header = GetHeader ();

  DataStartOffset = ALIGN_VALUE (
                      SNAPSHOT_HEADER_SIZE +
                      (UINT64)mConfig.RangeCount * sizeof (SNAPSHOT_MEM_RANGE),
                      SNAPSHOT_PAGE_SIZE
                      );
  if (DataStartOffset >= mConfig.BufferSize) {
    return EFI_BUFFER_TOO_SMALL;
  }

  Available = mConfig.BufferSize - DataStartOffset;
  if (Available < (4 * SNAPSHOT_PAGE_SIZE)) {
    return EFI_BUFFER_TOO_SMALL;
  }

  // One extra page absorbs the page-map alignment padding.
  FullNeeded = ALIGN_VALUE (
                 SNAPSHOT_HEADER_SIZE +
                 (UINT64)mPageCount * sizeof (SNAPSHOT_PAGE_ENTRY) +
                 (UINT64)mPageCount * SNAPSHOT_PAGE_SIZE,
                 SNAPSHOT_PAGE_SIZE
                 ) + SNAPSHOT_PAGE_SIZE;

  DeltaCount    = 0;
  DeltaSlotSize = 0;
  DeltaSpace    = 0;
  FullCapacity  = Available;

  if ((mConfig.Mode == SNAPSHOT_MODE_B) ||
      (mConfig.Mode == SNAPSHOT_MODE_C))
  {
    MinDelta = 64 * 1024;
    if (Available > (FullNeeded + MinDelta)) {
      FullCapacity = FullNeeded;
      DeltaSpace   = Available - FullCapacity;
    } else {
      FullCapacity = Available - MinDelta;
      DeltaSpace   = MinDelta;
    }

    if (FullCapacity < (2 * SNAPSHOT_PAGE_SIZE)) {
      FullCapacity = Available;
      DeltaSpace   = 0;
    }

    DeltaCount = PcdGet32 (PcdSnapshotDeltaSlots);
    if (DeltaCount > (SNAPSHOT_MAX_DIR_ENTRIES - 2)) {
      DeltaCount = SNAPSHOT_MAX_DIR_ENTRIES - 2;
    }

    if ((DeltaCount == 0) && (DeltaSpace >= (2 * SNAPSHOT_PAGE_SIZE))) {
      DeltaCount = 1;
    }

    if (DeltaCount != 0) {
      DeltaSlotSize = (DeltaSpace / DeltaCount) &
                      ~(UINT64)(SNAPSHOT_PAGE_SIZE - 1);
      if (DeltaSlotSize < (2 * SNAPSHOT_PAGE_SIZE)) {
        DeltaCount = (UINT32)(DeltaSpace / (2 * SNAPSHOT_PAGE_SIZE));
        if (DeltaCount == 0) {
          DeltaCount = 1;
        }

        DeltaSlotSize = (DeltaSpace / DeltaCount) &
                        ~(UINT64)(SNAPSHOT_PAGE_SIZE - 1);
      }
    }
  }

  ZeroMem (Header, SNAPSHOT_HEADER_SIZE);
  Header->Magic           = SNAPSHOT_HEADER_MAGIC;
  Header->Version         = SNAPSHOT_VERSION;
  Header->HeaderSize      = SNAPSHOT_HEADER_SIZE;
  Header->Mode            = mConfig.Mode;
  Header->State           = SNAPSHOT_STATE_READY;
  Header->BufferBase      = mConfig.BufferBase;
  Header->BufferSize      = mConfig.BufferSize;
  Header->DataStart       = mConfig.BufferBase + DataStartOffset;
  Header->DataEnd         = mConfig.BufferBase + mConfig.BufferSize;
  Header->FullSlotOffset  = DataStartOffset;
  Header->FullSlotCapacity = FullCapacity;
  Header->DeltaStart      = DataStartOffset + FullCapacity;
  Header->DeltaSlotSize   = DeltaSlotSize;
  Header->DeltaSlotCount  = DeltaCount;
  Header->DeltaWriteIndex = 0;
  Header->Sequence        = 0;
  Header->FullSequence    = 0;
  Header->Tick            = 0;
  Header->TscHz           = mConfig.TscHz;
  Header->SmiCount        = 0;
  Header->SmiTscTotal     = 0;
  Header->SmiTscMax       = 0;
  Header->SmiTscLast      = 0;
  Header->PagesTotal      = mPageCount;
  Header->PagesLast       = 0;
  Header->BytesCopied     = 0;
  Header->GuestRamTop     = mConfig.GuestRamTop;
  Header->RangeCount      = mConfig.RangeCount;
  Header->RangeEntrySize  = sizeof (SNAPSHOT_MEM_RANGE);
  Header->RangesOffset    = SNAPSHOT_HEADER_SIZE;
  Header->Flags           = (UINT32)mConfig.Flags;
  Header->Reserved[0]     = mConfig.PeriodSeconds;
  Header->State           = SNAPSHOT_STATE_READY;

  CopyMem (
    (UINT8 *)(UINTN)mConfig.BufferBase + SNAPSHOT_HEADER_SIZE,
    mConfig.Ranges,
    (UINTN)mConfig.RangeCount * sizeof (SNAPSHOT_MEM_RANGE)
    );

  mForceFull = TRUE;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
SnapshotConfigHandler (
  IN     EFI_HANDLE  DispatchHandle,
  IN     CONST VOID  *Context,
  IN OUT VOID        *CommBuffer,
  IN OUT UINTN       *CommBufferSize
  )
{
  SNAPSHOT_CONFIG  *Config;
  EFI_STATUS       Status;

  if (mConfigLocked) {
    return EFI_ACCESS_DENIED;
  }

  if ((CommBuffer == NULL) || (CommBufferSize == NULL) ||
      (*CommBufferSize < sizeof (SNAPSHOT_CONFIG)))
  {
    return EFI_INVALID_PARAMETER;
  }

  Config = (SNAPSHOT_CONFIG *)CommBuffer;
  Status = ValidateConfig (Config);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "SmmSnapshot: invalid config: %r\n", Status));
    return Status;
  }

  CopyMem (&mConfig, Config, sizeof (mConfig));

  Status = AllocateTracking ();
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "SmmSnapshot: tracking allocation: %r\n", Status));
    return Status;
  }

  Status = InitializeHeader ();
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "SmmSnapshot: header init: %r\n", Status));
    return Status;
  }

  mFullEvery   = PcdGet32 (PcdSnapshotFullEvery);
  mConfigured  = TRUE;
  mConfigLocked = TRUE;

  DEBUG ((
    DEBUG_INFO,
    "SmmSnapshot: configured mode=%u base=0x%lx size=0x%lx pages=%u ranges=%u\n",
    mConfig.Mode,
    mConfig.BufferBase,
    mConfig.BufferSize,
    mPageCount,
    mConfig.RangeCount
    ));

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
SmmSnapshotEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS                                  Status;
  EFI_SMM_PERIODIC_TIMER_REGISTER_CONTEXT     TimerContext;
  UINT32                                      PeriodSeconds;

  if (!PcdGetBool (PcdSnapshotEnable)) {
    return EFI_SUCCESS;
  }

  mCpuProtocol = NULL;
  Status = gSmst->SmmLocateProtocol (
                    &gEfiSmmCpuProtocolGuid,
                    NULL,
                    (VOID **)&mCpuProtocol
                    );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "SmmSnapshot: no SMM CPU protocol: %r\n", Status));
    mCpuProtocol = NULL;
  }

  mTimerProtocol = NULL;
  Status = gSmst->SmmLocateProtocol (
                    &gEfiSmmPeriodicTimerDispatch2ProtocolGuid,
                    NULL,
                    (VOID **)&mTimerProtocol
                    );
  if (EFI_ERROR (Status) || (mTimerProtocol == NULL)) {
    DEBUG ((DEBUG_ERROR, "SmmSnapshot: no periodic timer protocol: %r\n", Status));
    return EFI_NOT_FOUND;
  }

  PeriodSeconds = PcdGet32 (PcdSnapshotPeriodSeconds);
  if (PeriodSeconds < 8) {
    PeriodSeconds = 8;
  }

  TimerContext.Period = (UINT64)PeriodSeconds * 10000000ULL; // 100 ns units
  TimerContext.SmiTickInterval = 0;
  Status = mTimerProtocol->Register (
                             mTimerProtocol,
                             SnapshotTimerHandler,
                             &TimerContext,
                             &mTimerHandle
                             );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "SmmSnapshot: timer register: %r\n", Status));
    return Status;
  }

  mConfigHandle = NULL;
  Status = gSmst->SmiHandlerRegister (
                    SnapshotConfigHandler,
                    &gSnapshotConfigGuid,
                    &mConfigHandle
                    );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "SmmSnapshot: config handler register: %r\n", Status));
    return Status;
  }

  DEBUG ((DEBUG_INFO, "SmmSnapshot: ready, requested period=%u s\n", PeriodSeconds));
  return EFI_SUCCESS;
}
