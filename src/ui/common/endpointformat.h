#ifndef ENDPOINTFORMAT_H
#define ENDPOINTFORMAT_H

#include <QString>
#include "domain/portdraft.h"

// 端点的显示格式化：类型名、行标识（"A · 串口"）、草稿摘要与动作文案。
// 这些是纯函数，不含 QWidget；监视、发送、路由面板与端点行共用，
// 不需要为此 include 端点选择控件本身。

// Endpoint identity is derived from the row position and type ("A · 串口"). It is
// displayed, never edited, so no configuration field holds a user-supplied name.
QString endpointLabel(int index, int kind);

QString portKindName(int kind);
QString portDraftSummary(const PortDraft &draft);
QString portDraftDetails(const PortDraft &draft);

// Closed: the action to take. Open: the action that stops it.
QString portActionName(int kind, bool open);

// The glyph beside that text: a link for the actions that dial out, an access point for the
// ones that listen, and a broken link once the endpoint is open.
QString portActionIconName(int kind, bool open);

#endif
