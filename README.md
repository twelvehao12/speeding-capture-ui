# 车牌识别雷达测速摄像机管理软件

Windows 桌面端上位机，用于管理 RV1126B 车辆事件抓拍测速设备：设备搜索连接、RTSP 实时预览、主动拉取板端事件、evidence 缓存回看、设备配置与校时、FTP 配置与历史任务、事件 CSV 导出。

技术栈：C++17、Qt 6 Widgets、CMake、SQLite、Qt Multimedia（FFmpeg backend）。

## 当前状态

- 设备发现、连接、Token 保存、RTSP 主/辅码流预览已可用。
- 实时预览支持主码流 2560×1440、辅码流 1920×1080 的配置目标，两路可独立选择 H.264/H.265 后应用；实际下发需板端适配新增码流配置接口，详见[适配交接文档](doc/RV1126B_主辅码流配置适配交接.md)。
- HTTP 主动拉取已实现：事件列表、事件详情、evidence/snapshot、ACK。
- 本地 SQLite 仓储按 `(device_id, event_id, track_id)` upsert，支持终态更新和分页查询。
- evidence 缓存支持在线补下载、离线未缓存提示。
- 设备配置、时间、FTP 配置、FTP 历史任务页面已集成。
- 模拟模式 `--mock` 保留，方便无设备联调。
- 未完成产品化：用户可读事件导出包、按最近 1/2/7 天拉取、多 PC 拉取 UI、bundle API 对接。

## 主要模块

```text
src/rv1126b/network/        BoardApiClient、设备发现
src/rv1126b/services/       BoardEventSyncService、BoardEvidenceCache、FTP 服务
src/rv1126b/application/    EventViewController、设备操作控制器
src/rv1126b/storage/        SQLite 事件仓储
src/ui/                     主窗口、设备管理、实时预览、系统设置
tests/                      Qt Test 单元测试
doc/                        RV1126B 接入 API、联调与验证报告
```

## 构建

本机推荐 Qt 6.11.1 MinGW preset：

```powershell
cmake --preset mingw-debug
cmake --build --preset mingw-debug
ctest --preset mingw-debug
```

正式包：

```powershell
cmake --preset mingw-release
cmake --build --preset mingw-release
ctest --preset mingw-release
```

## 运行

默认真实设备模式：

```powershell
build\mingw-release\CameraManagerApp.exe
```

模拟模式：

```powershell
build\mingw-release\CameraManagerApp.exe --mock
```

## 便携包

构建 release 后运行 Qt 的 `windeployqt` 收集运行库：

```powershell
D:\Qt\6.11.1\mingw_64\bin\windeployqt.exe build\mingw-release\CameraManagerApp.exe
```

再把 `build\mingw-release` 中应用和运行库目录打成 zip 即可。

## 相关仓库

性能优化、诊断开关及验收步骤见 [2K 预览与快速操作优化实施记录](doc/2K预览与快速操作优化实施记录.md)。

- 板端管线：`Felosefe/RV1126B_CAM`
