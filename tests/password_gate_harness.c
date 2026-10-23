/* Host-side behavioral tests of the actual UEFI authentication implementation.
 * Mock only firmware protocols, storage and console input. */
#include "../submodules/uefi/edk2/QcomModulePkg/Application/LinuxLoader/SuperFbAuth.c"
#include <stdio.h>
#include <string.h>

EFI_GUID gEfiPartitionRecordGuid = {1, 0, 0, {0}};
EFI_GUID gEfiBlockIoProtocolGuid = {2, 0, 0, {0}};
struct StoragePartInfo Ptable[MAX_LUNS];
static EFI_BOOT_SERVICES Services;
EFI_BOOT_SERVICES *gBS = &Services;
static EFI_SYSTEM_TABLE System;
static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL Console;
EFI_SYSTEM_TABLE *gST = &System;
static BOOLEAN FontSupported = TRUE;
static CONST CHAR16 *DisplayedMessage;
static BOOLEAN GlyphWarningDisplayed;
static const CHAR8 *MessageFile;
static EFI_STATUS MessageReadStatus;
static EFI_PARTITION_ENTRY Record;
static EFI_BLOCK_IO_MEDIA Media;
static EFI_BLOCK_IO_PROTOCOL BlockIo;
static EFI_FILE_PROTOCOL Root;
static CHAR8 FileBytes[256];
static UINTN FileLength;
static SFB_KEY Keys[4096];
static UINTN KeyCount, KeyAt, TestCursor;
static UINTN WatchdogTimeout;

