#ifndef APPTHEME_H
#define APPTHEME_H

#include <QObject>
#include <Qt>

class QApplication;

// 浅色 / 深色。工作台样式把底色和字色写死了，写死的值盖住调色板，所以必须换整份 qss。
// Qt 6.5 起 Fusion 会读 QStyleHints::colorScheme()。手动选亮色或暗色时把这个值钉住，
// 选「跟随系统」时放开，系统切换会重刷调色板并换回对应的那份样式表。
// 弹窗不再单独写一套浅色字：它们吃当前这份应用级样式表。
// 画进 QTextDocument 的字色样式表管不到，那些调用点读 isDark() 现取。
class AppTheme : public QObject
{
    Q_OBJECT
public:
    enum class Mode { System, Light, Dark };

    static AppTheme *instance();
    static bool isDark();
    static Mode mode();

    // 按当前选择加载样式表。跟随系统时，系统切换会重载。只在程序入口调一次。
    static void install(QApplication *app);
    // 亮色、暗色或跟随系统。外观菜单和启动时恢复的偏好都走这里。
    static void setMode(Mode mode);
    static Mode modeFromKey(const QString &key);
    static QString modeKey(Mode mode);

signals:
    void schemeChanged();

private:
    explicit AppTheme(QObject *parent = nullptr);
    void applyResolved();

    Mode m_mode = Mode::System;
    bool m_dark = false;
    bool m_installed = false;
    bool m_hasApplied = false;
    bool m_applying = false;
};

#endif
