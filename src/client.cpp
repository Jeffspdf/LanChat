#include "client.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QSplitter>
#include <QJsonArray>
#include <QSqlQuery>
#include <QSqlError>
#include <QStandardPaths>
#include <QDir>
#include <QCryptographicHash>
#include <QDateTime>
#include <QStyle>
#include <QShortcut>
#include <QSettings>
#include <QMenu>
#include <QSignalBlocker>
#include <QResizeEvent>
#include <QTimer>
#include <QDebug>
#include <QApplication>

// ---------------------------------------------------------------- 构造函数：搭界面
// 主窗口构造函数：按“网络对象、界面布局、事件连接、托盘、主题”的顺序初始化。
// 局部变量 auto 推导的是控件或布局指针；放入 Qt 布局/对象树后不需要在本函数末尾删除。
ChatWindow::ChatWindow() {
    setWindowTitle(QStringLiteral("LAN Chat · 局域网即时通讯"));
    resize(960, 680);

    // socket 是真正的 TCP 连接；this 作为父对象，窗口销毁时一起清理。
    socket = new QTcpSocket(this);
    // wire 包装同一个 socket，负责长度帧与 JSON；不是另一条网络连接。
    wire = new FramedSocket(socket, this);

    // root 是主窗口中央内容容器；QMainWindow 不能直接像普通 QWidget 一样设置顶层布局。
    auto root = new QWidget;
    // layout 是 root 的纵向主布局，依次承载顶部栏、页面和底部状态提示。
    auto layout = new QVBoxLayout(root);
    // 让主窗口接管 root；子控件通过布局组成可随窗口缩放的界面。
    setCentralWidget(root);

    // 顶部：品牌名 + 主题按钮
    // top 是顶部横向布局，品牌靠左，主题按钮靠右。
    auto top = new QHBoxLayout;
    // brand 只显示产品名称；QStringLiteral 为中文/Unicode 字符串提供明确的 Qt 字面量。
    auto brand = new QLabel(QStringLiteral("LAN Chat"));
    brand->setStyleSheet("font-size:24px;font-weight:700;");
    top->addWidget(brand);
    top->addStretch();
    // skin 是局部保存的主题按钮指针；后面 clicked 回调切换 dark。
    auto skin = new QPushButton(QStringLiteral("切换主题"));
    top->addWidget(skin);
    layout->addLayout(top);

    // pages 管理两个页面：按加入顺序，登录页为 0，聊天页为 1。
    pages = new QStackedWidget;
    // 伸缩因子 1 让页面优先占用额外的纵向空间。
    layout->addWidget(pages, 1);

    // ------------------------------------------------------ 登录页
    // auth 是登录页容器，不是认证请求数据；真正请求保存在 authRequest。
    auto auth = new QWidget;
    // a 是登录页纵向布局，前后 stretch 为表单提供弹性留白。
    auto a = new QVBoxLayout(auth);
    a->addStretch();

    // heading 是登录页说明标题，与聊天对象标题成员 title 不同。
    auto heading = new QLabel(QStringLiteral("连接局域网，开始聊天"));
    heading->setAlignment(Qt::AlignCenter);
    heading->setStyleSheet("font-size:22px;");
    a->addWidget(heading);

    // form 按“说明标签 + 输入框”两列组织服务器地址、端口、账号和密码。
    auto form = new QFormLayout;
    // host 保存服务器地址输入；它是控件，读取实际内容需要调用 text。
    host = new QLineEdit;
    // port 是端口文本框，默认 45454；连接前转换成数值并校验范围。
    port = new QLineEdit("45454");
    // account 是待认证账号输入，不能直接当作服务端已确认身份 me。
    account = new QLineEdit;
    // password 是密码输入控件；普通 TCP 仍不会因遮挡显示而获得加密。
    password = new QLineEdit;

    // settings 根据 main 设置的组织/应用名称访问持久设置。
    // 构造阶段读取上次地址/账号；登录成功分支写入本次地址/账号，不保存密码。
    QSettings settings;
    // 没有保存过服务器时使用本机回环地址，适合两个客户端在同机测试。
    host->setText(settings.value("host", "127.0.0.1").toString());
    account->setText(settings.value("account").toString());

    // 仅隐藏输入框中的明文显示，不是密码哈希计算或网络加密。
    password->setEchoMode(QLineEdit::Password);
    password->setMaxLength(128);
    account->setMaxLength(24);

    form->addRow(QStringLiteral("服务器 IP"), host);
    form->addRow(QStringLiteral("端口"), port);
    form->addRow(QStringLiteral("账号"), account);
    form->addRow(QStringLiteral("密码"), password);
    a->addLayout(form);

    // buttons 横向并排放登录与注册按钮；两按钮触发不同的认证请求类型。
    auto buttons = new QHBoxLayout;
    loginButton = new QPushButton(QStringLiteral("登录"));
    registerButton = new QPushButton(QStringLiteral("注册"));
    buttons->addWidget(loginButton);
    buttons->addWidget(registerButton);
    a->addLayout(buttons);

    // 注意：这里的 \n 是换行转义符，别漏了反斜杠
    // hint 是账号规则提示；下一段相邻字符串在编译期拼接，反斜杠 n 产生实际换行。
    auto hint = new QLabel(QStringLiteral(
        "账号：3–24 位字母、数字、下划线；密码至少 8 位\n"
        "同一服务器上的注册用户会出现在联系人列表中"));
    hint->setAlignment(Qt::AlignCenter);
    a->addWidget(hint);
    a->addStretch();
    pages->addWidget(auth);

    // ------------------------------------------------------ 聊天页
    // chat 是聊天页容器，登录成功后才显示。
    auto chat = new QWidget;
    // c 把聊天页分成左侧联系人与右侧会话区域。
    auto c = new QHBoxLayout(chat);

    // contacts 显示账号在线状态与未读数；真实账号单独放在列表项 UserRole。
    contacts = new QListWidget;
    contacts->setMinimumWidth(200);
    contacts->setMaximumWidth(280);
    c->addWidget(contacts);

    // conversation 纵向安排标题、消息列表、输入框和操作按钮。
    auto conversation = new QVBoxLayout;
    title = new QLabel(QStringLiteral("选择联系人"));
    title->setStyleSheet("font-size:20px;font-weight:600;");
    conversation->addWidget(title);

    // messages 仅展示当前会话的气泡；完整历史在 SQLite，不靠列表永久保存。
    messages = new QListWidget;
    // 关闭列表项整体选中；气泡 QLabel 仍可单独设置鼠标选择文字。
    messages->setSelectionMode(QAbstractItemView::NoSelection);
    messages->setSpacing(8);
    conversation->addWidget(messages, 1);

    // composer 是多行编辑区，发送时取纯文本，不发送富文本 HTML。
    composer = new QTextEdit;
    composer->setPlaceholderText(
        QStringLiteral("输入消息，Ctrl+Enter 发送（最多 4000 字）"));
    composer->setMaximumHeight(120);
    conversation->addWidget(composer);

    // actions 横向放置退出登录与发送按钮，中间 stretch 把两者分开。
    auto actions = new QHBoxLayout;
    // logout 触发 TCP 正常断开，之后 disconnected 回调负责清理会话。
    auto logout = new QPushButton(QStringLiteral("退出登录"));
    actions->addWidget(logout);
    actions->addStretch();
    sendButton = new QPushButton(QStringLiteral("发送"));
    actions->addWidget(sendButton);
    conversation->addLayout(actions);

    c->addLayout(conversation, 1);
    pages->addWidget(chat);

    status = new QLabel(QStringLiteral("请启动服务端，再注册或登录"));
    status->setWordWrap(true);
    layout->addWidget(status);

    // ------------------------------------------------------ 信号连接
    // 主题按钮点击后翻转 dark，再应用 QSS；this 是回调上下文和成员访问捕获。
    connect(skin, &QPushButton::clicked, this, [this] { dark = !dark; theme(); });
    // false 表示登录模式，不代表认证失败。
    connect(loginButton, &QPushButton::clicked, this, [this] { authenticate(false); });
    // true 表示注册模式，服务端注册成功后仍需要登录。
    connect(registerButton, &QPushButton::clicked, this, [this] { authenticate(true); });
    // 密码框回车复用登录按钮逻辑，不再另写一套网络代码。
    connect(password, &QLineEdit::returnPressed, loginButton, &QPushButton::click);

    // ★ 关键：连上之后才发认证请求，不能在 connectToHost 后面立刻 send
    // 连接建立是异步事件，只有此时再提交此前保存的 authRequest。
    connect(socket, &QTcpSocket::connected, this, [this] { wire->send(authRequest); });

    // TCP 断开后的回调，重置页面和身份；用户主动退出及对端断开都可能触发。
    connect(socket, &QTcpSocket::disconnected, this, [this] {
        resetSession();
        status->setText(QStringLiteral("连接已断开，可重新登录"));
    });

    // 处理连接/网络错误，避免按钮在失败后一直保持禁用。
    connect(socket, &QTcpSocket::errorOccurred, this,
            // 参数是错误枚举，本回调未命名使用它，而读取 socket->errorString 显示具体说明。
            [this](QAbstractSocket::SocketError) {
                status->setText(socket->errorString());
                loginButton->setEnabled(true);
                registerButton->setEnabled(true);
            });

    // 完整 JSON 到达后分发到 handle；半包与粘包已在 wire 内处理。
    connect(wire, &FramedSocket::received, this, &ChatWindow::handle);
    connect(logout, &QPushButton::clicked, socket, &QTcpSocket::disconnectFromHost);

    // 选择会话变化时刷新标题、未读数和历史，不在此发起新的网络连接。
    connect(contacts, &QListWidget::currentItemChanged, this,
            // item 是新选中的列表项；清空列表也可能产生空指针，所以先判空。
            [this](QListWidgetItem *item) {
                if (!item) return;
                selected = item->data(Qt::UserRole).toString();  // ★ 读真账号，不是显示文本
                // 把当前会话的内存未读数清零；不代表向服务端发送已读回执。
                unread[selected] = 0;
                title->setText(selected);
                loadHistory();
                // text 是当前联系人可见文本副本，包含在线符号，可能还包含未读角标。
                QString text = item->text();
                // marker 是未读角标起点；找不到为 -1，下面据此裁掉旧数量。
                int marker = text.indexOf(" [");
                if (marker >= 0) item->setText(text.left(marker));  // 清掉未读角标
            });

    // 发送按钮业务入口；检查联系人与正文，然后提交 chat 并等待保存确认。
    connect(sendButton, &QPushButton::clicked, this, [this] {
        // body 保存点击瞬间的正文快照；后续用户继续编辑不会改变这个局部值。
        QString body = composer->toPlainText();
        // 没有选中会话时拒绝发送，防止提交空收件账号。
        if (selected.isEmpty()) {
            status->setText(QStringLiteral("请先选择联系人"));
            return;
        }
        // trimmed 只用于判断全空白；实际发送 body 仍保留原始空格和换行。
        if (body.trimmed().isEmpty() || body.size() > 4000) {
            status->setText(QStringLiteral("消息须为 1–4000 字"));
            return;
        }
        if (wire->send({{"type", "chat"}, {"to", selected}, {"body", body}})) {
            // 记录等待确认的正文，收到服务端确认时用于判断输入框是否被用户重新修改。
            pendingBody = body;
            // 同一时刻只允许一个待确认发送，简化正文快照与确认的对应关系。
            sendButton->setEnabled(false);
            status->setText(QStringLiteral("消息已提交，等待服务器确认"));
        } else {
            status->setText(QStringLiteral("发送失败，请检查连接"));
        }
    });

    // shortcut 是窗口所属快捷键对象；Ctrl+Return 激活时复用发送按钮 click。
    auto shortcut = new QShortcut(QKeySequence("Ctrl+Return"), this);
    connect(shortcut, &QShortcut::activated, sendButton, &QPushButton::click);

    // ------------------------------------------------------ 系统托盘
    tray = new QSystemTrayIcon(style()->standardIcon(QStyle::SP_ComputerIcon), this);
    tray->setToolTip("LAN Chat");
    // menu 是托盘右键菜单，窗口销毁时由父子对象树清理。
    auto menu = new QMenu(this);
    // 菜单项触发后恢复普通窗口状态并请求激活；此回调不重新创建窗口。
    menu->addAction(QStringLiteral("显示窗口"), this, [this] {
        showNormal();
        activateWindow();
    });
    tray->setContextMenu(menu);
    connect(tray, &QSystemTrayIcon::activated, this,
            // 托盘激活原因参数未命名；当前实现不区分点击类型，都尝试显示窗口。
            [this](QSystemTrayIcon::ActivationReason) {
                showNormal();
                activateWindow();
            });
    if (QSystemTrayIcon::isSystemTrayAvailable()) tray->show();

    theme();
}

