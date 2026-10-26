#include "server.h"
#include <QSqlQuery>
#include <QSqlError>
#include <QJsonArray>
#include <QDateTime>
#include <QDir>
#include <QUuid>
#include <QRegularExpression>
#include <QDebug>
#include <QtConcurrent/QtConcurrentRun>
#include <QFutureWatcher>
#include <QPointer>

// 构造函数。p 是可选 Qt 父对象，初始化列表 QObject(p) 先构造基类。
// 这里只建立事件连接，不打开数据库、不监听端口。
ChatServer::ChatServer(QObject *p) : QObject(p) {
    // 监听器发现新连接时调用本对象的 accept；this 同时是接收对象和生命周期上下文。
    connect(&listener, &QTcpServer::newConnection, this, &ChatServer::accept);
}
// 启动服务端。port 是监听端口，directory 是数据库目录；const 引用避免复制目录文本。
// 成功完成目录、数据库和监听初始化返回 true；失败返回 false。
bool ChatServer::start(quint16 port, const QString &directory) {
    // 创建目录及缺失的上级目录；目录已存在也可成功，失败则不继续打开数据库。
    if (!QDir().mkpath(directory))
        return false;
    // 添加名为 server 的 SQLite 连接；连接名区分 Qt 进程内的连接，不是数据库文件名。
    db = QSqlDatabase::addDatabase("QSQLITE", "server");
    // 把目录与文件名安全拼成数据库路径；SQLite 文件可以在打开时新建。
    db.setDatabaseName(QDir(directory).filePath("server.db"));
    // 实际打开数据库，失败时输出诊断并停止启动，不能带着无效数据库继续聊天。
    if (!db.open()) {
        // 打印 SQLite 打开失败原因，例如路径不可写或驱动无法加载。
        qCritical() << db.lastError();
        return false;
    }
    // q 是绑定到服务端 db 的临时查询对象；用于本作用域中的 SQL 执行及结果读取。
    // 不同函数/分支中的同名 q 是不同局部变量，不是共享的全局查询。
    QSqlQuery q(db);
    // 依次创建表和索引。任一步失败时，逻辑或 || 短路，使整个条件为 true 并返回失败。
    // IF NOT EXISTS 保留已有表和数据，不会每次启动清空聊天记录。
    if (!q.exec(
            // users：name 是唯一账号；salt 保存十六进制盐；hash 保存密码派生值，不存原始密码。
            "CREATE TABLE IF NOT EXISTS users(name TEXT PRIMARY KEY,salt TEXT NOT NULL,hash TEXT NOT NULL)") ||
        !q.exec(
            // messages：id 为消息唯一 ID；sender/recipient 是双方账号；body 是正文；time 是 UTC 时间。
            // delivered 默认 0，等待收件客户端保存确认后改为 1，不表示对方已经阅读。
            "CREATE TABLE IF NOT EXISTS messages(id TEXT PRIMARY KEY,sender TEXT NOT NULL,recipient TEXT NOT NULL,body TEXT NOT NULL,time TEXT NOT NULL,delivered INTEGER DEFAULT 0)") ||
        // 为收件人、确认状态和时间建立组合索引，帮助筛选待确认消息。
        // 当前 pending 最终按 rowid 排序；此索引不是完整覆盖该排序的性能保证。
        !q.exec("CREATE INDEX IF NOT EXISTS pending_idx ON messages(recipient,delivered,time)"))
        return false;
    // 监听本机所有 IPv4 接口，允许本机或可达的局域网客户端连接。
    // 失败可能是端口占用等原因；不代表外部网络/防火墙已经允许访问。
    if (!listener.listen(QHostAddress::AnyIPv4, port)) {
        // 输出 TCP 监听失败的具体文本，便于定位启动问题。
        qCritical() << listener.errorString();
        return false;
    }
    // 打印实际端口和数据库文件路径，帮助确认启动配置。
    qInfo() << "LAN Chat server listening on" << port << "database:" << db.databaseName();
    return true;
}
// 接收新连接并建立会话。这个函数由 newConnection 信号触发，而不是每帧调用。
void ChatServer::accept() {
    // 循环取完当前等待队列中的连接；一次信号触发时可能已经积累多个连接。
    // 队列为空就结束，之后再有新连接会由下一次信号触发。
    while (listener.hasPendingConnections()) {
        // s 推导为 QTcpSocket*，指向本轮取出的已建立 TCP 连接。
        // 连接成功还不等于账号登录成功。
        auto s = listener.nextPendingConnection();
        // 同时登记的连接已达 128 条时拒绝新连接；数量包含尚未登录的连接。
        if (sessions.size() >= 128) {
            // 立即中止这个超额连接，不等待缓存发送完成。
            s->abort();
            // 安排事件循环稍后删除该 socket，避免留下不再使用的连接对象。
            s->deleteLater();
            // 结束本轮连接处理，继续检查等待队列中的下一条连接。
            continue;
        }
        // peer 是本连接的协议封装指针；this 作为父对象，使服务端销毁时能清理它。
        auto peer = new FramedSocket(s, this);
        // 把 socket 交给 peer 的 QObject 对象树管理；删除 peer 时同时清理其 socket。
        s->setParent(peer);
        // 先登记空账号，表示未认证会话；{} 在此构造空 QString，不是已登录用户。
        sessions.insert(peer, {});
        // 协议层成功提取完整 JSON 后交给服务端业务分发；底层半包不会直接进入 handle。
        connect(peer, &FramedSocket::received, this,
                // 回调捕获服务端指针和当前连接指针；o 是这一条解析完成的消息对象。
                // 把连接一起传给 handle，才能知道该消息来自哪个已认证身份。
                [this, peer](QJsonObject o) { handle(peer, o); });
        // 连接断开回调。按值捕获指针，不复制整个连接对象；this 是回调上下文。
        connect(s, &QTcpSocket::disconnected, this, [this, peer] {
            // 移除在线/未认证会话，后续 presence 就不会把该连接算作在线。
            sessions.remove(peer);
            // 删除此收件连接的内存待确认标记；数据库里未确认记录仍可在重登后投递。
            inFlight.remove(peer);
            // 移除该连接的认证中标记；这里不是强行停止已经开始的密码计算。
            authenticating.remove(peer);
            // 安排稍后删除封装及子 socket；不在当前信号调用栈中直接释放。
            peer->deleteLater();
            // 刷新并广播用户在线状态。注册、登录和断开等位置都会调用此方法。
            presence();
        });
    }
}

