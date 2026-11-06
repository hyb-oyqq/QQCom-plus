# 服务层聚合入口：QQCom.pro 与 ui_smoke.pro 保持原有 include 路径不变。
# 想要更细粒度的目标（如 M1 的无 UI 核心测试）可直接 include 子模块 .pri：
# domain（纯值类型）/ transport（传输层，自带 QT += network）/ app（运行时）/
# discovery（串口扫描）/ persistence（QSettings 偏好）。
include($$PWD/domain/domain.pri)
include($$PWD/transport/transport.pri)
include($$PWD/app/app.pri)
include($$PWD/discovery/discovery.pri)
include($$PWD/persistence/persistence.pri)
include($$PWD/automation/automation.pri)