// ---------------------------------------------------------------- 发起登录/注册
// 发起认证。registration 区分注册与登录；本函数不等待服务器同步返回。
void ChatWindow::authenticate(bool registration) {
    // ok 接收 toInt 的转换结果；独立于真正的端口数值。
    bool ok = false;
    // number 暂用 int 保留输入，验证 1 到 65535 后才转换成 quint16。
    int number = port->text().toInt(&ok);
    if (!ok || number < 1 || number > 65535 || host->text().trimmed().isEmpty()) {
        status->setText(QStringLiteral("请输入正确的服务器地址和端口"));
        return;
    }
    socket->abort();   // ★ 断开旧连接，防止把密码发给上一台服务器

    // 构建连接成功后要发送的 JSON；type 根据布尔参数选取，密码只用于认证请求。
    authRequest = {{"type", registration ? "register" : "login"},
                   {"name", account->text().trimmed()},
                   {"password", password->text()}};
    // 保存这次连接的地址副本，不让后续界面输入改动影响历史身份标识。
    loginHost = host->text().trimmed();
    // 端口转为规范文本，用于显示和历史文件键；避免前导零造成不同文件名。
    loginPort = QString::number(number);

    loginButton->setEnabled(false);
    registerButton->setEnabled(false);
    status->setText(QStringLiteral("正在连接…"));

    socket->connectToHost(loginHost, quint16(number));   // 异步！返回时还没连上
}

