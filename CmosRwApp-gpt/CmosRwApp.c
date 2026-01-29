#include <Base.h>
#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiApplicationEntryPoint.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Protocol/PciRootBridgeIo.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <stdarg.h>


#define CMOS_INDEX_PORT 0x70
#define CMOS_DATA_PORT  0x71

#define CMOS_MIN        0x00
#define CMOS_MAX        0x7F

// 寫入保護範圍（你要求的 0x30~0x3F）
#define CMOS_WRITE_MIN  0x30
#define CMOS_WRITE_MAX  0x3F

#define COLS 16
#define ROWS 8

static EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *mRbIo = NULL;

static EFI_STATUS InitRootBridgeIo(VOID) {
  return gBS->LocateProtocol(&gEfiPciRootBridgeIoProtocolGuid, NULL, (VOID **)&mRbIo);
}

static UINT8 CmosRead8(UINT8 Index) {
  UINT8 Sel = (UINT8)(0x80 | (Index & 0x7F)); // Disable NMI + index
  UINT8 Data = 0;
  if (mRbIo == NULL) return 0;

  (VOID)mRbIo->Io.Write(mRbIo, EfiPciWidthUint8, CMOS_INDEX_PORT, 1, &Sel);
  (VOID)mRbIo->Io.Read (mRbIo, EfiPciWidthUint8, CMOS_DATA_PORT,  1, &Data);
  return Data;
}

static EFI_STATUS CmosWrite8(UINT8 Index, UINT8 Value) {
  UINT8 Sel = (UINT8)(0x80 | (Index & 0x7F));
  if (mRbIo == NULL) return EFI_NOT_READY;

  EFI_STATUS Status = mRbIo->Io.Write(mRbIo, EfiPciWidthUint8, CMOS_INDEX_PORT, 1, &Sel);
  if (EFI_ERROR(Status)) return Status;

  return mRbIo->Io.Write(mRbIo, EfiPciWidthUint8, CMOS_DATA_PORT, 1, &Value);
}

static EFI_STATUS WaitKey(EFI_INPUT_KEY *Key) {
  UINTN Idx;
  gBS->WaitForEvent(1, &gST->ConIn->WaitForKey, &Idx);

  EFI_STATUS Status;
  do {
    Status = gST->ConIn->ReadKeyStroke(gST->ConIn, Key);
  } while (Status == EFI_NOT_READY);

  return Status;
}

static INTN HexVal(CHAR16 Ch) {
  if (Ch >= L'0' && Ch <= L'9') return (INTN)(Ch - L'0');
  if (Ch >= L'a' && Ch <= L'f') return (INTN)(Ch - L'a' + 10);
  if (Ch >= L'A' && Ch <= L'F') return (INTN)(Ch - L'A' + 10);
  return -1;
}

// 在畫面底部讀兩個 hex digit，支援 ESC 取消、Backspace 刪除
static EFI_STATUS ReadHexByte(UINT8 *OutValue) {
  if (OutValue == NULL) return EFI_INVALID_PARAMETER;

  UINT8 digits = 0;
  INTN hi = -1, lo = -1;

  while (TRUE) {
    EFI_INPUT_KEY Key;
    EFI_STATUS Status = WaitKey(&Key);
    if (EFI_ERROR(Status)) return Status;

    // ESC 取消
    if (Key.ScanCode == SCAN_ESC) return EFI_ABORTED;

    // Backspace
    if (Key.UnicodeChar == CHAR_BACKSPACE) {
      if (digits > 0) {
        digits--;
        if (digits == 0) hi = -1;
        if (digits == 1) lo = -1;
        Print(L"\b \b"); // 簡單回刪顯示
      }
      continue;
    }

    // 只接受 hex 字元
    INTN v = HexVal(Key.UnicodeChar);
    if (v < 0) continue;

    if (digits == 0) {
      hi = v;
      digits = 1;
      Print(L"%c", Key.UnicodeChar);
    } else if (digits == 1) {
      lo = v;
      digits = 2;
      Print(L"%c", Key.UnicodeChar);
      *OutValue = (UINT8)((hi << 4) | lo);
      return EFI_SUCCESS;
    }
  }
}

static VOID SetAttr(UINTN Attr) {
  gST->ConOut->SetAttribute(gST->ConOut, Attr);
}

static VOID Clear(VOID) {
  gST->ConOut->ClearScreen(gST->ConOut);
}

