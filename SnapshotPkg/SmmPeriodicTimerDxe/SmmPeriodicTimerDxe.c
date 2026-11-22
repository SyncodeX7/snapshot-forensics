/** @file
  QEMU Q35 ICH9 periodic SMI source + PI periodic timer dispatch protocol.

  Hardware details (QEMU hw/acpi/ich9_timer.c):
    - LPC PCI config 0:1f.0 offset 0xa0 (GEN_PMCON_1) bits [1:0] select the
      period: 00=64s, 01=32s, 10=16s, 11=8s.
    - PMBASE + 0x30 (SMI_EN) bit 14 (PERIODIC_EN) enables the source.
    - PMBASE + 0x34 (SMI_STS) bit 14 (PERIODIC_STS) is write-1-to-clear.

  Copyright (c) 2026
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/SmmServicesTableLib.h>
#include <Protocol/SmmPeriodicTimerDispatch2.h>

#define SNAPSHOT_MAX_TIMER_CHILDREN  16

#define ICH9_PMIO_SMI_EN             0x30
#define ICH9_PMIO_SMI_STS            0x34
#define ICH9_SMI_EN_GBL_SMI_EN       BIT0
#define ICH9_SMI_EN_PERIODIC_EN      BIT14
#define ICH9_SMI_STS_PERIODIC_STS    BIT14

#define ICH9_LPC_GEN_PMCON_1         0xA0
#define ICH9_LPC_PMBASE              0x40
#define ICH9_PMBASE_MASK             0xFF80

typedef struct {
  BOOLEAN                                  Used;
  EFI_MM_HANDLER_ENTRY_POINT               Handler;
  EFI_SMM_PERIODIC_TIMER_REGISTER_CONTEXT  Context;
  UINT64                                   Elapsed100ns;
} SNAPSHOT_TIMER_CHILD;

STATIC SNAPSHOT_TIMER_CHILD  mChildren[SNAPSHOT_MAX_TIMER_CHILDREN];
STATIC UINT32                mPmBase;
STATIC UINT64                mPeriod100ns;
STATIC UINT64                mTickInterval100ns;
STATIC UINT64                mTickCount;
STATIC EFI_HANDLE            mSmiHandle;
STATIC EFI_HANDLE            mProtocolHandle;

STATIC
UINT32
PciRead32 (
  IN UINT8  Offset
  )
{
  IoWrite32 (0xCF8, 0x80000000U | (0x1FU << 11) | (Offset & 0xFC));
  return IoRead32 (0xCFC);
}

STATIC
VOID
PciWrite16 (
  IN UINT8   Offset,
  IN UINT16  Value
  )
{
  UINT32 Shift;
  UINT32 Data;

  Shift = (Offset & 2) * 8;
  IoWrite32 (0xCF8, 0x80000000U | (0x1FU << 11) | (Offset & 0xFC));
  Data  = IoRead32 (0xCFC);
  Data &= ~(0xFFFFU << Shift);
  Data |= ((UINT32)Value << Shift);
  IoWrite32 (0xCFC, Data);
}

STATIC
VOID
EnablePeriodicSmi (
  VOID
  )
{
  UINT16  GenPmCon1;
  UINT16  Select;
  UINT32  SmiEn;

  mPmBase = PciRead32 (ICH9_LPC_PMBASE) & ICH9_PMBASE_MASK;

  // Select the shortest supported hardware period: 8 seconds.
  GenPmCon1  = (UINT16)(PciRead32 (ICH9_LPC_GEN_PMCON_1) & 0xFFFF);
  GenPmCon1 &= (UINT16)~0x3;
  GenPmCon1 |= 0x3;
  PciWrite16 (ICH9_LPC_GEN_PMCON_1, GenPmCon1);

  // Read back; SMI_LOCK does not prevent changing bits [1:0] in QEMU, but be
  // defensive in case a different chipset model masks the write.
  GenPmCon1 = (UINT16)(PciRead32 (ICH9_LPC_GEN_PMCON_1) & 0xFFFF);
  Select    = (UINT16)(GenPmCon1 & 0x3);
  mPeriod100ns = (UINT64)8 * (1ULL << (3 - Select)) * 10000000ULL;
  mTickInterval100ns = mPeriod100ns;

  SmiEn  = IoRead32 (mPmBase + ICH9_PMIO_SMI_EN);
  SmiEn |= ICH9_SMI_EN_GBL_SMI_EN | ICH9_SMI_EN_PERIODIC_EN;
  IoWrite32 (mPmBase + ICH9_PMIO_SMI_EN, SmiEn);

  DEBUG ((
    DEBUG_INFO,
    "SmmPeriodicTimer: PMBASE=0x%x GEN_PMCON_1=0x%x period=%lu00ns SMI_EN=0x%x\n",
    mPmBase,
    GenPmCon1,
    mPeriod100ns / 100,
    IoRead32 (mPmBase + ICH9_PMIO_SMI_EN)
    ));
}

STATIC
EFI_STATUS
EFIAPI
SmmPeriodicTimerRegister (
  IN CONST EFI_SMM_PERIODIC_TIMER_DISPATCH2_PROTOCOL  *This,
  IN       EFI_MM_HANDLER_ENTRY_POINT                 DispatchFunction,
  IN CONST EFI_SMM_PERIODIC_TIMER_REGISTER_CONTEXT    *RegisterContext,
  OUT      EFI_HANDLE                                 *DispatchHandle
  )
{
  UINTN  Index;

  if ((DispatchFunction == NULL) || (RegisterContext == NULL) ||
      (DispatchHandle == NULL))
  {
    return EFI_INVALID_PARAMETER;
  }

  for (Index = 0; Index < SNAPSHOT_MAX_TIMER_CHILDREN; Index++) {
    if (!mChildren[Index].Used) {
      mChildren[Index].Used          = TRUE;
      mChildren[Index].Handler       = DispatchFunction;
      mChildren[Index].Context       = *RegisterContext;
      mChildren[Index].Elapsed100ns  = 0;
      *DispatchHandle                = (EFI_HANDLE)&mChildren[Index];
      return EFI_SUCCESS;
    }
  }

  return EFI_OUT_OF_RESOURCES;
}

STATIC
EFI_STATUS
EFIAPI
SmmPeriodicTimerUnRegister (
  IN CONST EFI_SMM_PERIODIC_TIMER_DISPATCH2_PROTOCOL  *This,
  IN       EFI_HANDLE                                 DispatchHandle
  )
{
  SNAPSHOT_TIMER_CHILD  *Child;

  Child = (SNAPSHOT_TIMER_CHILD *)DispatchHandle;
  if ((Child < &mChildren[0]) || (Child >= &mChildren[SNAPSHOT_MAX_TIMER_CHILDREN]) ||
      (!Child->Used))
  {
    return EFI_INVALID_PARAMETER;
  }

  Child->Used = FALSE;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
SmmPeriodicTimerGetNextShorterInterval (
  IN CONST EFI_SMM_PERIODIC_TIMER_DISPATCH2_PROTOCOL  *This,
  IN OUT   UINT64                                     **MmiTickInterval
  )
{
  if (MmiTickInterval == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (*MmiTickInterval == NULL) {
    *MmiTickInterval = &mTickInterval100ns;
  } else {
    *MmiTickInterval = NULL;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
SmmPeriodicTimerSmiHandler (
  IN     EFI_HANDLE  DispatchHandle,
  IN     CONST VOID  *Context,
  IN OUT VOID        *CommBuffer,
  IN OUT UINTN       *CommBufferSize
  )
{
  UINT32                          SmiSts;
  UINTN                           Index;
  EFI_SMM_PERIODIC_TIMER_CONTEXT  TimerContext;
  UINTN                           TimerContextSize;
  EFI_STATUS                      Status;

  SmiSts = IoRead32 (mPmBase + ICH9_PMIO_SMI_STS);
  if ((SmiSts & ICH9_SMI_STS_PERIODIC_STS) == 0) {
    // This SMI belongs to another source (APM, TCO, ...).
    return EFI_SUCCESS;
  }

  // Write-1-to-clear, then dispatch children.
  IoWrite32 (mPmBase + ICH9_PMIO_SMI_STS, ICH9_SMI_STS_PERIODIC_STS);
  mTickCount++;

  for (Index = 0; Index < SNAPSHOT_MAX_TIMER_CHILDREN; Index++) {
    if (!mChildren[Index].Used) {
      continue;
    }

    mChildren[Index].Elapsed100ns += mPeriod100ns;
    if (mChildren[Index].Elapsed100ns < mChildren[Index].Context.Period) {
      continue;
    }

    TimerContext.ElapsedTime = mChildren[Index].Elapsed100ns;
    TimerContextSize         = sizeof (TimerContext);
    mChildren[Index].Elapsed100ns = 0;

    Status = mChildren[Index].Handler (
                                  (EFI_HANDLE)&mChildren[Index],
                                  &mChildren[Index].Context,
                                  &TimerContext,
                                  &TimerContextSize
                                  );
    if (EFI_ERROR (Status) && (Status != EFI_INTERRUPT_PENDING)) {
      DEBUG ((DEBUG_WARN, "SmmPeriodicTimer: child %u returned %r\n", Index, Status));
    }
  }

  return EFI_SUCCESS;
}

STATIC EFI_SMM_PERIODIC_TIMER_DISPATCH2_PROTOCOL  mPeriodicTimerProtocol = {
  SmmPeriodicTimerRegister,
  SmmPeriodicTimerUnRegister,
  SmmPeriodicTimerGetNextShorterInterval
};;

EFI_STATUS
EFIAPI
SmmPeriodicTimerEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  EnablePeriodicSmi ();

  Status = gSmst->SmiHandlerRegister (
                    SmmPeriodicTimerSmiHandler,
                    NULL,
                    &mSmiHandle
                    );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "SmmPeriodicTimer: SmiHandlerRegister: %r\n", Status));
    return Status;
  }

  mProtocolHandle = NULL;
  Status = gSmst->SmmInstallProtocolInterface (
                    &mProtocolHandle,
                    &gEfiSmmPeriodicTimerDispatch2ProtocolGuid,
                    EFI_NATIVE_INTERFACE,
                    &mPeriodicTimerProtocol
                    );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "SmmPeriodicTimer: InstallProtocol: %r\n", Status));
  }

  return Status;
}