// ---------------------------------------------------------------- 打开本地历史库
// 打开当前已登录身份的本地历史库；返回打开/建表是否成功。
bool ChatWindow::openHistory() {
    if (db.isOpen()) db.close();
    if (!db.isValid()) db = QSqlDatabase::addDatabase("QSQLITE", "client");

    // directory 是该应用可写数据目录，不应把历史强行放进可执行文件目录。
    QString directory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(directory);

    // 文件名 = SHA256(地址:端口/账号).db —— 换账号/换服务器自动分开存
    // key 是服务器地址、端口与账号组合的 SHA-256 十六进制文本，用作文件名。
    // 这里只是文件名隔离，不是给 SQLite 正文加密；服务器别名变化会生成另一份历史。
    QString key = QString::fromLatin1(
        QCryptographicHash::hash((loginHost + ":" + loginPort + "/" + me).toUtf8(),
                                 QCryptographicHash::Sha256).toHex());
    db.setDatabaseName(QDir(directory).filePath(key + ".db"));
    if (!db.open()) return false;

    // q 是此函数/分支的临时数据库查询对象，使用已打开的客户端 db 连接。
    QSqlQuery q(db);
    // 建立历史表：id 主键用于去重，sender/recipient/body/time 保存消息内容。
    // IF NOT EXISTS 不清除已有记录；exec 失败会把 false 返回给调用者。
    return q.exec("CREATE TABLE IF NOT EXISTS history("
                  "id TEXT PRIMARY KEY, sender TEXT, recipient TEXT, "
                  "body TEXT, time TEXT)");
}

