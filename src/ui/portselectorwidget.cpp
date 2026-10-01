#include "portselectorwidget.h"

#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>

QString portKindName(int kind)
{
    return QStringList{QObject::tr("串口"), QObject::tr("TCP 客户端"),
                       QObject::tr("TCP 服务器"), QObject::tr("UDP")}.value(kind);
}

QString portDraftSummary(const PortDraft &draft)
{
    const auto &v = draft.values;
    switch (draft.kind) {
    case 0: return v.value("serial.port", QObject::tr("未选择串口")).toString()
                + " · " + v.value("serial.baud", "115200").toString();
    case 1: return v.value("client.host", "127.0.0.1").toString()
                + ":" + v.value("client.port", 9000).toString();
    case 2: return v.value("server.address", "127.0.0.1").toString()
                + ":" + v.value("server.port", 9000).toString();
    default: return v.value("udp.address", "127.0.0.1").toString()
                + ":" + v.value("udp.port", 9001).toString();
    }
}

PortSelectorWidget::PortSelectorWidget(QWidget *parent) : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 14, 16, 16);
    layout->setSpacing(12);
    auto *heading = new QLabel(tr("端口配置"));
    heading->setObjectName("SectionTitle");
    layout->addWidget(heading);

    auto *form = new QFormLayout;
    form->setVerticalSpacing(10);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_name = new QLineEdit;
    m_name->setObjectName("portName");
    m_name->setMaxLength(32);
    m_type = new QComboBox;
    m_type->setObjectName("portType");
    for (int i = 0; i < 4; ++i)
        m_type->addItem(portKindName(i), i);
    form->addRow(tr("名称"), m_name);
    form->addRow(tr("类型"), m_type);
    layout->addLayout(form);

    m_pages = new QStackedWidget;
    layout->addWidget(m_pages);
    auto makePage = [this]() {
        auto *page = new QWidget;
        auto *pageLayout = new QVBoxLayout(page);
        pageLayout->setContentsMargins(0, 0, 0, 0);
        pageLayout->setSpacing(10);
        m_pages->addWidget(page);
        return pageLayout;
    };
    auto makeForm = [](QVBoxLayout *box) {
        auto *f = new QFormLayout;
        f->setVerticalSpacing(10);
        f->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        box->addLayout(f);
        return f;
    };

    auto *serial = makePage();
    auto *serialForm = makeForm(serial);
    serialForm->addRow(tr("串口"), addText("serial.port", tr("输入端口，如 COM3")));
    serialForm->addRow(tr("波特率"), addChoice("serial.baud", {"9600", "38400", "115200", "230400", "921600"}, true));
    auto *advanced = new QToolButton;
    advanced->setObjectName("advancedSerial");
    advanced->setText(tr("高级参数"));
    advanced->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    advanced->setArrowType(Qt::RightArrow);
    advanced->setCheckable(true);
    serial->addWidget(advanced);
    auto *advancedPage = new QWidget;
    auto *advancedForm = new QFormLayout(advancedPage);
    advancedForm->setContentsMargins(0, 0, 0, 0);
    advancedForm->addRow(tr("数据位"), addChoice("serial.bits", {"5", "6", "7", "8"}));
    advancedForm->addRow(tr("校验"), addChoice("serial.parity", {"None", "Even", "Odd", "Mark", "Space"}));
    advancedForm->addRow(tr("停止位"), addChoice("serial.stop", {"1", "1.5", "2"}));
    advancedForm->addRow(tr("流控"), addChoice("serial.flow", {"None", "RTS/CTS", "XON/XOFF"}));
    serial->addWidget(advancedPage);
    advancedPage->hide();
    connect(advanced, &QToolButton::toggled, this, [advanced, advancedPage](bool checked) {
        advanced->setArrowType(checked ? Qt::DownArrow : Qt::RightArrow);
        advancedPage->setVisible(checked);
    });
    serial->addStretch();

    auto *client = makePage();
    auto *clientForm = makeForm(client);
    clientForm->addRow(tr("远端地址"), addText("client.host", tr("IP 或主机名")));
    clientForm->addRow(tr("远端端口"), addNumber("client.port", 1, 65535));
    auto *timeout = addNumber("client.timeout", 100, 60000);
    timeout->setSuffix(tr(" ms"));
    clientForm->addRow(tr("连接超时"), timeout);
    client->addStretch();

    auto *server = makePage();
    auto *serverForm = makeForm(server);
    serverForm->addRow(tr("本地地址"), addChoice("server.address", {"127.0.0.1", "0.0.0.0"}, true));
    serverForm->addRow(tr("监听端口"), addNumber("server.port", 0, 65535));
    serverForm->addRow(tr("客户端上限"), addNumber("server.clients", 1, 64));
    auto *peers = new QLabel(tr("暂无客户端\n启动监听后，在此管理连接与发送目标。"));
    peers->setObjectName("InfoBox");
    peers->setWordWrap(true);
    server->addWidget(peers);
    server->addStretch();

    auto *udp = makePage();
    auto *udpForm = makeForm(udp);
    udpForm->addRow(tr("本地地址"), addChoice("udp.address", {"127.0.0.1", "0.0.0.0"}, true));
    udpForm->addRow(tr("本地端口"), addNumber("udp.port", 0, 65535));
    udpForm->addRow(tr("目标地址"), addText("udp.target", tr("目标 IPv4 地址")));
    udpForm->addRow(tr("目标端口"), addNumber("udp.targetPort", 1, 65535));
    udp->addStretch();

    // Page height follows the selected form rather than the largest hidden page.
    connect(m_pages, &QStackedWidget::currentChanged, this, [this](int index) {
        for (int i = 0; i < m_pages->count(); ++i)
            m_pages->widget(i)->setSizePolicy(QSizePolicy::Preferred,
                i == index ? QSizePolicy::Preferred : QSizePolicy::Ignored);
        m_pages->updateGeometry();
    });
    auto *open = new QPushButton(tr("打开端口"));
    open->setObjectName("PrimaryButton");
    open->setEnabled(false);
    open->setToolTip(tr("当前为界面预览，尚未启用设备连接"));
    layout->addWidget(open);
    auto *note = new QLabel(tr("○  未连接"));
    note->setObjectName("Muted");
    note->setAlignment(Qt::AlignCenter);
    layout->addWidget(note);
    layout->addStretch();

    connect(m_type, &QComboBox::currentIndexChanged, m_pages, &QStackedWidget::setCurrentIndex);
    connect(m_type, &QComboBox::currentIndexChanged, this, [this, open](int kind) {
        open->setText(kind == 2 ? tr("开始监听") : kind == 3 ? tr("绑定端口") : tr("打开端口"));
        notifyChange();
    });
    connect(m_name, &QLineEdit::textChanged, this, &PortSelectorWidget::notifyChange);
}

