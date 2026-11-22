/** @file
  SnapshotDxe - reserve a physical memory ring buffer and hand its layout to
  SmmSnapshotDriver over EFI_SMM_COMMUNICATION_PROTOCOL.

  This driver runs in normal DXE context before ExitBootServices().  It never
  touches the buffer after the configuration message has been accepted; all
  snapshot writes happen in SMM.

  Timing: SMM drivers are only dispatched after the DXE dispatcher has
  finished its work (PiSmmIpl listens to gEfiEventDxeDispatchGuid).  A plain
  DXE driver therefore cannot talk to SmmSnapshotDriver from its entry point.
  The buffer and the source ranges are prepared in the entry point, and the
  configuration message is sent from a protocol notification on
  gEfiDxeSmmReadyToLockProtocolGuid, which is installed after SMM dispatch and
  before boot options are processed.  The SMM side locks the configuration on
  first acceptance.

  Copyright (c) 2026
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Snapshot.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/DxeSmmReadyToLock.h>
#include <Protocol/SmmCommunication.h>

#define SNAPSHOT_MAX_BUFFER_FRACTION_NUM   11
#define SNAPSHOT_MAX_BUFFER_FRACTION_DEN   20

STATIC SNAPSHOT_CONFIG  mConfig;
STATIC BOOLEAN          mConfigured;
STATIC VOID             *mCommBuffer;
STATIC UINT64           mTotalPages;

STATIC
BOOLEAN
IsCapturableType (
  IN EFI_MEMORY_TYPE  Type
  )
{
  if (Type == EfiConventionalMemory) {
    return TRUE;
  }

  if (PcdGetBool (PcdSnapshotIncludeReclaimable)) {
    if ((Type == EfiLoaderCode) || (Type == EfiLoaderData) ||
        (Type == EfiBootServicesCode) || (Type == EfiBootServicesData)) {
      return TRUE;
    }
  }

  return FALSE;
}

STATIC
BOOLEAN
IsMmioType (
  IN EFI_MEMORY_TYPE  Type
  )
{
  return (BOOLEAN)((Type == EfiMemoryMappedIO) ||
                   (Type == EfiMemoryMappedIOPortSpace) ||
                   (Type == EfiUnusableMemory) ||
                   (Type == EfiPalCode));
}

STATIC
VOID
SortAndMergeRanges (
  IN OUT SNAPSHOT_MEM_RANGE  *Ranges,
  IN OUT UINT32              *RangeCount
  )
{
  UINT32              Index;
  UINT32              Inner;
  SNAPSHOT_MEM_RANGE  Temp;
  UINT32              Count;
  UINT32              Out;

  Count = *RangeCount;

  for (Index = 1; Index < Count; Index++) {
    Temp = Ranges[Index];
    Inner = Index;
    while ((Inner > 0) && (Ranges[Inner - 1].Base > Temp.Base)) {
      Ranges[Inner] = Ranges[Inner - 1];
      Inner--;
    }

    Ranges[Inner] = Temp;
  }

  Out = 0;
  for (Index = 0; Index < Count; Index++) {
    if (Ranges[Index].Length == 0) {
      continue;
    }

    if ((Out > 0) &&
        (Ranges[Index].Base <= (Ranges[Out - 1].Base + Ranges[Out - 1].Length)))
    {
      UINT64  End;
      UINT64  PrevEnd;

      End     = Ranges[Index].Base + Ranges[Index].Length;
      PrevEnd = Ranges[Out - 1].Base + Ranges[Out - 1].Length;
      if (End > PrevEnd) {
        Ranges[Out - 1].Length = End - Ranges[Out - 1].Base;
      }
    } else {
      Ranges[Out] = Ranges[Index];
      Out++;
    }
  }

  *RangeCount = Out;
}

STATIC
VOID
AddRange (
  IN OUT SNAPSHOT_MEM_RANGE  *Ranges,
  IN OUT UINT32              *RangeCount,
  IN     UINT64              Base,
  IN     UINT64              End,
  IN     UINT32              Type
  )
{
  UINT64  AlignedBase;
  UINT64  AlignedEnd;

  if (End <= Base) {
    return;
  }

  AlignedBase = ALIGN_VALUE (Base, SNAPSHOT_PAGE_SIZE);
  AlignedEnd  = End & ~(UINT64)(SNAPSHOT_PAGE_SIZE - 1);
  if (AlignedEnd <= AlignedBase) {
    return;
  }

  if (*RangeCount >= SNAPSHOT_MAX_RANGES) {
    return;
  }

  Ranges[*RangeCount].Base   = AlignedBase;
  Ranges[*RangeCount].Length = AlignedEnd - AlignedBase;
  Ranges[*RangeCount].Type   = Type;
  Ranges[*RangeCount].Flags  = 0;
  (*RangeCount)++;
}

/**
  Collect capturable ranges, trimming the reserved snapshot buffer and any
  range above 4 GiB (SMM runs with a 4 GiB-visible identity map in this
  prototype).  GuestRamTop is the top of actual RAM, not of the PCI MMIO
  window: the SMM tracking arrays are sized from it and SMRAM is small.
**/
STATIC
EFI_STATUS
BuildRanges (
  IN  EFI_MEMORY_DESCRIPTOR  *Map,
  IN  UINTN                  MapSize,
  IN  UINTN                  DescSize,
  IN  UINT64                 BufferBase,
  IN  UINT64                 BufferSize,
  OUT SNAPSHOT_MEM_RANGE     *Ranges,
  OUT UINT32                 *RangeCount,
  OUT UINT64                 *GuestRamTop,
  OUT UINT64                 *TotalRamBytes
  )
{
  UINTN                  Offset;
  EFI_MEMORY_DESCRIPTOR  *Desc;
  UINT64                 Base;
  UINT64                 End;
  UINT64                 BufferEnd;
  UINT32                 Count;
  UINT64                 RamTop;
  UINT64                 TotalRam;

  Count     = 0;
  RamTop    = 0;
  TotalRam  = 0;
  BufferEnd = BufferBase + BufferSize;

  for (Offset = 0; Offset < MapSize; Offset += DescSize) {
    Desc = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)Map + Offset);
    Base = Desc->PhysicalStart;
    End  = Base + MultU64x64 (Desc->NumberOfPages, SNAPSHOT_PAGE_SIZE);

    if (Base >= SIZE_4GB) {
      continue;
    }

    if (End > SIZE_4GB) {
      End = SIZE_4GB;
    }

    if (IsMmioType ((EFI_MEMORY_TYPE)Desc->Type)) {
      // The PCI MMIO window and other holes are not guest RAM; do not let
      // them inflate GuestRamTop.
      continue;
    }

    if (!IsCapturableType ((EFI_MEMORY_TYPE)Desc->Type)) {
      continue;
    }

    if (End > RamTop) {
      RamTop = End;
    }

    TotalRam += End - Base;

    // Trim the reserved snapshot buffer out of the source ranges.
    if ((Base < BufferEnd) && (End > BufferBase)) {
      if (Base < BufferBase) {
        AddRange (Ranges, &Count, Base, BufferBase, Desc->Type);
      }

      if (End > BufferEnd) {
        AddRange (Ranges, &Count, BufferEnd, End, Desc->Type);
      }

      continue;
    }

    AddRange (Ranges, &Count, Base, End, Desc->Type);
  }

  SortAndMergeRanges (Ranges, &Count);

  *RangeCount    = Count;
  *GuestRamTop   = RamTop;
  *TotalRamBytes = TotalRam;

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
SendConfig (
  IN EFI_SMM_COMMUNICATION_PROTOCOL  *SmmCommunication
  )
{
  EFI_STATUS                 Status;
  EFI_SMM_COMMUNICATE_HEADER *Header;
  UINTN                      HeaderSize;
  UINTN                      CommSize;
  UINTN                      Retry;

  HeaderSize = OFFSET_OF (EFI_SMM_COMMUNICATE_HEADER, Data);
  Header     = (EFI_SMM_COMMUNICATE_HEADER *)mCommBuffer;

  Header->HeaderGuid    = gSnapshotConfigGuid;
  Header->MessageLength = sizeof (SNAPSHOT_CONFIG);
  CopyMem ((UINT8 *)mCommBuffer + HeaderSize, &mConfig, sizeof (SNAPSHOT_CONFIG));

  Status = EFI_NOT_READY;
  for (Retry = 0; Retry < 100; Retry++) {
    CommSize = HeaderSize + sizeof (SNAPSHOT_CONFIG);
    Status   = SmmCommunication->Communicate (
                                  SmmCommunication,
                                  mCommBuffer,
                                  &CommSize
                                  );
    if (!EFI_ERROR (Status)) {
      break;
    }

    // The SMM driver should already be dispatched at ReadyToLock; keep a
    // short back-off in case the SMM core is still finishing its dispatch.
    gBS->Stall (10 * 1000);
  }

  return Status;
}