// ---------------------------------------------------------------- 处理服务端来的消息
// 服务端消息统一分发入口。o 是一条完整 JSON，const 引用避免不必要的复制。
void ChatWindow::handle(const QJsonObject &o) {
    // type 取服务端消息类别，下面按类别更新不同状态，每个已处理分支及时 return。
    QString type = o["type"].toString();

    // 错误和注册完成都恢复认证按钮；注册完成只提示登录，不直接进入聊天页。
    if (type == "error" || type == "registered") {
        pendingBody.clear();
        sendButton->setEnabled(true);
        status->setText(type == "error" ? o["message"].toString()
                                        : QStringLiteral("注册成功，请登录"));
        loginButton->setEnabled(true);
        registerButton->setEnabled(true);
        return;
    }

    // 只有服务端明确认证通过，才设置可信 me 并准备本地历史。
    if (type == "logged_in") {
        // me 记录服务端确认的账号，不直接使用登录输入框作为身份依据。
        me = o["name"].toString();
        // 本地历史无法保存时中止连接，避免之后无法保存却误确认入站消息。
        if (!openHistory()) {
            socket->abort();
            status->setText(QStringLiteral("无法打开本地历史数据库"));
            return;
        }
        // 切到聊天页；在自检中也用于模拟已经登录后的界面状态。
        pages->setCurrentIndex(1);
        // 清空密码控件，不等于执行密码内存安全擦除。
        password->clear();
        authRequest.remove("password");          // ★ 别把密码留在内存里
        // settings 根据 main 设置的组织/应用名称访问持久设置。
        // 构造阶段读取上次地址/账号；登录成功分支写入本次地址/账号，不保存密码。
        QSettings settings;
        settings.setValue("host", loginHost);
        settings.setValue("account", me);
        status->setText(QStringLiteral("已登录：%1 · %2:%3")
                            .arg(me, loginHost, loginPort));
        return;
    }

    // 全量联系人目录更新分支；重新创建列表，并尝试恢复此前选中的账号。
    if (type == "users") {
        // previous 备份当前会话账号，避免列表重建后丢失选中位置。
        QString previous = selected;
        QSignalBlocker blocker(contacts);        // ★ 防止 clear() 触发选中变化
        contacts->clear();
        // 逐个读取服务端数组元素；value 是当前 QJsonValue，不是列表下标。
        for (auto value : o["users"].toArray()) {
            // user 是当前账号的 JSON 对象，含 name 与 online 字段。
            auto user = value.toObject();
            // name 是这一项的真实账号，与画面上带在线符号的显示文本分开保存。
            QString name = user["name"].toString();
            if (name == me) continue;            // ★ 跳过自己
            // item 是新建联系人列表项指针；构造时传 contacts 即加入并由列表管理。
            auto item = new QListWidgetItem(
                (user["online"].toBool() ? QStringLiteral("● ")
                                         : QStringLiteral("○ "))
                    + name
                    + (unread.value(name) ? QString(" [%1]").arg(unread[name]) : ""),
                contacts);
            item->setData(Qt::UserRole, name);   // ★ 真账号藏在 UserRole
            if (name == previous) contacts->setCurrentItem(item);
        }
        return;
    }

    // 非聊天消息或当前未登录时不进入消息保存流程。
    if (type != "message" || me.isEmpty()) return;

    // from 为服务端给出的发送账号，用于区分自己发出的确认与对方入站消息。
    QString from = o["from"].toString();
    // to 为收件账号；to==me 时才需要由本客户端回复收件保存 ACK。
    QString to   = o["to"].toString();
    // id 为服务端生成的消息唯一标识，history 主键及 ACK 都使用它。
    QString id   = o["id"].toString();

    // q 是此函数/分支的临时数据库查询对象，使用已打开的客户端 db 连接。
    QSqlQuery q(db);
    q.prepare("INSERT OR IGNORE INTO history VALUES(?,?,?,?,?)");   // ★ 去重
    // 依序绑定五个字段，第一个是唯一 ID；所有输入作为参数，不拼接 SQL。
    q.addBindValue(id);
    // 绑定发送账号，对应 history 的 sender。
    q.addBindValue(from);
    // 绑定收件账号，对应 history 的 recipient。
    q.addBindValue(to);
    q.addBindValue(o["body"].toString());
    q.addBindValue(o["time"].toString());
    if (!q.exec()) {
        sendButton->setEnabled(true);
        status->setText(QStringLiteral("本地保存失败：") + q.lastError().text());
        return;
    }

    // 自己发出去的：服务器已保存，可以清空输入框了
    // 这是自己发送消息的服务器确认；到达此处也已成功执行本地保存。
    if (from == me) {
        if (composer->toPlainText() == pendingBody) composer->clear();  // ★ 用户改过就不清
        pendingBody.clear();
        sendButton->setEnabled(true);
    }

    bool fresh = q.numRowsAffected() > 0;        // false = 重复消息，不重复显示

    if (to == me) wire->send({{"type", "ack"}, {"id", id}});  // ★ 重复也要 ack

    if (!fresh) return;

    // other 是会话对方：自己发出取 to，对方发来取 from。
    QString other = (from == me) ? to : from;
    // 属于当前会话时直接追加气泡，不增加其他会话的未读数。
    if (other == selected) {
        bubble(from, o["body"].toString(), o["time"].toString());
        messages->scrollToBottom();
    // 不是当前会话且确实是发给自己的新消息，才增加对方未读数量。
    } else if (to == me) {
        // 该联系人本次登录的未读数加一；QMap 的 operator[] 可为新键创建初始整数。
        ++unread[other];
        // 遍历联系人列表，寻找账号为 other 的项并更新未读角标；i 是列表下标。
        // 比较 UserRole 而不是可见文字，因此在线符号和旧数字不会影响查找。
        for (int i = 0; i < contacts->count(); ++i) {
            // item 是当前遍历的已有联系人项，只读取/更新，不重新创建。
            auto item = contacts->item(i);
            if (item->data(Qt::UserRole).toString() == other) {
                // text 是当前联系人可见文本副本，包含在线符号，可能还包含未读角标。
                QString text = item->text();
                // marker 是未读角标起点；找不到为 -1，下面据此裁掉旧数量。
                int marker = text.indexOf(" [");
                if (marker >= 0) text = text.left(marker);
                item->setText(text + QString(" [%1]").arg(unread[other]));
            }
        }
    }

    // 新入站消息在窗口不活跃或非当前会话时请求系统提醒，避免只靠气泡提示。
    if (to == me && (!isActiveWindow() || other != selected))
        tray->showMessage(from, o["body"].toString().left(100));

    status->setText(from == me
                        ? QStringLiteral("服务器已保存消息（离线联系人将在登录后收到）")
                        : QStringLiteral("收到 %1 的消息").arg(from));
}