// 统一错误回复函数：p 是目标连接，m 是给客户端展示的错误原因。
// const 引用避免复制错误文本；本函数本身不关闭连接。
void ChatServer::fail(FramedSocket *p, const QString &m) {
    // 构造 error JSON 并交给协议层发送；这里未进一步处理 send 返回失败的情况。
    p->send({{"type", "error"}, {"message", m}});
}
// 处理一条业务请求。p 是来源连接，o 是完整 JSON，按 const 引用读取。
// 分为认证、ACK 和聊天三类；每个已处理分支用 return 避免继续执行其他业务。
void ChatServer::handle(FramedSocket *p, const QJsonObject &o) {
    // type 保存请求种类；字段缺失或不是可转换的字符串时将得到空字符串。
    QString type = o["type"].toString();
    // 注册和登录共用输入检查及密码派生流程，但注册写账号，登录核验已有账号。
    if (type == "register" || type == "login") {
        // 该连接已有账号身份时拒绝再次认证，避免在同一已登录连接上切换身份。
        if (!sessions.value(p).isEmpty()) {
            fail(p, QStringLiteral("已经登录"));
            return;
        }
        // 同一连接的上一轮密码计算尚未完成时，拒绝再次提交认证任务。
        if (authenticating.contains(p)) {
            fail(p, QStringLiteral("正在验证账号，请稍候"));
            return;
        }
        // name 是请求账号；password 是本次输入密码，暂时存在内存中用于派生计算。
        // 这两个值还不可信，下面先做格式校验，认证成功后才写入 sessions。
        QString name = o["name"].toString(), password = o["password"].toString();
        // 正则用完整字符串起止锚点限制账号：只允许 3 到 24 位 ASCII 字母、数字、下划线。
        // C++ 字符串中双反斜杠产生正则所需的单反斜杠；不能随意删掉转义。
        if (!QRegularExpression("\\A[A-Za-z0-9_]{3,24}\\z").match(name).hasMatch() ||
            // 密码长度要求 8 到 128 个 QString UTF-16 代码单元，并非所有字符都恰好一个单位。
            password.size() < 8 || password.size() > 128) {
            fail(p, QStringLiteral("账号须为3-24位字母、数字或下划线；密码须为8-128位"));
            return;
        }
        // q 是绑定到服务端 db 的临时查询对象；用于本作用域中的 SQL 执行及结果读取。
        // 不同函数/分支中的同名 q 是不同局部变量，不是共享的全局查询。
        QSqlQuery q(db);
        // salt 保存原始盐字节：注册时新生成，登录时从数据库十六进制文本恢复。
        QByteArray salt;
        // expected 保存数据库里已有的密码派生值，仅在登录比较时使用；注册时无需比较。
        QString expected;
        // 注册没有已有盐，所以生成一个新盐；下面 else 是登录时读取已有盐。
        if (type == "register")
            // 生成随机 UUID 并取得其 16 字节表示，用作该账号的盐。
            salt = QUuid::createUuid().toRfc4122();
        else {
            // 准备按账号查询的 SQL，? 是参数占位符，不把用户输入直接拼接进 SQL。
            q.prepare("SELECT salt,hash FROM users WHERE name = ?");
            // 把账号绑定到本 SQL 的第一个占位符；数据里的引号仍按普通参数处理。
            q.addBindValue(name);
            // exec 执行失败，或 next 无法移到第一条结果时，都不能继续读取查询字段。
            // 这里统一按业务分支报告失败；执行成功不代表查到一行记录。
            if (!q.exec() || !q.next()) {
                fail(p, QStringLiteral("账号或密码错误"));
                return;
            }
            // 第 0 列是数据库里的十六进制盐文本；fromHex 恢复为计算使用的原始字节。
            salt = QByteArray::fromHex(q.value(0).toByteArray());
            // 第 1 列是注册时保存的派生结果，之后与这次密码计算结果比较。
            expected = q.value(1).toString();
        }

        // 启动异步任务前先登记认证中，防止同一连接重复提交。
        authenticating.insert(p);
        // watcher 观察异步任务，结果类型为 QString；以连接 p 为父对象管理生命周期。
        // 它本身不是计算线程，真正的任务由 QtConcurrent 提交到线程池。
        auto watcher = new QFutureWatcher<QString>(p);
        // guarded 是能感知 QObject 删除的弱引用；连接对象销毁后自动变空。
        // 防止计算完成时继续访问已被客户端断开并删除的连接。
        QPointer<FramedSocket> guarded(p);
        // 先连接完成信号，再设置任务。this 作为接收上下文，使回调在服务端所属线程处理。
        connect(watcher, &QFutureWatcher<QString>::finished, this,
                // 按值保存认证后处理所需数据：类型、账号、盐、期望值及对象引用。
                // 这个回调处理结果、数据库和网络；它不是下面执行 PBKDF2 的工作任务。
                [this, guarded, watcher, type, name, salt, expected] {
                    // computed 是已完成任务返回的密码派生值；finished 到达后再获取结果。
                    QString computed = watcher->result();
                    // 完成后安排释放观察器；不会影响已经取出的 computed 字符串。
                    watcher->deleteLater();
                    // 连接对象已销毁或已不在会话表时放弃结果，不回复已失效连接。
                    if (!guarded || !sessions.contains(guarded.data()))
                        return;
                    // peer 是通过有效性检查后的目标连接指针，供本回调后续使用。
                    FramedSocket *peer = guarded.data();
                    // 移除该连接的认证中标记；这里不是强行停止已经开始的密码计算。
                    authenticating.remove(peer);
                    // 密码派生完成后的注册分支：持久化账号，再回 registered；不会自动登录。
                    if (type == "register") {
                        // insert 是此注册回调专用的数据库查询对象，仍在服务端线程操作 db。
                        QSqlQuery insert(db);
                        // 按当前 users 建表顺序依次写入 name、salt、hash，共三个占位符。
                        insert.prepare("INSERT INTO users VALUES(?,?,?)");
                        // 第一个参数为新注册账号，主键限制保证不能重复创建同名账号。
                        insert.addBindValue(name);
                        // 第二个参数为盐的十六进制文本；十六进制只含 ASCII，适合 fromLatin1 转换。
                        insert.addBindValue(QString::fromLatin1(salt.toHex()));
                        // 第三个参数保存密码派生值，数据库不写入原始 password。
                        insert.addBindValue(computed);
                        // 插入失败时返回错误，可能是主键冲突或数据库错误；不能报告注册成功。
                        if (!insert.exec()) {
                            fail(peer, QStringLiteral("注册失败，账号可能已经存在"));
                            return;
                        }
                        // 注册入库成功后发送注册确认，客户端仍需单独进行登录。
                        peer->send({{"type", "registered"}});
                        // 刷新并广播用户在线状态。注册、登录和断开等位置都会调用此方法。
                        presence();
                        return;
                    }
                    // 登录分支比较同一盐下的派生值，不相等则密码验证失败。
                    if (computed != expected) {
                        fail(peer, QStringLiteral("账号或密码错误"));
                        return;
                    }
                    // 再次检查账号是否已经在线：异步计算期间其他连接也可能已经登录。
                    if (sessions.values().contains(name)) {
                        fail(peer, QStringLiteral("该账号已在线，请先退出另一个客户端"));
                        return;
                    }
                    // 正式建立可信登录身份，之后发送人的账号从该映射读取。
                    sessions[peer] = name;
                    // 告诉客户端认证通过及实际账号；客户端随后打开历史库并进入聊天页。
                    peer->send({{"type", "logged_in"}, {"name", name}});
                    // 刷新并广播用户在线状态。注册、登录和断开等位置都会调用此方法。
                    presence();
                    // 尝试继续向该收件连接投递下一条待确认记录。
                    // 登录后开始补发，确认上一条后继续；没有待投递记录就直接结束。
                    pending(peer);
                });
        // 把线程池任务返回的 QFuture 交给 watcher 观察；本调用不是阻塞等待任务完成。
        watcher->setFuture(
            // 工作任务只捕获密码和盐的值副本，执行耗时派生并返回字符串。
            // 这里不操作 socket、sessions 或 db，避免跨线程访问这些业务对象。
            QtConcurrent::run([password, salt] { return passwordHash(password, salt); }));
        return;
    }
    // 非认证请求先取得可信发送身份；不能由请求中的 from 决定自己是谁。
    QString sender = sessions.value(p);
    // 未登录连接不能发送聊天或确认消息，先回复请登录再返回。
    if (sender.isEmpty()) {
        fail(p, QStringLiteral("请先登录"));
        return;
    }
    // ACK 表示收件客户端已保存消息，不是已阅读，也不是发送者的提交确认。
    if (type == "ack") {
        // 只接受此连接当前正在等待的消息 ID；空值、其他 ID、重复过期 ACK 均忽略。
        if (inFlight.value(p).isEmpty() || inFlight.value(p) != o["id"].toString())
            return;
        // q 是绑定到服务端 db 的临时查询对象；用于本作用域中的 SQL 执行及结果读取。
        // 不同函数/分支中的同名 q 是不同局部变量，不是共享的全局查询。
        QSqlQuery q(db);
        // 同时按消息 ID 和当前收件人账号更新，防止用户替其他收件人确认消息。
        q.prepare("UPDATE messages SET delivered =1 WHERE id =? AND recipient=?");
        // 绑定需要确认的消息 ID，对应 UPDATE 的第一个占位符。
        q.addBindValue(o["id"].toString());
        // 绑定当前可信账号：在 ACK 分支是收件人，在聊天 INSERT 中则是发送人。
        q.addBindValue(sender);
        // 确认更新执行成功后，才释放本连接的待确认位置并继续投递。
        // 若 SQL 执行失败则保留 inFlight，不把失败错误地视作成功。
        if (q.exec()) {
            // 清除本连接已完成的等待标记，让 pending 可以发送下一条。
            inFlight.remove(p);
            pending(p);
        }
        return;
    }
    // 已处理认证和 ACK 后，只允许 chat；空类型或其他未知类型返回错误。
    if (type != "chat") {
        fail(p, QStringLiteral("不支持的请求"));
        return;
    }
    // to 是收件账号，body 是原始正文；下面校验，不直接信任客户端输入。
    QString to = o["to"].toString(), body = o["body"].toString();
    // 拒绝给自己发送、纯空白正文和过长正文。trimmed 仅用于判断，实际保存仍是原正文。
    if (to == sender || body.trimmed().isEmpty() || body.size() > 4000) {
        fail(p, QStringLiteral("消息须为1-4000字，且不能发给自己"));
        return;
    }
    // q 是绑定到服务端 db 的临时查询对象；用于本作用域中的 SQL 执行及结果读取。
    // 不同函数/分支中的同名 q 是不同局部变量，不是共享的全局查询。
    QSqlQuery q(db);
    // 只需判断收件人是否存在，因此查询常量 1，而不读取用户完整信息。
    q.prepare("SELECT 1 FROM users WHERE name=?");
    // 绑定收件账号；具体占位符位置由当前已 prepare 的 SQL 及此前绑定顺序决定。
    q.addBindValue(to);
    // exec 执行失败，或 next 无法移到第一条结果时，都不能继续读取查询字段。
    // 这里统一按业务分支报告失败；执行成功不代表查到一行记录。
    if (!q.exec() || !q.next()) {
        fail(p, QStringLiteral("收件人不存在"));
        return;
    }
    // id 是服务端生成的消息唯一 UUID，去掉外层大括号；用于数据库主键与接收去重。
    QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    // time 是服务端 UTC 时间文本，包含毫秒，客户端显示时再转本地时区。
    QString time = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    // 先保存消息再投递；delivered 没有显式写入，使用建表默认值 0。
    q.prepare("INSERT INTO messages(id,sender,recipient,body,time) VALUES(?,?,?,?,?)");
    // 绑定新消息 ID，为 INSERT 的第一个字段。
    q.addBindValue(id);
    // 绑定当前可信账号：在 ACK 分支是收件人，在聊天 INSERT 中则是发送人。
    q.addBindValue(sender);
    // 绑定收件账号；具体占位符位置由当前已 prepare 的 SQL 及此前绑定顺序决定。
    q.addBindValue(to);
    // 绑定原始正文，保留用户输入的多行文本和空白，不把正文当 SQL 执行。
    q.addBindValue(body);
    // 绑定统一的服务端时间，避免相信客户端自行填写的消息时间。
    q.addBindValue(time);
    // 消息入库失败就报告失败并返回，不能发送“已保存”确认。
    if (!q.exec()) {
        fail(p, QStringLiteral("消息保存失败"));
        return;
    }
    // 消息入库成功后，把带 ID、双方账号、正文和时间的 message 回给发送方。
    // 这是服务器保存确认，不代表收件方已经在线、保存或阅读。
    p->send({{"type", "message"},
             {"id", id},
             {"from", sender},
             {"to", to},
             {"body", body},
             {"time", time}});
    // 只读遍历所有会话；i 是哈希迭代器，i.key 为连接指针，i.value 为账号。
    // handle 中用于找在线收件人；presence 中用于逐个发送用户列表。
    // cbegin/cend 表示只读遍历，不保证账号遍历顺序。
    for (auto i = sessions.cbegin(); i != sessions.cend(); ++i)
        // 找到属于目标收件账号的在线连接才投递；找不到时数据库记录保留等待登录。
        if (i.value() == to)
            // 向找到的收件连接尝试投递；若该连接正等待上一条 ACK，此次不会重复塞入下一条。
            pending(i.key());
}
// 重新生成账号目录并广播在线状态；不是发送聊天正文的函数。
void ChatServer::presence() {
    // users 是本次广播的 JSON 数组，每个元素保存一个账号及其 online 布尔值。
    QJsonArray users;
    // 构造时直接执行查询，按账号排序读取所有注册用户，不仅仅查询在线用户。
    QSqlQuery q("SELECT name FROM users ORDER BY name", db);
    // 逐行推进查询游标；每次成功对应一个账号，结果耗尽或读取失败时退出。
    while (q.next()) {
        // name 是当前行第 0 列的账号；本局部变量只在这次循环体中使用。
        QString name = q.value(0).toString();
        // 把账号加入目录；是否在线由当前会话表决定，不用数据库永久存一个在线标记。
        users.append(QJsonObject{{"name", name}, {"online", sessions.values().contains(name)}});
    }
    // 只读遍历所有会话；i 是哈希迭代器，i.key 为连接指针，i.value 为账号。
    // handle 中用于找在线收件人；presence 中用于逐个发送用户列表。
    // cbegin/cend 表示只读遍历，不保证账号遍历顺序。
    for (auto i = sessions.cbegin(); i != sessions.cend(); ++i)
        // 只有已认证会话才接收账号目录；空字符串代表未登录连接。
        if (!i.value().isEmpty())
            // 给当前迭代连接发送同一份用户列表，由客户端刷新联系人与在线状态。
            i.key()->send({{"type", "users"}, {"users", users}});
}
// 向收件连接 p 尝试发送一条未确认消息；不一次循环发送整个离线队列。
void ChatServer::pending(FramedSocket *p) {
    // 已有一条等待 ACK 就返回，一次一条避免离线消息大量写入 socket 缓存。
    if (inFlight.contains(p))
        return;

    // q 是绑定到服务端 db 的临时查询对象；用于本作用域中的 SQL 执行及结果读取。
    // 不同函数/分支中的同名 q 是不同局部变量，不是共享的全局查询。
    QSqlQuery q(db);
    q.prepare(
        // 筛选此用户未确认记录，按 SQLite rowid 插入顺序取最早一条。
        // LIMIT 1 配合 ACK 后再次调用 pending，逐条推进队列。
        "SELECT id,sender,recipient,body,time FROM messages WHERE recipient=? AND delivered=0 ORDER BY rowid LIMIT 1");
    // 把该连接的登录账号绑定为收件人，不允许请求随意指定读取别人的离线消息。
    q.addBindValue(sessions.value(p));
    // 只有查询执行成功且确实取得一行，才可以读取字段和投递；无记录则正常结束。
    if (q.exec() && q.next()) {
        // 将这一行转为 message 帧；下面 value(0..4) 分别对应 SELECT 中五个字段。
        // send 为 true 只表示本地接受了帧，仍需等待客户端保存后发送 ACK。
        if (p->send({{"type", "message"},
                     {"id", q.value(0).toString()},
                     {"from", q.value(1).toString()},
                     {"to", q.value(2).toString()},
                     {"body", q.value(3).toString()},
                     {"time", q.value(4).toString()}}))
            // 仅在 send 返回 true 时，记录本连接正在等待的消息 ID。
            // 此处不把 delivered 改为 1；断线后仍能从数据库补发相同 ID。
            inFlight[p] = q.value(0).toString();
    }
}
