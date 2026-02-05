# edk2-PCI_ROOT_BRIDGE_IO_PROTOCOL
usd PCI_ROOT_BRIDGE_IO_PROTOCOL

README — CMOS Editor (UEFI Shell / EDK2)
1. 這支程式在做什麼

在 UEFI Shell 內顯示 CMOS 0x00~0x7F (128 bytes) 的表格（8x16）

方向鍵移動游標

Enter 進入寫入模式（限制寫入範圍：0x30~0x3F）

輸入 2 位 Hex（支援 Backspace/ESC 取消）

寫入後讀回驗證、重畫表格

2. 關鍵硬體背景：為什麼是 0x70 / 0x71

0x70 = CMOS/RTC Index Port：先把要讀/寫的 CMOS Offset 寫到這裡

0x71 = CMOS/RTC Data Port：再從這裡讀出或寫入該 Offset 的值

典型流程（1 byte）：

OUT 0x70, index

IN 0x71 -> data

寫入則是：

OUT 0x70, index

OUT 0x71, value

3. 你這份程式「最重要的依賴」
3.1 EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL（你目前在用）

你用的是：

PciRootBridgeIo->Io.Write() / PciRootBridgeIo->Io.Read()

它的設計目標是：透過 Root Bridge 去做 I/O Port Access（以及 PCI config access）。

重點：它不是保證每個 port 都能讀寫。
0x70/0x71 在很多真機上會因為平台策略、I/O decode、SMM 保護、晶片組鎖而失敗或回固定值。

4. 函數使用方法（你程式裡每個核心函數怎麼用）
4.1 InitRootBridgeIo()

用途：取得 EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL 實例到 mRbIo

你實作：

gBS->LocateProtocol(&gEfiPciRootBridgeIoProtocolGuid, NULL, (VOID**)&mRbIo);

常見回傳：

EFI_SUCCESS：拿到了

EFI_NOT_FOUND：系統沒有安裝此協定（或你執行階段沒有）

EFI_INVALID_PARAMETER：傳入指標錯（較少）

筆記：

真機通常「有」PciRootBridgeIo，但 不代表它允許你對 0x70/0x71 做 I/O。

4.2 CmosRead8(Index)

用途：讀 CMOS offset（0x00~0x7F）的一個 byte

你的流程：

組 Sel（你用了 0x80 | (Index & 0x7F)）

Io.Write(..., Address=0x70, Buffer=&Sel)

Io.Read (..., Address=0x71, Buffer=&Data)

return Data

注意點（超常見地雷）：

0x80 是 “Disable NMI” 的常見做法，但有些平台/環境會讓你讀不到或讀出怪值
→ 你可以做 A/B 測試：

Sel = Index & 0x7F;（先不要 OR 0x80）

或者只在必要時才 OR 0x80

更重要：你現在 CmosRead8 把 Status 直接丟掉了
如果平台回 EFI_INVALID_PARAMETER，你會完全看不出來，只會看到表格一片 00 或固定值。

建議：CmosRead8 內一定要檢查 Status，至少 debug print 一次。

4.3 CmosWrite8(Index, Value)

用途：寫 CMOS offset 的一個 byte

你的流程：

Io.Write(0x70, Sel)

Io.Write(0x71, Value)

常見回傳：

EFI_SUCCESS：寫成功

EFI_INVALID_PARAMETER：最常見！多半是平台不允許這個 port、或這個 RootBridge 不 decode 0x70/0x71

EFI_OUT_OF_RESOURCES：很少見

你遇到的「write failed status = invalid parameter」通常代表：

RootBridgeIo 介面本身不接受 Address=0x70/0x71（I/O 範圍不屬於它、或被平台擋）

或是 在真機上被 SMM / BIOS policy / chipset lock 擋下來（讀可能還給你 0、寫直接拒絕）

4.4 WaitKey(&Key)

用途：阻塞等待一個鍵盤事件，回傳 EFI_INPUT_KEY

流程：

gBS->WaitForEvent(1, &gST->ConIn->WaitForKey, &Idx);

然後用 ReadKeyStroke 把按鍵讀出來

常見回傳：

EFI_SUCCESS

EFI_NOT_READY（理論上 WaitForEvent 後少見，但你 do/while 防住了）

EFI_DEVICE_ERROR（輸入裝置問題）

4.5 ReadHexByte(&OutValue)

用途：讀兩位 hex digit，組成一個 byte

ESC → 回 EFI_ABORTED

Backspace → 刪掉上一位

非 hex 字元 → 忽略

常見回傳：

EFI_SUCCESS：OutValue 得到 00~FF

EFI_ABORTED：使用者取消

EFI_INVALID_PARAMETER：OutValue == NULL

EFI_DEVICE_ERROR：鍵盤/輸入錯誤（少見）

4.6 DrawTable(sel)

用途：整屏重畫 CMOS grid，並把游標格反白
你現在是「每次移動都全畫面重畫」，所以：

好處：簡單、邏輯直

壞處：慢、閃（而且會一直大量讀 0x70/0x71）

5. Io.Read / Io.Write 的使用規則（必背版）

以 EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL.Io.Read/Write 為例：

5.1 Width / Address / Count / Buffer

Width 決定一次 I/O 的資料寬度（Uint8/16/32/64，或 FIFO/FILL 變體）

Address 是 I/O Port base（例如 0x70、0x71）

Count 是做幾次（每次 Address/Buffer 是否遞增看 Width 類型）

Buffer：

Read：目的地

Write：來源

5.2 Increment 規則（你貼的 spec 重點）

EfiPciWidthUint8/16/32/64：Address 和 Buffer 都會依 width 遞增

EfiPciWidthFifo...：Address 不動、Buffer 遞增

EfiPciWidthFill...：Address 遞增、Buffer 不動

你現在都用 EfiPciWidthUint8 + Count=1，是最單純安全的用法。

5.3 什麼情況會 EFI_INVALID_PARAMETER

最常見原因（真機常遇到）：

這個 Root Bridge 不支援該 Address I/O decode

平台限制某些 I/O port range（尤其 0x70/0x71、0xCF8/0xCFC、EC、SMBus…）

Width/Alignment 不被支援（你用 Uint8 通常不會是這個問題）

Buffer == NULL







cd /d D:\BIOS\MyWorkSpace\edk2

edksetup.bat Rebuild

chcp 65001

set PYTHONUTF8=1

set PYTHONIOENCODING=utf-8

rmdir /s /q Build\CmosRwAppPkg  

build -p CmosRwAppPkg\CmosRwAppPkg.dsc -a X64 -t VS2019 -b DEBUG