// ---------------------------------------------------------------- 加载与某人的历史
// 显示 me 与 selected 的最近 200 条本地历史；只查本地，不请求服务器历史。
void ChatWindow::loadHistory() {
    messages->clear();
    // q 是此函数/分支的临时数据库查询对象，使用已打开的客户端 db 连接。
    QSqlQuery q(db);
    // 内层按时间降序和 rowid 降序取最近 200 条；外层按时间和 seq 升序展示。
    // 双方向 WHERE 同时覆盖我发给对方、对方发给我；seq 是 rowid 的查询别名。
    q.prepare("SELECT sender,body,time FROM ("
              "SELECT rowid AS seq,* FROM history "
              "WHERE (sender=? AND recipient=?) OR (sender=? AND recipient=?) "
              "ORDER BY time DESC,rowid DESC LIMIT 200) "
              "ORDER BY time,seq");
    // 按当前 SQL 顺序绑定自己的账号；双向会话查询需要出现两次 me。
    q.addBindValue(me);
    // 按顺序绑定对方账号；两组发送/接收条件共需四个参数。
    q.addBindValue(selected);
    // 按顺序绑定对方账号；两组发送/接收条件共需四个参数。
    q.addBindValue(selected);
    // 按当前 SQL 顺序绑定自己的账号；双向会话查询需要出现两次 me。
    q.addBindValue(me);
    if (!q.exec()) {
        status->setText(q.lastError().text());
        return;
    }
    // 循环推进结果集，每次定位一条历史。没有更多记录时停止，不是一直轮询数据库。
    while (q.next())
        // SELECT 的三列依次是发送者、正文、时间；逐条绘制而不重复插入历史库。
        bubble(q.value(0).toString(), q.value(1).toString(), q.value(2).toString());
    messages->scrollToBottom();
}