QComboBox *PortSelectorWidget::addChoice(const QString &key, const QStringList &items, bool editable)
{
    auto *field = new QComboBox;
    field->setObjectName(key);
    field->addItems(items);
    field->setEditable(editable);
    field->setMinimumWidth(0);
    m_fields.insert(key, field);
    connect(field, &QComboBox::currentTextChanged, this, &PortSelectorWidget::notifyChange);
    return field;
}

QLineEdit *PortSelectorWidget::addText(const QString &key, const QString &placeholder)
{
    auto *field = new QLineEdit;
    field->setObjectName(key);
    field->setPlaceholderText(placeholder);
    field->setMaxLength(253);
    m_fields.insert(key, field);
    connect(field, &QLineEdit::textChanged, this, &PortSelectorWidget::notifyChange);
    return field;
}

QSpinBox *PortSelectorWidget::addNumber(const QString &key, int minimum, int maximum)
{
    auto *field = new QSpinBox;
    field->setObjectName(key);
    field->setRange(minimum, maximum);
    m_fields.insert(key, field);
    connect(field, &QSpinBox::valueChanged, this, &PortSelectorWidget::notifyChange);
    return field;
}

void PortSelectorWidget::setDraft(const PortDraft &draft)
{
    m_loading = true;
    m_draft = draft;
    m_name->setText(draft.name);
    m_type->setCurrentIndex(draft.kind);
    const QVariantMap defaults{{"serial.port", ""}, {"serial.baud", "115200"},
        {"serial.bits", "8"}, {"serial.parity", "None"}, {"serial.stop", "1"}, {"serial.flow", "None"},
        {"client.host", "127.0.0.1"}, {"client.port", 9000}, {"client.timeout", 5000},
        {"server.address", "127.0.0.1"}, {"server.port", 9000}, {"server.clients", 64},
        {"udp.address", "127.0.0.1"}, {"udp.port", 9001}, {"udp.target", "127.0.0.1"}, {"udp.targetPort", 9002}};
    for (auto it = m_fields.cbegin(); it != m_fields.cend(); ++it) {
        const auto value = draft.values.value(it.key(), defaults.value(it.key()));
        if (auto *combo = qobject_cast<QComboBox *>(it.value()))
            combo->setCurrentText(value.toString());
        else if (auto *edit = qobject_cast<QLineEdit *>(it.value()))
            edit->setText(value.toString());
        else if (auto *spin = qobject_cast<QSpinBox *>(it.value()))
            spin->setValue(value.toInt());
    }
    m_loading = false;
}

PortDraft PortSelectorWidget::draft() const
{
    auto result = m_draft;
    result.name = m_name->text();
    result.kind = m_type->currentIndex();
    for (auto it = m_fields.cbegin(); it != m_fields.cend(); ++it) {
        if (auto *combo = qobject_cast<QComboBox *>(it.value()))
            result.values.insert(it.key(), combo->currentText());
        else if (auto *edit = qobject_cast<QLineEdit *>(it.value()))
            result.values.insert(it.key(), edit->text());
        else if (auto *spin = qobject_cast<QSpinBox *>(it.value()))
            result.values.insert(it.key(), spin->value());
    }
    return result;
}

void PortSelectorWidget::notifyChange()
{
    if (!m_loading)
        emit configurationChanged();
}
