# edk2-PCI_ROOT_BRIDGE_IO_PROTOCOL
usd PCI_ROOT_BRIDGE_IO_PROTOCOL
---

# README — CMOS Editor (UEFI Shell / EDK2)

## 1. 這支程式在做什麼

* 在 UEFI Shell 內顯示 **CMOS 0x00~0x7F (128 bytes)** 的表格（8x16）
* 方向鍵移動游標
* Enter 進入寫入模式（限制寫入範圍：**0x30~0x3F**）
* 輸入 2 位 Hex（支援 Backspace/ESC 取消）
* 寫入後讀回驗證、重畫表格

---

## 2. 關鍵硬體背景：為什麼是 0x70 / 0x71

* `0x70` = CMOS/RTC **Index Port**：先把要讀/寫的 CMOS Offset 寫到這裡
* `0x71` = CMOS/RTC **Data Port**：再從這裡讀出或寫入該 Offset 的值

典型流程（1 byte）：

1. `OUT 0x70, index`
2. `IN  0x71 -> data`

寫入則是：

1. `OUT 0x70, index`
2. `OUT 0x71, value`

---

## 3. 你這份程式「最重要的依賴」

### 3.1 EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL（你目前在用）

你用的是：

* `PciRootBridgeIo->Io.Write()` / `PciRootBridgeIo->Io.Read()`

它的設計目標是：**透過 Root Bridge 去做 I/O Port Access**（以及 PCI config access）。

**重點：它不是保證每個 port 都能讀寫。**
0x70/0x71 在很多真機上會因為平台策略、I/O decode、SMM 保護、晶片組鎖而失敗或回固定值。

---

## 4. 函數使用方法（你程式裡每個核心函數怎麼用）

### 4.1 `InitRootBridgeIo()`

用途：取得 `EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL` 實例到 `mRbIo`

你實作：

* `gBS->LocateProtocol(&gEfiPciRootBridgeIoProtocolGuid, NULL, (VOID**)&mRbIo);`

常見回傳：

* `EFI_SUCCESS`：拿到了
* `EFI_NOT_FOUND`：系統沒有安裝此協定（或你執行階段沒有）
* `EFI_INVALID_PARAMETER`：傳入指標錯（較少）

**筆記：**

* 真機通常「有」PciRootBridgeIo，但 **不代表它允許你對 0x70/0x71 做 I/O**。

---

### 4.2 `CmosRead8(Index)`

用途：讀 CMOS offset（0x00~0x7F）的一個 byte

你的流程：

1. 組 `Sel`（你用了 `0x80 | (Index & 0x7F)`）
2. `Io.Write(..., Address=0x70, Buffer=&Sel)`
3. `Io.Read (..., Address=0x71, Buffer=&Data)`
4. return `Data`

**注意點（超常見地雷）：**

* `0x80` 是 “Disable NMI” 的常見做法，但**有些平台/環境會讓你讀不到或讀出怪值**
  → 你可以做 A/B 測試：

  * `Sel = Index & 0x7F;`（先不要 OR 0x80）
  * 或者只在必要時才 OR 0x80

**更重要：你現在 CmosRead8 把 Status 直接丟掉了**
如果平台回 `EFI_INVALID_PARAMETER`，你會完全看不出來，只會看到表格一片 `00` 或固定值。

> 建議：CmosRead8 內一定要檢查 Status，至少 debug print 一次。

---

### 4.3 `CmosWrite8(Index, Value)`

用途：寫 CMOS offset 的一個 byte

你的流程：

1. `Io.Write(0x70, Sel)`
2. `Io.Write(0x71, Value)`

常見回傳：

* `EFI_SUCCESS`：寫成功
* `EFI_INVALID_PARAMETER`：最常見！多半是**平台不允許這個 port**、或這個 RootBridge 不 decode 0x70/0x71
* `EFI_OUT_OF_RESOURCES`：很少見

你遇到的「write failed status = invalid parameter」通常代表：

* **RootBridgeIo 介面本身不接受 Address=0x70/0x71**（I/O 範圍不屬於它、或被平台擋）
* 或是 **在真機上被 SMM / BIOS policy / chipset lock 擋下來**（讀可能還給你 0、寫直接拒絕）

---

### 4.4 `WaitKey(&Key)`

用途：阻塞等待一個鍵盤事件，回傳 `EFI_INPUT_KEY`

流程：

* `gBS->WaitForEvent(1, &gST->ConIn->WaitForKey, &Idx);`
* 然後用 `ReadKeyStroke` 把按鍵讀出來

常見回傳：

* `EFI_SUCCESS`
* `EFI_NOT_READY`（理論上 WaitForEvent 後少見，但你 do/while 防住了）
* `EFI_DEVICE_ERROR`（輸入裝置問題）

---

### 4.5 `ReadHexByte(&OutValue)`

用途：讀兩位 hex digit，組成一個 byte

* ESC → 回 `EFI_ABORTED`
* Backspace → 刪掉上一位
* 非 hex 字元 → 忽略

常見回傳：