// ---------------------------------------------------------------- 画一个消息气泡
// 绘制一个气泡。三个参数分别为发送账号、正文、UTC 时间文本；不负责落库或网络发送。
void ChatWindow::bubble(const QString &from, const QString &body, const QString &time) {
    // item 是消息列表项，负责列表布局空间；实际文字由下面的 QLabel 显示。
    auto item = new QListWidgetItem(messages);
    // label 是气泡内容控件，后面交给 messages 的 setItemWidget 管理。
    auto label = new QLabel;
    label->setTextFormat(Qt::PlainText);         // ★ 正文不会被当 HTML 执行
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);

    // clock 把 ISO 毫秒时间解析后转本地时区，格式化为月日时分秒；不修改原始存储时间。
    QString clock = QDateTime::fromString(time, Qt::ISODateWithMs)
                        .toLocalTime().toString("MM-dd HH:mm:ss");
    label->setText(from + " · " + clock + "\n" + body);
    label->setMargin(12);
    label->setStyleSheet(from == me
                             ? "background:#2563eb;color:white;border-radius:10px;"
                             : (dark ? "background:#334155;color:#f1f5f9;border-radius:10px;"
                                     : "background:#e2e8f0;color:#0f172a;border-radius:10px;"));

    // width 是气泡目标宽度：消息视口减去留白，并至少取 80；不是整个主窗口宽度。
    int width = qMax(80, messages->viewport()->width() - 32);
    label->setFixedWidth(width);
    // 根据指定宽度估算换行后高度，为列表项分配足够空间，避免长文字被裁切。
    item->setSizeHint(QSize(width, label->heightForWidth(width)));
    messages->setItemWidget(item, label);
}