STATIC
VOID
EFIAPI
SnapshotReadyToLockNotify (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_STATUS                      Status;
  EFI_SMM_COMMUNICATION_PROTOCOL  *SmmCommunication;

  if (mConfigured) {
    gBS->CloseEvent (Event);
    return;
  }

  Status = gBS->LocateProtocol (
                  &gEfiSmmCommunicationProtocolGuid,
                  NULL,
                  (VOID **)&SmmCommunication
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "SnapshotDxe: LocateProtocol(SmmCommunication): %r\n", Status));
    return;
  }

  Status = SendConfig (SmmCommunication);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "SnapshotDxe: SMM config rejected: %r\n", Status));
    return;
  }

  mConfigured = TRUE;
  gBS->CloseEvent (Event);

  DEBUG ((
    DEBUG_INFO,
    "SNAPSHOT_CFG base=0x%016lx size=0x%016lx mode=%u ranges=%u pages=%lu "
    "ramtop=0x%016lx reclaim=%u period=%u maxpages=%u\n",
    mConfig.BufferBase,
    mConfig.BufferSize,
    mConfig.Mode,
    mConfig.RangeCount,
    mTotalPages,
    mConfig.GuestRamTop,
    (UINT32)PcdGetBool (PcdSnapshotIncludeReclaimable),
    mConfig.PeriodSeconds,
    mConfig.MaxPagesPerSmi
    ));

  if (gST->ConOut != NULL) {
    Print (
      L"SNAPSHOT_CFG base=0x%016lx size=0x%016lx mode=%u ranges=%u pages=%lu "
      L"ramtop=0x%016lx reclaim=%u period=%u maxpages=%u\n",
      mConfig.BufferBase,
      mConfig.BufferSize,
      mConfig.Mode,
      mConfig.RangeCount,
      mTotalPages,
      mConfig.GuestRamTop,
      (UINT32)PcdGetBool (PcdSnapshotIncludeReclaimable),
      mConfig.PeriodSeconds,
      mConfig.MaxPagesPerSmi
      );
  }

  for (UINT32 Index = 0; Index < mConfig.RangeCount; Index++) {
    DEBUG ((
      DEBUG_INFO,
      "SnapshotDxe: range[%u] 0x%016lx-0x%016lx type=%u\n",
      Index,
      mConfig.Ranges[Index].Base,
      mConfig.Ranges[Index].Base + mConfig.Ranges[Index].Length,
      mConfig.Ranges[Index].Type
      ));
  }
}

