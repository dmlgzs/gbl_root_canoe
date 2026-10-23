/* Numeric password gate for the BDS, configured by the rooted module WebUI.
 * SPDX-License-Identifier: BSD-3-Clause */
#include "SuperFbAuth.h"
#include "SuperFbMenu.h"
#include "SuperFbPassword.h"
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/PartitionTableUpdate.h>
#include <Library/UefiLib.h>
#include <Uefi/UefiGpt.h>

#define SFB_PASSWORD_PATH L"\\efisp\\menu_password"
#define SFB_PASSWORD_ATTEMPTS 3

/* Qualcomm's GPT-record protocol has no standalone public protocol header. */
extern EFI_GUID gEfiPartitionRecordGuid;

STATIC BOOLEAN mInitialized;
STATIC BOOLEAN mConfigured;
STATIC BOOLEAN mUnlocked;
STATIC UINTN mFailures;
STATIC CHAR8 mSalt[32];
STATIC UINT8 mDigest[32];

/* Use the platform's internal-storage partition table, never a scan of FAT
 * labels, USB media, BOOTENTRIES or efisp.fat. Ambiguous identities fail closed. */
EFI_STATUS
SfbGetPersistVolume (OUT EFI_HANDLE *Volume)
{
  UINT32 Lun;
  UINTN Index;
  EFI_HANDLE Found = NULL;
  if (Volume == NULL) return EFI_INVALID_PARAMETER;
  *Volume = NULL;
  for (Lun = 0; Lun < GetMaxLuns () && Lun < MAX_LUNS; Lun++) {
    for (Index = 0; Index < Ptable[Lun].MaxHandles; Index++) {
      EFI_HANDLE Handle = Ptable[Lun].HandleInfoList[Index].Handle;
      EFI_PARTITION_ENTRY *Record = NULL;
      EFI_BLOCK_IO_PROTOCOL *BlkIo = NULL;
      CHAR16 Name[ARRAY_SIZE (Record->PartitionName) + 1];
      if (Handle == NULL ||
          EFI_ERROR (gBS->HandleProtocol (Handle, &gEfiPartitionRecordGuid,
                                         (VOID **)&Record)) || Record == NULL)
        continue;
      CopyMem (Name, Record->PartitionName, sizeof (Record->PartitionName));
      Name[ARRAY_SIZE (Record->PartitionName)] = L'\0';
      if (StrCmp (Name, L"persist") != 0) continue;
      if (EFI_ERROR (gBS->HandleProtocol (Handle, &gEfiBlockIoProtocolGuid,
                                         (VOID **)&BlkIo)) ||
          BlkIo == NULL || BlkIo->Media == NULL ||
          BlkIo->Media->RemovableMedia || !BlkIo->Media->MediaPresent ||
          !BlkIo->Media->LogicalPartition || !SfbIsExt4Volume (Handle))
        return EFI_ACCESS_DENIED;
      if (Found != NULL && Found != Handle) return EFI_ACCESS_DENIED;
      Found = Handle;
    }
  }
  if (Found == NULL) return EFI_NOT_FOUND;
  *Volume = Found;
  return EFI_SUCCESS;
}

VOID
SfbAuthInitialize (VOID)
{
  EFI_HANDLE Volume;
  EFI_FILE_PROTOCOL *Root = NULL;
  CHAR8 Buffer[SFB_PASSWORD_RECORD_BYTES + 2];
  UINTN Bytes = 0;
  EFI_STATUS Status;
  if (mInitialized) return;
  mInitialized = TRUE;
  if (EFI_ERROR (SfbGetPersistVolume (&Volume)) ||
      EFI_ERROR (SfbOpenVolumeRoot (Volume, &Root)) || Root == NULL) return;
  Status = SfbReadFileBytes (Root, SFB_PASSWORD_PATH, Buffer, sizeof (Buffer), &Bytes);
  Root->Close (Root);
  /* Only a genuinely absent file disables the gate. Corruption or I/O
   * failures do not turn a configured password into unrestricted access. */
  if (Status == EFI_NOT_FOUND) mUnlocked = TRUE;
  if (!EFI_ERROR (Status))
    mConfigured = SfbPasswordParse (Buffer, Bytes, mSalt, mDigest);
  ZeroMem (Buffer, sizeof (Buffer));
}