// 畫整個 0x00~0x7F 表格，並高亮選取格
static VOID DrawTable(UINT8 SelectedIndex) {
  // 畫面布局
  // Row 0: title
  // Row 2..: table
  Clear();

  SetAttr(EFI_LIGHTGRAY);
  Print(L"CMOS 0x00-0x7F Viewer/Editor (EDK2)\r\n");
  Print(L"Arrows: Move | Enter: Write (only 0x%02x-0x%02x) | Esc: Exit\r\n\r\n",
        CMOS_WRITE_MIN, CMOS_WRITE_MAX);

  // Header
  Print(L"     ");
  for (UINTN c = 0; c < COLS; c++) {
    Print(L"%02x ", c);
  }
  Print(L"\r\n");

  for (UINTN r = 0; r < ROWS; r++) {
    UINT8 base = (UINT8)(r * COLS);
    Print(L"%02x: ", base);

    for (UINTN c = 0; c < COLS; c++) {
      UINT8 idx = (UINT8)(base + c);
      UINT8 val = CmosRead8(idx);

      BOOLEAN sel = (idx == SelectedIndex);
      if (sel) {
        // 反白：黑底白字（你也可改成其他 attribute）
        SetAttr(EFI_WHITE | EFI_BACKGROUND_BLUE);
      } else {
        SetAttr(EFI_LIGHTGRAY);
      }
      Print(L"%02x ", val);
    }
    SetAttr(EFI_LIGHTGRAY);
    Print(L"\r\n");
  }

  Print(L"\r\n");
}

//static VOID ShowStatusLine(CONST CHAR16 *Fmt, ...) {
  // 在表格下方直接 Print（本範例簡化，不做精確定位）
  //VA_LIST Marker;
  //VA_START(Marker, Fmt);
  //Print(Fmt, Marker);
  //VA_END(Marker);
//}

EFI_STATUS
EFIAPI
UefiMain(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
 EFI_STATUS Status;

  Status = InitRootBridgeIo();
  if (EFI_ERROR(Status)) {
    Print(L"Locate PciRootBridgeIo failed: %r\r\n", Status);
    return Status;
  }

  UINT8 sel = 0x00;
  DrawTable(sel);

  while (TRUE) {
    EFI_INPUT_KEY Key;
    Status = WaitKey(&Key);
    if (EFI_ERROR(Status)) break;

    // ESC 退出（ScanCode）
    if (Key.ScanCode == SCAN_ESC) {
      break;
    }

    // 方向鍵移動（ScanCode）
    switch (Key.ScanCode) {
      case SCAN_LEFT:
        if ((sel & 0x0F) != 0) sel--;
        DrawTable(sel);
        continue;
      case SCAN_RIGHT:
        if ((sel & 0x0F) != 0x0F && sel < CMOS_MAX) sel++;
        DrawTable(sel);
        continue;
      case SCAN_UP:
        if (sel >= COLS) sel = (UINT8)(sel - COLS);
        DrawTable(sel);
        continue;
      case SCAN_DOWN:
        if (sel + COLS <= CMOS_MAX) sel = (UINT8)(sel + COLS);
        DrawTable(sel);
        continue;
      default:
        break;
    }

    // Enter：寫入模式（注意 Enter 多半是 UnicodeChar == '\r'）
    if (Key.UnicodeChar == CHAR_CARRIAGE_RETURN) {
      DrawTable(sel);

      // 保護：只允許 0x30~0x3F
      if (sel < CMOS_WRITE_MIN || sel > CMOS_WRITE_MAX) {
        Print(L"Write blocked: offset 0x%02x is outside allowed range (0x%02x-0x%02x).\r\n",
                       sel, CMOS_WRITE_MIN, CMOS_WRITE_MAX);
        continue;
      }

      UINT8 oldv = CmosRead8(sel);
      Print(L"Offset 0x%02x current=0x%02x. Enter 2 hex digits to write (ESC cancel): 0x",
                     sel, oldv);

      UINT8 newv = 0;
      Status = ReadHexByte(&newv);
      if (Status == EFI_ABORTED) {
        Print(L"\r\nCanceled.\r\n");
        continue;
      }
      if (EFI_ERROR(Status)) {
        Print(L"\r\nInput error: %r\r\n", Status);
        continue;
      }

      Status = CmosWrite8(sel, newv);
      if (EFI_ERROR(Status)) {
        Print(L"\r\nWrite failed: %r\r\n", Status);
        continue;
      }

      UINT8 verify = CmosRead8(sel);
      Print(L"\r\nWrote 0x%02x, verify readback=0x%02x.\r\n", newv, verify);

      // 更新畫面
      DrawTable(sel);
    }
  }

  SetAttr(EFI_LIGHTGRAY);
  Print(L"\r\nBye.\r\n");
  return EFI_SUCCESS;
}