EFI_STATUS
EFIAPI
SnapshotDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS             Status;
  EFI_MEMORY_DESCRIPTOR  *Map;
  UINTN                  MapSize;
  UINTN                  MapKey;
  UINTN                  DescSize;
  UINT32                 DescVersion;
  UINT64                 TotalRam;
  UINT64                 GuestRamTop;
  UINT64                 Requested;
  UINT64                 MaxBuffer;
  UINT64                 BufferSize;
  EFI_PHYSICAL_ADDRESS   BufferBase;
  UINTN                  BufferPages;
  SNAPSHOT_MEM_RANGE     *Ranges;
  UINT32                 RangeCount;
  EFI_EVENT              Event;
  VOID                   *Registration;
  UINTN                  HeaderSize;

  if (!PcdGetBool (PcdSnapshotEnable)) {
    return EFI_SUCCESS;
  }

  Ranges = AllocateZeroPool (sizeof (SNAPSHOT_MEM_RANGE) * SNAPSHOT_MAX_RANGES);
  if (Ranges == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  MapSize = 0;
  Map     = NULL;
  Status  = gBS->GetMemoryMap (&MapSize, Map, &MapKey, &DescSize, &DescVersion);
  if (Status != EFI_BUFFER_TOO_SMALL) {
    DEBUG ((DEBUG_ERROR, "SnapshotDxe: initial GetMemoryMap: %r\n", Status));
    FreePool (Ranges);
    return Status;
  }

  MapSize += 2 * DescSize;
  Map      = AllocatePool (MapSize);
  if (Map == NULL) {
    FreePool (Ranges);
    return EFI_OUT_OF_RESOURCES;
  }

  Status = gBS->GetMemoryMap (&MapSize, Map, &MapKey, &DescSize, &DescVersion);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "SnapshotDxe: GetMemoryMap: %r\n", Status));
    FreePool (Map);
    FreePool (Ranges);
    return Status;
  }

  // Determine total RAM and choose a buffer that still leaves the guest
  // enough memory for Linux (at most 55% of usable RAM).
  RangeCount = 0;
  BuildRanges (Map, MapSize, DescSize, 0, 0, Ranges, &RangeCount, &GuestRamTop, &TotalRam);

  Requested = PcdGet64 (PcdSnapshotBufferSize);
  MaxBuffer = MultU64x32 (
                DivU64x32 (TotalRam, SNAPSHOT_MAX_BUFFER_FRACTION_DEN),
                SNAPSHOT_MAX_BUFFER_FRACTION_NUM
                );
  if ((Requested == 0) || (Requested > MaxBuffer)) {
    Requested = MaxBuffer;
  }

  BufferSize  = ALIGN_VALUE (Requested, SNAPSHOT_PAGE_SIZE);
  BufferPages = (UINTN)(BufferSize / SNAPSHOT_PAGE_SIZE);
  if (BufferPages == 0) {
    FreePool (Map);
    FreePool (Ranges);
    return EFI_OUT_OF_RESOURCES;
  }

  BufferBase = 0xFFFFFFFFULL; // allocate below 4 GiB for the SMM identity map
  Status     = gBS->AllocatePages (
                      AllocateMaxAddress,
                      EfiReservedMemoryType,
                      BufferPages,
                      &BufferBase
                      );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "SnapshotDxe: AllocatePages(%lu): %r\n", BufferPages, Status));
    FreePool (Map);
    FreePool (Ranges);
    return Status;
  }

  ZeroMem ((VOID *)(UINTN)BufferBase, SNAPSHOT_HEADER_SIZE);

  // Re-read the map after the reservation so the ranges exclude our buffer.
  MapSize = 0;
  Status  = gBS->GetMemoryMap (&MapSize, Map, &MapKey, &DescSize, &DescVersion);
  if (Status == EFI_BUFFER_TOO_SMALL) {
    FreePool (Map);
    MapSize += 2 * DescSize;
    Map = AllocatePool (MapSize);
    if (Map == NULL) {
      FreePool (Ranges);
      return EFI_OUT_OF_RESOURCES;
    }

    Status = gBS->GetMemoryMap (&MapSize, Map, &MapKey, &DescSize, &DescVersion);
  }

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "SnapshotDxe: final GetMemoryMap: %r\n", Status));
    FreePool (Map);
    FreePool (Ranges);
    return Status;
  }

  RangeCount = 0;
  BuildRanges (
    Map,
    MapSize,
    DescSize,
    BufferBase,
    BufferSize,
    Ranges,
    &RangeCount,
    &GuestRamTop,
    &TotalRam
    );

  if (RangeCount == 0) {
    DEBUG ((DEBUG_ERROR, "SnapshotDxe: no capturable RAM ranges\n"));
    FreePool (Map);
    FreePool (Ranges);
    return EFI_NOT_FOUND;
  }

  mTotalPages = 0;
  for (UINT32 Index = 0; Index < RangeCount; Index++) {
    mTotalPages += Ranges[Index].Length / SNAPSHOT_PAGE_SIZE;
  }

  ZeroMem (&mConfig, sizeof (mConfig));
  mConfig.Signature      = SNAPSHOT_CONFIG_MAGIC;
  mConfig.Version        = SNAPSHOT_VERSION;
  mConfig.Mode           = PcdGet32 (PcdSnapshotMode);
  mConfig.BufferBase     = BufferBase;
  mConfig.BufferSize     = BufferSize;
  mConfig.MaxPagesPerSmi = PcdGet32 (PcdSnapshotMaxPagesPerSmi);
  mConfig.PeriodSeconds  = PcdGet32 (PcdSnapshotPeriodSeconds);
  mConfig.GuestRamTop    = GuestRamTop;
  mConfig.TscHz          = PcdGet64 (PcdSnapshotTscHz);
  mConfig.RangeCount     = RangeCount;
  mConfig.RangeEntrySize = sizeof (SNAPSHOT_MEM_RANGE);
  mConfig.Flags          = PcdGetBool (PcdSnapshotIncludeReclaimable) ?
                           SNAPSHOT_CONFIG_INCLUDE_RECLAIMABLE : 0;
  CopyMem (mConfig.Ranges, Ranges, RangeCount * sizeof (SNAPSHOT_MEM_RANGE));

  HeaderSize = OFFSET_OF (EFI_SMM_COMMUNICATE_HEADER, Data);
  mCommBuffer = AllocateRuntimeZeroPool (HeaderSize + sizeof (SNAPSHOT_CONFIG));
  if (mCommBuffer == NULL) {
    FreePool (Map);
    FreePool (Ranges);
    return EFI_OUT_OF_RESOURCES;
  }

  Event = EfiCreateProtocolNotifyEvent (
            &gEfiDxeSmmReadyToLockProtocolGuid,
            TPL_CALLBACK,
            SnapshotReadyToLockNotify,
            NULL,
            &Registration
            );
  if (Event == NULL) {
    DEBUG ((DEBUG_ERROR, "SnapshotDxe: cannot create ReadyToLock notify\n"));
    FreePool (Map);
    FreePool (Ranges);
    return EFI_OUT_OF_RESOURCES;
  }

  DEBUG ((
    DEBUG_INFO,
    "SnapshotDxe: buffer 0x%lx-0x%lx mode=%u ranges=%u pages=%lu, waiting for ReadyToLock\n",
    BufferBase,
    BufferBase + BufferSize,
    mConfig.Mode,
    RangeCount,
    mTotalPages
    ));

  FreePool (Map);
  FreePool (Ranges);
  return EFI_SUCCESS;
}