BOOLEAN
SfbAuthIsUnlocked (VOID)
{
  return mUnlocked;
}

BOOLEAN
SfbAuthRequest (VOID)
{
  CHAR8 Input[SFB_PIN_MAX_DIGITS];
  UINTN Length = 0;
  UINTN Cursor = 0;
  UINTN Index;
  SFB_KEY Key;
  if (mUnlocked) return TRUE;
  if (!mConfigured || mFailures >= SFB_PASSWORD_ATTEMPTS) return FALSE;
  ZeroMem (Input, sizeof (Input));
  /* Long numeric passwords must not race the default five-minute watchdog. */
  gBS->SetWatchdogTimer (0, 0, 0, NULL);
  SfbShowEnteringScreen (L"Superfastboot Authentication");
  while (mFailures < SFB_PASSWORD_ATTEMPTS) {
    UINTN Start = SfbWindowStart (Cursor, 13, SFB_VISIBLE_ROWS);
    SfbBeginScreen (L"Enter Superfastboot", L"Volume: choose. Power: confirm.");
    Print (L"Password: ");
    for (Index = 0; Index < Length; Index++) Print (L"*");
    Print (L"_ (%u/%u)", (UINT32)Length, (UINT32)SFB_PIN_MAX_DIGITS);
    Print (L"\r\nAttempts remaining: %u\r\n\r\n",
           (UINT32)(SFB_PASSWORD_ATTEMPTS - mFailures));
    for (Index = Start; Index < 13 && Index < Start + SFB_VISIBLE_ROWS; Index++) {
      CHAR16 Digit[2] = { (CHAR16)(L'0' + Index), L'\0' };
      CONST CHAR16 *Text = Index < 10 ? Digit :
                          Index == 10 ? L"Delete one digit" :
                          Index == 11 ? L"Confirm password" : L"Boot Android";
      SfbDrawRow (Index == Cursor, L" ", Text);
    }
    /* Inactivity and console failures take the same Android-only path. */
    Key = SfbWaitForKey (30000);
    if (Key == SfbKeyTimeout) break;
    if (Key == SfbKeyUp || Key == SfbKeyDown) {
      SfbMoveCursor (&Cursor, 13, Key);
      continue;
    }
    SfbDebounceMenuExit ();
    if (Cursor < 10) {
      if (Length < SFB_PIN_MAX_DIGITS) Input[Length++] = (CHAR8)('0' + Cursor);
    } else if (Cursor == 10) {
      if (Length > 0) Input[--Length] = 0;
    } else if (Cursor == 12) {
      break;
    } else if (Length == 0) {
      break; /* Confirm with no password: Android, never privileged access. */
    } else {
      UINT8 Digest[32];
      UINT8 Difference = 1;
      /* The minimum length applies when setting a password, not to whether
       * a nonempty confirmation consumes an attempt. Short input is wrong. */
      if (SfbPasswordHash (mSalt, Input, Length, Digest)) {
        Difference = 0;
        for (Index = 0; Index < sizeof (Digest); Index++)
          Difference |= (UINT8)(Digest[Index] ^ mDigest[Index]);
      }
      ZeroMem (Digest, sizeof (Digest));
      ZeroMem (Input, sizeof (Input));
      if (Difference == 0) {
        mUnlocked = TRUE;
        ZeroMem (mDigest, sizeof (mDigest));
        gBS->SetWatchdogTimer (300, 0, 0, NULL);
        return TRUE;
      }
      mFailures++;
      Length = 0;
      Cursor = 0;
      SfbBeginScreen (L"Incorrect password",
                     mFailures >= SFB_PASSWORD_ATTEMPTS ? L"Booting Android..." : NULL);
      Print (L"%u attempt(s) remaining.\r\n",
             (UINT32)(SFB_PASSWORD_ATTEMPTS - mFailures));
      SfbDebounceMenuExit ();
    }
  }
  ZeroMem (Input, sizeof (Input));
  /* Restore the ordinary UEFI watchdog before returning to the boot chain. */
  gBS->SetWatchdogTimer (300, 0, 0, NULL);
  return FALSE;
}