* `EFI_SUCCESS`：OutValue 得到 00~FF
* `EFI_ABORTED`：使用者取消
* `EFI_INVALID_PARAMETER`：OutValue == NULL
* `EFI_DEVICE_ERROR`：鍵盤/輸入錯誤（少見）

---

### 4.6 `DrawTable(sel)`

用途：整屏重畫 CMOS grid，並把游標格反白
你現在是「每次移動都全畫面重畫」，所以：

* 好處：簡單、邏輯直
* 壞處：慢、閃（而且會一直大量讀 0x70/0x71）

---

## 5. Io.Read / Io.Write 的使用規則（必背版）

以 `EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL.Io.Read/Write` 為例：

### 5.1 Width / Address / Count / Buffer

* `Width` 決定一次 I/O 的資料寬度（Uint8/16/32/64，或 FIFO/FILL 變體）
* `Address` 是 I/O Port base（例如 0x70、0x71）
* `Count` 是做幾次（每次 Address/Buffer 是否遞增看 Width 類型）
* `Buffer`：

  * Read：目的地
  * Write：來源

### 5.2 Increment 規則（你貼的 spec 重點）

* `EfiPciWidthUint8/16/32/64`：**Address 和 Buffer 都會依 width 遞增**
* `EfiPciWidthFifo...`：**Address 不動、Buffer 遞增**
* `EfiPciWidthFill...`：**Address 遞增、Buffer 不動**

你現在都用 `EfiPciWidthUint8 + Count=1`，是最單純安全的用法。

### 5.3 什麼情況會 EFI_INVALID_PARAMETER

最常見原因（真機常遇到）：

* 這個 Root Bridge **不支援該 Address I/O decode**
* 平台限制某些 I/O port range（尤其 0x70/0x71、0xCF8/0xCFC、EC、SMBus…）
* Width/Alignment 不被支援（你用 Uint8 通常不會是這個問題）
* Buffer == NULL

---

## 6. 為什麼 0x70/0x71 在真機 UEFI Shell 常被擋

這是你目前「讀不到 / 寫回 INVALID_PARAMETER」的核心。

常見原因（由常見到較少見）：

1. **SMM / BIOS policy 保護 RTC/CMOS**

   * 真機會用 SMM 攔截或限制 Legacy I/O
   * 讀可能回 0 或固定值、寫直接拒絕
2. **PciRootBridgeIo 的 Io() 不是「CPU port I/O 的無條件代理」**

   * 有些平台只允許它對某些 range 做 decode
3. **UEFI Shell 的執行環境/權限限制**

   * 某些 BIOS 版本對 Shell app 做了保護
4. **RTC index bit7 (NMI disable) 的行為差異**

   * OR 0x80 可能讓你在特定平台讀不到
5. **硬體已切到 ACPI/SMI 特定模式，CMOS access path 被重導或封鎖**

> 結論：你程式邏輯可能完全正確，但平台就是不讓你用這條路讀寫 0x70/0x71。

---

## 7. 你這份程式的流程圖式摘要（文字版）

```
UefiMain
 ├─ InitRootBridgeIo()
 │   └─ LocateProtocol(PciRootBridgeIo)
 ├─ sel=0
 ├─ DrawTable(sel)
 │   └─ for idx=0..0x7F:
 │       └─ CmosRead8(idx) -> 印出 val（選取格反白）
 └─ while(1):
     ├─ WaitKey(&Key)
     ├─ if ESC -> break
     ├─ if Arrow -> 更新 sel -> DrawTable(sel)
     └─ if Enter:
         ├─ if sel not in 0x30..0x3F -> 顯示 blocked
         ├─ oldv = CmosRead8(sel)
         ├─ ReadHexByte(&newv)  (ESC cancel)
         ├─ Status = CmosWrite8(sel, newv)
         ├─ if error -> 顯示 "Write failed: %r"
         ├─ verify = CmosRead8(sel)
         └─ DrawTable(sel)
```

---

## 8. 建議你立刻加上的「最有用除錯點」

你目前最大的痛點是：**Read/Write 的 Status 被丟掉**，導致你只看到結果是 00 或寫入失敗，卻不知道是哪一步 fail。

建議做法（概念）：

* 在 `CmosRead8()` 裡：

  * 把 `Io.Write` / `Io.Read` 的 Status 印出來（至少第一次出錯時印一次）
* 在 `CmosWrite8()` 裡：

  * 已有 Status return，OK
  * 但建議把「是哪個 port 失敗」也印出來（0x70 or 0x71）

這樣你會立刻確認：

* 是 `Write 0x70` 就 `EFI_INVALID_PARAMETER`？
* 還是 `Read 0x71` 才 `EFI_INVALID_PARAMETER`？
* 或者其實都成功但讀回就是 0（代表被 SMM 置換/遮蔽）

---
cd /d D:\BIOS\MyWorkSpace\edk2

edksetup.bat Rebuild

chcp 65001

set PYTHONUTF8=1

set PYTHONIOENCODING=utf-8

rmdir /s /q Build\CmosRwAppPkg  

build -p CmosRwAppPkg\CmosRwAppPkg.dsc -a X64 -t VS2019 -b DEBUG