VOID *EFIAPI CopyMem (VOID *Out, CONST VOID *In, UINTN Bytes) { return memcpy (Out, In, Bytes); }
VOID *EFIAPI ZeroMem (VOID *Out, UINTN Bytes) { return memset (Out, 0, Bytes); }
INTN EFIAPI StrCmp (CONST CHAR16 *A, CONST CHAR16 *B) {
  while (*A != 0 && *A == *B) { A++; B++; }
  return (INTN)*A - (INTN)*B;
}
UINTN EFIAPI Print (CONST CHAR16 *Format, ...) {
  if (StrCmp (Format, L"Some characters may appear as boxes. Try English text.\r\n\r\n") == 0)
    GlyphWarningDisplayed = TRUE;
  return 0;
}
UINT32 GetMaxLuns (VOID) { return 1; }
BOOLEAN SfbIsExt4Volume (EFI_HANDLE Volume) { return TRUE; }
static EFI_STATUS EFIAPI Protocol (EFI_HANDLE Handle, EFI_GUID *Guid, VOID **Out) {
  if (Guid == &gEfiPartitionRecordGuid) *Out = &Record;
  else if (Guid == &gEfiBlockIoProtocolGuid) *Out = &BlockIo;
  else return EFI_NOT_FOUND;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Close (EFI_FILE_PROTOCOL *File) { return EFI_SUCCESS; }
static EFI_STATUS EFIAPI Watchdog (UINTN Timeout, UINT64 Code, UINTN Bytes, CHAR16 *Data) { WatchdogTimeout = Timeout; return EFI_SUCCESS; }
EFI_STATUS SfbOpenVolumeRoot (EFI_HANDLE Volume, EFI_FILE_PROTOCOL **Out) { *Out = &Root; return EFI_SUCCESS; }
EFI_STATUS SfbReadFileBytes (EFI_FILE_PROTOCOL *File, CONST CHAR16 *Path,
                            VOID *Out, UINTN Capacity, UINTN *Bytes) {
  if (StrCmp (Path, SFB_AUTH_MESSAGE_PATH) == 0) {
    if (EFI_ERROR (MessageReadStatus)) return MessageReadStatus;
    if (MessageFile == NULL) return EFI_NOT_FOUND;
    *Bytes = strlen (MessageFile) < Capacity ? strlen (MessageFile) : Capacity;
    memcpy (Out, MessageFile, *Bytes);
    return EFI_SUCCESS;
  }
  if (FileLength == 0) return EFI_NOT_FOUND;
  *Bytes = FileLength < Capacity ? FileLength : Capacity;
  memcpy (Out, FileBytes, *Bytes);
  return EFI_SUCCESS;
}
VOID SfbShowEnteringScreen (CONST CHAR16 *Text) {}
VOID SfbBeginScreen (CONST CHAR16 *Title, CONST CHAR16 *Subtitle) {
  if (StrCmp (Title, L"Superfastboot Authentication") == 0) DisplayedMessage = Subtitle;
}
static EFI_STATUS EFIAPI TestString (EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, CHAR16 *Text) {
  return FontSupported ? EFI_SUCCESS : EFI_UNSUPPORTED;
}
VOID SfbDebounceMenuExit (VOID) {}
VOID SfbDrawRow (BOOLEAN Selected, CONST CHAR16 *Marker, CONST CHAR16 *Text) {}
UINTN SfbWindowStart (UINTN Cursor, UINTN Count, UINTN Rows) { return 0; }
VOID SfbMoveCursor (UINTN *Cursor, UINTN Count, SFB_KEY Key) {
  *Cursor = Key == SfbKeyUp ? (*Cursor + Count - 1) % Count : (*Cursor + 1) % Count;
}
SFB_KEY SfbWaitForKey (UINT32 Timeout) { return KeyAt < KeyCount ? Keys[KeyAt++] : SfbKeyTimeout; }

static void reset (const char *Pin) {
  UINT8 Digest[32];
  CHAR8 Salt[32];
  UINTN Index;
  mInitialized = mConfigured = mUnlocked = FALSE;
  mFailures = 0;
  ZeroMem (mAuthMessage, sizeof (mAuthMessage));
  MessageFile = NULL; FontSupported = TRUE; DisplayedMessage = NULL;
  GlyphWarningDisplayed = FALSE;
  MessageReadStatus = EFI_SUCCESS;
  System.ConOut = &Console; Console.TestString = TestString;
  KeyAt = KeyCount = TestCursor = 0;
  memset (Ptable, 0, sizeof (Ptable));
  Ptable[0].MaxHandles = 1;
  Ptable[0].HandleInfoList[0].Handle = (EFI_HANDLE)1;
  memset (&Record, 0, sizeof (Record));
  memcpy (Record.PartitionName, L"persist", sizeof (L"persist"));
  memset (&Media, 0, sizeof (Media));
  Media.MediaPresent = Media.LogicalPartition = TRUE;
  BlockIo.Media = &Media;
  Services.HandleProtocol = Protocol;
  Services.SetWatchdogTimer = Watchdog;
  Root.Close = Close;
  FileLength = 0;
  if (Pin != NULL) {
    memset (Salt, 'a', sizeof (Salt));
    SfbPasswordHash (Salt, Pin, strlen (Pin), Digest);
    memcpy (FileBytes, "SFBPW1:", 7);
    memcpy (FileBytes + 7, Salt, 32);
    FileBytes[39] = ':';
    for (Index = 0; Index < 32; Index++) sprintf (FileBytes + 40 + Index * 2, "%02x", Digest[Index]);
    FileLength = SFB_PASSWORD_RECORD_BYTES;
  }
}
static void choose (UINTN Row) {
  while (TestCursor != Row) {
    Keys[KeyCount++] = SfbKeyDown;
    TestCursor = (TestCursor + 1) % 13;
  }
  Keys[KeyCount++] = SfbKeySelect;
}
static void type (const char *Pin) {
  while (*Pin) { choose ((UINTN)(*Pin++ - '0')); }
}
static void submit (const char *Pin) { type (Pin); choose (11); TestCursor = 0; }
#define CHECK(Condition) do { if (!(Condition)) { fprintf (stderr, "Failed at line %d: %s\n", __LINE__, #Condition); return 1; } } while (0)

int main (void) {
  const char *Long = "0123456789012345" "0123456789012345"
                     "0123456789012345" "0123456789012345";
  reset (NULL);
  SfbAuthInitialize ();
  CHECK (SfbAuthRequest () && KeyAt == 0 && SfbAuthIsUnlocked ());
  reset ("001234");
  FileBytes[0] = 'X';
  SfbAuthInitialize ();
  CHECK (!SfbAuthRequest () && KeyAt == 0);
  reset ("001234");
  Ptable[0].MaxHandles = 2;
  Ptable[0].HandleInfoList[1].Handle = (EFI_HANDLE)2;
  SfbAuthInitialize ();
  CHECK (!SfbAuthRequest ());
  reset ("001234");
  Media.RemovableMedia = TRUE;
  SfbAuthInitialize ();
  CHECK (!SfbAuthRequest ());
  reset ("001234");
  SfbAuthInitialize ();
  submit ("001234");
  CHECK (SfbAuthRequest () && SfbAuthIsUnlocked ());
  CHECK (WatchdogTimeout == 300);
  SfbAuthInitialize ();
  CHECK (SfbAuthIsUnlocked ());
  reset ("777777");
  SfbAuthInitialize ();
  choose (7);
  for (UINTN Repeat = 0; Repeat < 5; Repeat++) Keys[KeyCount++] = SfbKeySelect;
  choose (11);
  CHECK (SfbAuthRequest ()); /* Repeated power presses keep the selected digit. */
  reset (Long);
  SfbAuthInitialize ();
  submit (Long);
  CHECK (SfbAuthRequest ());
  reset ("12345678");
  SfbAuthInitialize ();
  type ("12345670"); choose (10); type ("8"); choose (11);
  CHECK (SfbAuthRequest ());
  reset ("123456");
  SfbAuthInitialize ();
  submit ("12345"); submit ("123456");
  CHECK (SfbAuthRequest () && mFailures == 1);
  reset ("123456");
  SfbAuthInitialize ();
  submit ("1"); submit ("12"); submit ("12345"); submit ("123456");
  CHECK (!SfbAuthRequest () && mFailures == 3 && !SfbAuthIsUnlocked ());
  CHECK (KeyAt < KeyCount); /* A queued fourth confirmation is never read. */
  CHECK (!SfbAuthRequest () && mFailures == 3);
  reset ("123456");
  SfbAuthInitialize ();
  submit ("000000"); submit ("000000"); submit ("000000"); submit ("123456");
  CHECK (!SfbAuthRequest () && mFailures == 3 && !SfbAuthIsUnlocked ());
  SfbAuthInitialize ();
  CHECK (!SfbAuthRequest () && mFailures == 3);
  reset ("123456");
  SfbAuthInitialize ();
  choose (12);
  CHECK (!SfbAuthRequest () && mFailures == 0);
  reset ("123456");
  SfbAuthInitialize ();
  CHECK (!SfbAuthRequest () && mFailures == 0); /* Input timeout. */
  CHECK (WatchdogTimeout == 300);
  reset ("123456");
  SfbAuthInitialize ();
  choose (11);
  CHECK (!SfbAuthRequest () && mFailures == 0); /* Empty confirmation. */
  {
    CHAR16 Text[SFB_AUTH_MESSAGE_CHARS + 1];
    const UINT8 Chinese[] = {0xE4,0xBD,0xA0,0xE5,0xA5,0xBD};
    const UINT8 Bad[][4] = {{0xC0,0xAF,0,0}, {0xED,0xA0,0x80,0}, {0xF0,0x9F,0x98,0x80}};
    CHECK (SfbDecodeAuthMessage (Chinese, sizeof (Chinese), Text));
    CHECK (Text[0] == 0x4F60 && Text[1] == 0x597D && Text[2] == 0);
    for (UINTN Index = 0; Index < 3; Index++) CHECK (!SfbDecodeAuthMessage (Bad[Index], 4, Text));
    CHECK (!SfbDecodeAuthMessage ((CONST UINT8 *)"bad\rtext", 8, Text));
    reset ("123456"); MessageFile = "Owner: test@example.com";
    SfbAuthInitialize (); submit ("123456");
    CHECK (SfbAuthRequest () && StrCmp (DisplayedMessage, L"Owner: test@example.com") == 0);
    CHECK (!GlyphWarningDisplayed);
    reset ("123456"); MessageFile = "\xE4\xBD\xA0\xE5\xA5\xBD";
    FontSupported = FALSE;
    SfbAuthInitialize (); submit ("123456");
    CHECK (SfbAuthRequest () && DisplayedMessage == mAuthMessage && GlyphWarningDisplayed);
    CHECK (DisplayedMessage[0] == 0x4F60); /* Original text is still rendered. */
    reset ("123456"); MessageFile = "Owner: test@example.com";
    Console.TestString = NULL;
    SfbAuthInitialize (); submit ("123456");
    CHECK (SfbAuthRequest () && StrCmp (DisplayedMessage, L"Owner: test@example.com") == 0);
    CHECK (GlyphWarningDisplayed);
    {
      UINT8 LongText[243];
      memset (LongText, 'x', sizeof (LongText));
      CHECK (SfbDecodeAuthMessage (LongText, 80, Text) && Text[80] == 0);
      CHECK (!SfbDecodeAuthMessage (LongText, 81, Text) && Text[0] == 0);
      CHECK (!SfbDecodeAuthMessage (LongText, sizeof (LongText), Text));
      CHECK (!SfbDecodeAuthMessage ((CONST UINT8 *)"x", (UINTN)-1, Text));
      CHECK (!SfbDecodeAuthMessage (NULL, 1, Text));
      CHECK (!SfbDecodeAuthMessage ((CONST UINT8 *)"x", 1, NULL));
      CHECK (!SfbDecodeAuthMessage ((CONST UINT8 *)"a\0b", 3, Text));
      CHECK (!SfbDecodeAuthMessage ((CONST UINT8 *)"\x1b[2J", 4, Text));
      CHECK (!SfbDecodeAuthMessage ((CONST UINT8 *)"\xEF\xBF\xB0", 3, Text));
      CHECK (!SfbDecodeAuthMessage ((CONST UINT8 *)"\xE4\xBD", 2, Text));
      CHECK (SfbDecodeAuthMessage ((CONST UINT8 *)"%s%n%a", 6, Text));
      CHECK (StrCmp (Text, L"%s%n%a") == 0);
      reset ("123456"); MessageFile = "bad\rtext";
      SfbAuthInitialize (); submit ("123456");
      CHECK (SfbAuthRequest () && DisplayedMessage[0] == 0 && !GlyphWarningDisplayed);
      reset ("123456"); MessageFile = "%s%n%a";
      SfbAuthInitialize (); submit ("123456");
      CHECK (SfbAuthRequest () && StrCmp (DisplayedMessage, L"%s%n%a") == 0);
      reset ("123456"); MessageFile = "optional"; MessageReadStatus = EFI_DEVICE_ERROR;
      SfbAuthInitialize (); submit ("123456");
      CHECK (SfbAuthRequest () && DisplayedMessage[0] == 0 && !GlyphWarningDisplayed);
    }
  }
  puts ("UEFI authentication: no-password bypass, invalid-file denial, storage identity, leading zeros, 64 digits, delete, short-password failures, three-error lockout, empty input, cancel and timeout passed");
  return 0;
}
