# Better-IN-Q 双轨测试与真桌面验证说明

Better-IN-Q 包含覆盖逻辑、Hook、配置、C ABI 与图形/输入拦截的 **27 项自动化测试**。其中 5 项桌面交互测试（`cursor_test`、`ime_test`、`window_chain_test`、`overlay_test`、`overlay_dx12_test`）在 Windows 锁屏（`WinSta0\Winlogon`）或无头 CI 环境下无法直接调用交互桌面 API；为此仓库采用**无头快速回归 + 真桌面/虚拟机整链验收**的双轨方案。

---

## 1. 为什么需要双轨测试

当 Windows 宿主机锁屏、处于非交互会话或无头构建环境时，底层会拒绝以下三类交互：

1. **DXGI 交换链创建**：带窗口句柄的 `CreateSwapChain*` 返回 `0x887A0022`（`DXGI_ERROR_NOT_CURRENTLY_AVAILABLE`），即使选择 WARP 软件设备也会失败；
2. **全局光标控制**：`SetCursorPos` / `ClipCursor` / `GetCursorPos` 被安全桌面静默忽略或拒绝；
3. **IMM32 输入法上下文**：`ImmCreateContext()` 返回 `NULL`，无法运行真实组字与候选窗消息校验。

为了既不降低本地日常回归效率，又不放弃真硬件/真桌面路径覆盖，5 项测试统一拆分为两套执行路径：

| 轨道 | 运行环境 | 触发方式 | 验证范围 |
| :--- | :--- | :--- | :--- |
| **轨道 1：无头 / 锁屏回归** | 任何环境（锁屏、SSH/后台、CI 均可） | 自动探测桌面锁定，或显式传入 `--headless` / `BETTERINQ_HEADLESS=1` | 状态机、配置、Hook、假 IMM/光标桩、`IDXGISwapChain3` 离屏 WARP 虚表与覆盖层渲染链路（全套 27/27 通过） |
| **轨道 2：真桌面 / 虚拟机验收** | 常驻解锁的物理桌面或 Hyper-V GPU-P 虚拟机 Session 1 | **不设置** `--headless` 与 `BETTERINQ_HEADLESS`，直接在交互桌面运行 5 项测试 `.exe` | 真实硬件 D3D11/D3D12 交换链、`ResizeBuffers` 重建、真实 Win32 光标与 `IMM32` 生命周期（零跳过、零无头回退） |

---

## 2. 轨道 1：无头测试设计与桩接口

默认运行完整构建与全部 27 项测试：

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1 all
```

也可以强制对单个桌面测试启用无头模式（即使当前桌面已解锁）：

```powershell
out\overlay_test.exe out --headless
# 或通过环境变量控制全部测试
$env:BETTERINQ_HEADLESS = "1"
powershell -ExecutionPolicy Bypass -File build.ps1 test
```

各测试接缝如下：

- **离屏 DXGI 交换链桩（[`tests/offscreen_swapchain.h`](../tests/offscreen_swapchain.h)）**
  - 实现完整的 `IDXGISwapChain3` 虚表并将实现隐藏在抽象接口后，防止 MSVC `/O2` 跨调用点去虚化；
  - 底层结合可用 WARP `ID3D11Device` / `ID3D12Device` 与离屏 `RenderTarget` 资源运行，不依赖 `HWND` 呈现桌面；
  - 通过 [`overlay::InstallOnSwapChain`](../src/overlay.h) 直接挂接假交换链虚表，完整覆盖 `Present`、`ResizeBuffers` 与退出清理流程。
- **光标测试接缝（[`cursorfilter::TestOps`](../src/cursorfilter.h) / `SetTestOps`）**
  - 允许在无头模式下注入内存虚拟屏幕坐标与模拟的 `SetCursorPos` / `GetCursorPos` / `ClipCursor` 函数表，独立校验放行/拦截/恢复状态机；桌面解锁且未指定 `--headless` 时追加真实 `user32` 冒烟。
- **IMM32 测试接缝（`overlay::ImmOps` / `ime_test`）**
  - 始终运行无桌面句柄泄漏的假 IMM 状态机验证；桌面解锁且未指定 `--headless` 时追加真实 Win32 `IMM32` 窗口与 `WM_IME_*` / `ImmSetCompositionWindow` 消息流校验。

---

## 3. 轨道 2：通过 Hyper-V GPU-P 虚拟机跑真桌面测试

如果宿主机经常处于锁屏、远程或无人值守状态，建议搭配一台独立的 **Hyper-V 第 2 代 Windows 11 GPU-P（GPU 分区）测试虚拟机**，让其在后台始终保持 Session 1 自动登录解锁桌面：

### 3.1 虚拟机关键配置要求

1. **宿主机与 Guest 系统 Build 同代**：使用与宿主机一致的 Windows 11 Build（避免 WDDM 版本差导致 GPU-P 设备出现 Code 43）。
2. **GPU-P 硬性要求**：
   - 使用固定**静态内存**（关闭动态内存）并禁用检查点（`CheckpointType = Disabled`）；
   - 分配 `Add-VMGpuPartitionAdapter` 与充足的 MMIO 空间（`LowMemoryMappedIoSpace` / `HighMemoryMappedIoSpace`）；
   - 将宿主机显卡驱动包（含 OpenGL/Vulkan 组件如 `amdogl` / `amdvlk`，避免依赖 GL/CEF 的进程缺失库报错）同步至 Guest 的 `C:\Windows\System32\HostDriverStore\FileRepository\`。
3. **常驻解锁桌面（Session 1）**：
   - 配置本地测试账号 `AutoLogon`、禁用锁屏与熄屏休眠（`NoLockScreen=1`、`powercfg`），保持固定 `1920x1080` 控制台显示输出；
   - 日常查看或操作 VM 画面时使用 Hyper-V VMConnect **基本会话（取消勾选“增强会话”）**，避免增强会话 RDP 锁定 Session 1。
4. **跨会话投递执行（关键）**：
   - PowerShell Direct（`Invoke-Command -VMName ...`）默认处于 **Session 0**，无法访问图形桌面，直接运行仍会触发 `0x887A0022`；
   - 测试编排脚本应通过 `Register-ScheduledTask` 注册 `LogonType = Interactive` 的计划任务将测试进程精准投递到已解锁的 **Session 1** 桌面运行，并回收标准输出、退出码与渲染截图。

### 3.2 真桌面验收标准

在 VM Session 1 交互桌面中，**不设置 `--headless` 与 `BETTERINQ_HEADLESS`** 依次运行以下 5 项测试：

```powershell
out\cursor_test.exe
out\ime_test.exe
out\window_chain_test.exe
out\overlay_test.exe out
out\overlay_dx12_test.exe out
```

验收要求：
- 退出码全部为 `0`；
- 输出中**不得出现**回退日志（如 `live cursor smoke skipped`、`live Win32 IMM pass skipped` 或无头/离屏交换链回退提示）；
- `overlay_test` 与 `overlay_dx12_test` 需基于真实硬件 D3D11/D3D12 交换链完成 `1080p` / `720p` `ResizeBuffers` 重建、像素回读及真渲染 PNG 截图导出。

### 3.3 宿主机仍建议复核的边界

- **真实物理显示器 HDR10 / scRGB 色域握手**：Hyper-V 虚拟显示器默认上报 SDR 色域；物理外接 HDR 显示器的 PQ / scRGB 输出建议在宿主机实机复核一次。
- **极限帧率与微卡顿基准**：GPU-P 存在虚拟化调度开销，纯性能基准应以物理宿主机数据为准。