// ---------------------------------------------------------------- 断开后重置
// 清除本次登录的界面/内存状态并关闭数据库连接；不删除历史文件或服务端账号。
void ChatWindow::resetSession() {
    // 回到登录页，避免断线后继续操作旧会话。
    pages->setCurrentIndex(0);
    me.clear();
    selected.clear();
    unread.clear();
    contacts->clear();
    messages->clear();
    composer->clear();
    title->setText(QStringLiteral("选择联系人"));
    pendingBody.clear();
    // 丢弃保留的认证请求字段；下一次 authenticate 会重新构造它。
    authRequest = {};
    sendButton->setEnabled(true);
    loginButton->setEnabled(true);
    registerButton->setEnabled(true);
    if (db.isOpen()) db.close();
}

// ---------------------------------------------------------------- 窗口缩放时重算气泡
// 窗口缩放后，按新消息视口宽度重新安排现有气泡；不会新增消息。
void ChatWindow::resizeBubbles() {
    // width 是气泡目标宽度：消息视口减去留白，并至少取 80；不是整个主窗口宽度。
    int width = qMax(80, messages->viewport()->width() - 32);
    // 逐个遍历当前消息列表项；i 是下标，每个气泡都需要重新计算尺寸。
    for (int i = 0; i < messages->count(); ++i) {
        // item 指向当前已有消息项，用来取得其关联显示控件。
        auto item = messages->item(i);
        // label 尝试把项控件安全转换成 QLabel；类型不匹配返回 nullptr。
        auto label = qobject_cast<QLabel *>(messages->itemWidget(item));
        // 没有 QLabel 的项跳过，避免空指针访问；继续处理下一个列表项。
        if (!label) continue;
        label->setFixedWidth(width);
        // 根据指定宽度估算换行后高度，为列表项分配足够空间，避免长文字被裁切。
        item->setSizeHint(QSize(width, label->heightForWidth(width)));
    }
}

// event 描述本次尺寸变化；这是 Qt 调用的虚函数回调，不需手动循环检测窗口。
void ChatWindow::resizeEvent(QResizeEvent *event) {
    // 先保留基类的事件处理，再安排本窗口自己的气泡排版。
    QMainWindow::resizeEvent(event);
    QTimer::singleShot(0, this, [this] { resizeBubbles(); });   // 等布局稳定后再算
}

// ---------------------------------------------------------------- 主题
// 根据 dark 选择整窗 QSS；相邻字符串由编译器拼接，不需要运行时循环。
// 样式里的 QWidget 等是控件选择器，disabled 是按钮禁用状态的选择条件。
void ChatWindow::theme() {
    // 下面这几段用相邻字符串字面量拼接，等价于教程里那一整行
    setStyleSheet(dark
                      ? "QWidget{background:#0f172a;color:#e2e8f0;"
                        "font-family:'Microsoft YaHei';font-size:14px;} "
                        "QLineEdit,QTextEdit,QListWidget{background:#1e293b;"
                        "border:1px solid #475569;border-radius:6px;padding:8px;} "
                        "QPushButton{background:#2563eb;color:white;border:0;"
                        "border-radius:6px;padding:10px;} "
                        "QPushButton:disabled{background:#475569;}"
                      : "QWidget{background:#f8fafc;color:#0f172a;"
                        "font-family:'Microsoft YaHei';font-size:14px;} "
                        "QLineEdit,QTextEdit,QListWidget{background:white;"
                        "border:1px solid #cbd5e1;border-radius:6px;padding:8px;} "
                        "QPushButton{background:#2563eb;color:white;border:0;"
                        "border-radius:6px;padding:10px;} "
                        "QPushButton:disabled{background:#94a3b8;}");
    // 若有当前会话，重建气泡以应用新配色；不修改数据库正文。
    if (!selected.isEmpty()) loadHistory();
}

