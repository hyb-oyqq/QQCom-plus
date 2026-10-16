#include "apptheme.h"

#include <QApplication>
#include <QFile>
#include <QStyleHints>

AppTheme::AppTheme(QObject *parent) : QObject(parent) {}

AppTheme *AppTheme::instance()
{
    static AppTheme *theme = new AppTheme(qApp);
    return theme;
}

bool AppTheme::isDark()
{
    return instance()->m_dark;
}

AppTheme::Mode AppTheme::mode()
{
    return instance()->m_mode;
}

AppTheme::Mode AppTheme::modeFromKey(const QString &key)
{
    if (key == QLatin1String("light")) return Mode::Light;
    if (key == QLatin1String("dark")) return Mode::Dark;
    return Mode::System;
}

QString AppTheme::modeKey(Mode mode)
{
    switch (mode) {
    case Mode::Light: return QStringLiteral("light");
    case Mode::Dark: return QStringLiteral("dark");
    case Mode::System: break;
    }
    return QStringLiteral("system");
}

void AppTheme::install(QApplication *app)
{
    auto *theme = instance();
    if (theme->m_installed || !app) return;
    theme->m_installed = true;
    theme->applyResolved();
    connect(app->styleHints(), &QStyleHints::colorSchemeChanged, theme, [theme](Qt::ColorScheme) {
        // 手动钉住亮色或暗色时，系统切换不能把样式表换回去。
        if (theme->m_applying || theme->m_mode != Mode::System) return;
        const bool wasDark = theme->m_dark;
        theme->applyResolved();
        if (theme->m_dark != wasDark) emit theme->schemeChanged();
    });
}

void AppTheme::setMode(Mode mode)
{
    auto *theme = instance();
    if (theme->m_hasApplied && theme->m_mode == mode) return;
    theme->m_mode = mode;
    const bool wasDark = theme->m_dark;
    theme->applyResolved();
    if (theme->m_dark != wasDark) emit theme->schemeChanged();
}

void AppTheme::applyResolved()
{
    if (!qApp) return;
    m_applying = true;
    auto *hints = qApp->styleHints();
    if (m_mode == Mode::Light)
        hints->setColorScheme(Qt::ColorScheme::Light);
    else if (m_mode == Mode::Dark)
        hints->setColorScheme(Qt::ColorScheme::Dark);
    else
        hints->unsetColorScheme();

    const bool dark = hints->colorScheme() == Qt::ColorScheme::Dark;
    // 同一明暗再套一遍整份样式表会让每个控件重新求值，切换「跟随系统」但系统本来就是
    // 当前这档时跳过。明暗变了必须换表：表里的颜色盖住调色板。
    if (!m_hasApplied || dark != m_dark) {
        m_dark = dark;
        QFile file(m_dark ? QStringLiteral(":/styles/workbench-dark.qss")
                          : QStringLiteral(":/styles/workbench.qss"));
        if (file.open(QIODevice::ReadOnly))
            qApp->setStyleSheet(QString::fromUtf8(file.readAll()));
    }
    m_hasApplied = true;
    m_applying = false;
}