// ---------------------------------------------------------------- 界面自检
// 独立界面回归入口：创建测试窗口和内存数据库，通过直接注入 JSON 检查行为。
// 任意断言式条件失败就 return false；不是测试真实服务器或全部操作系统通知。
bool ChatWindow::selfTest() {
    // window 是此自检局部实例；静态函数仍可访问类私有成员进行受控检查。
    ChatWindow window;
    // 隐藏测试窗口的托盘图标，减少对日常桌面的干扰。
    window.tray->hide();
    window.show();
    window.me = "alice";
    window.pages->setCurrentIndex(1);

    window.db = QSqlDatabase::addDatabase("QSQLITE", "client");
    // SQLite 特殊名称：使用内存数据库，不写入真实账号的历史文件。
    window.db.setDatabaseName(":memory:");
    if (!window.db.open()) return false;

    // query 用于创建测试表、稍后删表制造保存失败；与业务函数里的 q 各自独立。
    QSqlQuery query(window.db);
    if (!query.exec("CREATE TABLE history(id TEXT PRIMARY KEY, sender TEXT, "
                    "recipient TEXT, body TEXT, time TEXT)")) return false;

    // 直接模拟服务端用户列表，随后选中 bob，准备气泡与历史测试。
    window.handle({{"type", "users"},
                   {"users", QJsonArray{QJsonObject{{"name", "bob"}, {"online", true}}}}});
    window.contacts->setCurrentRow(0);

    // body 构造中文、HTML 样式文本及 600 个“中”，覆盖纯文本与长消息换行。
    QString body = QStringLiteral("你好 <b>纯文本</b> ") + QString(600, QChar(0x4e2d));
    // message 是模拟入站消息；先用 z-first 再用同时间 a-second，检查不按 UUID 字母排序。
    auto message = QJsonObject{{"type", "message"}, {"id", "z-first"},
                               {"from", "bob"}, {"to", "alice"},
                               {"body", body}, {"time", "2026-10-10T08:30:00.000Z"}};
    window.handle(message);
    window.handle(message);                       // 同一条发两次 → 去重
    if (window.messages->count() != 1) return false;

    // 使用另一 ID 与同一时间制造第二条消息，验证历史保留插入先后。
    message["id"] = "a-second";
    message["body"] = "second";
    window.handle(message);
    window.loadHistory();

    // first 是重载历史后的第一条气泡控件，检查它仍包含最初的长消息且使用纯文本。
    auto first = qobject_cast<QLabel *>(window.messages->itemWidget(window.messages->item(0)));
    if (!first || first->textFormat() != Qt::PlainText || !first->text().contains(body))
        return false;

    window.resize(640, 480);
    // 自检中主动处理已排队事件，使缩放与布局生效；业务代码不要用它代替正常异步设计。
    QApplication::processEvents();
    window.resizeBubbles();
    if (first->width() > window.messages->viewport()->width()
        || window.messages->item(0)->sizeHint().height() < first->heightForWidth(first->width()))
        return false;

    // index 是可选截图参数位置；存在时把当前测试界面保存到下一个参数路径。
    int index = QApplication::arguments().indexOf("--screenshot");
    if (index >= 0
        && !window.grab().save(QApplication::arguments().value(index + 1))) return false;

    // 准备待确认草稿；下面删除 history 表故意让消息保存失败，检查草稿不被清空。
    window.composer->setPlainText("unsaved draft");
    window.pendingBody = "unsaved draft";
    if (!query.exec("DROP TABLE history")) return false;   // 制造"保存失败"
    window.handle({{"type", "message"}, {"id", "failed-store"},
                   {"from", "alice"}, {"to", "bob"}, {"body", "unsaved draft"}});
    if (window.composer->toPlainText() != "unsaved draft"
        || !window.sendButton->isEnabled()) return false;   // 草稿不能被清掉

    // 测试退出/断开清理，随后逐个检查账号、选择项、列表、草稿和标题。
    window.resetSession();
    if (!window.composer->toPlainText().isEmpty() || !window.me.isEmpty()
        || !window.selected.isEmpty() || window.contacts->count()
        || window.messages->count()
        || window.title->text() != QStringLiteral("选择联系人")) return false;

    window.account->setText("alice");
    window.password->setText("test-password");
    // 测试登录请求构造，可能异步尝试连接配置地址；下面马上 abort，不等真实认证。
    window.authenticate(false);
    // authReady 只证明已构造 login 请求，不代表服务器认证成功。
    bool authReady = window.authRequest.value("type").toString() == "login";
    window.socket->abort();

    qInfo() << "UI self-test:" << (authReady ? "passed" : "failed");
    // 前面检查均未失败且请求构造正确才返回 true，供主入口转换为退出码。
    return authReady;
}
