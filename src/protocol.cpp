#include "protocol.h"
#include <QJsonDocument>
#include <QCryptographicHash>
#include <QMessageAuthenticationCode>
#include <QtEndian>

// 包体上限 65536 字节，不含 4 字节头。constexpr 是编译期常量，static 限定名字在当前编译单元使用。
static constexpr quint32 MaxFrame = 64 * 1024;
// 把 object 编成“4 字节大端长度 + UTF-8 JSON”。const 引用只读传参；返回完整帧，不在此发起网络发送。
QByteArray encodeFrame(const QJsonObject &object) {
    // data 是紧凑 JSON 字节。Compact 不加美化缩进；长度应统计 UTF-8 字节而不是中文字符数量。
    QByteArray data = QJsonDocument(object).toJson(QJsonDocument::Compact);
    // result 先分配 4 个零字节作为帧头；这不是空 QByteArray，后面会覆盖这 4 字节。
    QByteArray result(4, '\0');
    // 把包体字节数作为 32 位无符号数按大端写入头部。reinterpret_cast 将可写 char 内存视作无符号字节，不复制数据。
    qToBigEndian<quint32>(quint32(data.size()), reinterpret_cast<uchar *>(result.data()));
    // 拼接头与包体。头内长度不包含头自身，发送方与接收方必须遵守同一约定。
    return result + data;
}
// s 是外部创建的 socket，初始化列表将其保存为成员 socket；parent 是本封装的父对象。构造时不建立连接，也不改变 s 的父对象。
FramedSocket::FramedSocket(QTcpSocket *s, QObject *parent) : QObject(parent), socket(s) {
    // 限制 Qt socket 内部接收缓冲大小；不能代替后面的帧长检查，也不是整个进程内存上限。
    socket->setReadBufferSize(MaxFrame * 2);
    // 有新字节可读时调用 read。一次信号可能只有半包，也可能已经粘着多帧。
    connect(socket, &QTcpSocket::readyRead, this, &FramedSocket::read);
    // 断开后清空残留半帧，避免重连时混入旧连接数据。this 是回调上下文，并允许 lambda 访问 buffer。
    connect(socket, &QTcpSocket::disconnected, this, [this] { buffer.clear(); });
}
// 发送业务 JSON。true 仅代表完整帧被本地 write 接受，不代表远端已保存或阅读。
bool FramedSocket::send(const QJsonObject &object) {
    // frame 是本次编码出的完整帧；const 防止意外改变它，frame.size 包含 4 字节头。
    const QByteArray frame = encodeFrame(object);
    // 未连接或整帧超过上限时拒绝发送；逻辑或任一条件成立即可进入失败分支。
    if (socket->state() != QAbstractSocket::ConnectedState || frame.size() > int(MaxFrame + 4) ||
        // 待写缓冲已超过 4 MiB 就不继续排队，降低慢连接积压。这里检查写入前的积压，并非写入后的严格总量限制。
        socket->bytesToWrite() > 4 * 1024 * 1024)
        return false;
    // write 返回本地接受的字节数，必须等于整帧大小才报告成功。对方保存状态由上层 ACK 判断。
    return socket->write(frame) == frame.size();
}
// 增量收包函数：保存半包并循环提取完整帧，专门解决 TCP 拆包、粘包；不在此处理登录和聊天业务。
void FramedSocket::read() {
    // 读取当前已到达的全部可读字节，追加到成员 buffer。不会等待未来字节；旧半包不能丢弃。
    buffer += socket->readAll();
    // 循环条件保证头部完整。每轮提取一帧后继续检查剩余数据；不足 4 字节时退出并保留数据到下次事件。
    while (buffer.size() >= 4) {
        // size 是头中记录的包体字节数。constData 提供只读地址，按大端读取前 4 字节以统一不同 CPU 的字节序。
        quint32 size = qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(buffer.constData()));
        // 零长度或超长帧直接拒绝，不用不可信长度继续等待包体。
        if (!size || size > MaxFrame) {
            // 发出协议错误信号供已连接的观察者处理；emit 是信号发送，不是抛出 C++ 异常。
            emit invalidFrame(QStringLiteral("非法帧长度"));
            // 立即中止异常连接，不尝试在错误字节流中搜索下一条消息。断开回调也会清空缓冲。
            socket->abort();
            // 丢弃当前无效字节，防止继续解析已判定错误的帧。
            buffer.clear();
            return;
        }
        // 合法帧头已到但包体未齐，返回等待下一次 readyRead；不清空 buffer。size 已被限制，此处加 4 转 int 不会因巨大长度溢出。
        if (buffer.size() < int(size + 4))
            return;
        // error 接收 JSON 解析错误状态和位置，与 socket 网络错误不是同一类错误。
        QJsonParseError error;
        // doc 推导为 QJsonDocument。跳过头部，仅截取本帧包体解析，不混入后续帧的字节。
        auto doc = QJsonDocument::fromJson(buffer.mid(4, int(size)), &error);
        // 移除本帧头与包体，保留后续整帧或半帧；若不移除，循环就会重复处理同一条消息。
        buffer.remove(0, int(size + 4));
        // 要求 JSON 语法正确且根值为对象，数组或数字即使合法也不接受。
        if (error.error != QJsonParseError::NoError || !doc.isObject()) {
            // 通知解析或根类型错误，然后中止连接并返回；业务 handle 不会收到非法对象。
            emit invalidFrame(QStringLiteral("非法JSON"));
            // 立即中止异常连接，不尝试在错误字节流中搜索下一条消息。断开回调也会清空缓冲。
            socket->abort();
            return;
        }
        // 发出一条完整业务对象，交给客户端或服务端处理。随后继续 while，所以一次读事件可能产生多次 received。
        emit received(doc.object());
    }
}
// PBKDF2-HMAC-SHA256 派生函数，60000 轮、32 字节输出。password 是密码，salt 是原始盐；只计算不访问数据库或网络，可放在线程池。
QString passwordHash(const QString &password, const QByteArray &salt) {
    // key 是密码 UTF-8 字节，作为 HMAC 密钥反复使用；不是返回值，也不直接存入账号数据库。
    QByteArray key = password.toUtf8();
    // u 首先是 U1=HMAC-SHA256(key,salt+INT32_BE(1))。00000001 是第一个派生块的四字节编号，不是文本字符 1。SHA256 一块 32 字节，当前只需一块输出。
    QByteArray u = QMessageAuthenticationCode::hash(salt + QByteArray::fromHex("00000001"), key,
                                                    QCryptographicHash::Sha256);
    // result 是累计异或结果，先保存 U1，再逐轮与后续 U 值异或；不是网络帧编码函数里的同名局部变量。
    QByteArray result = u;
    // i 从 1 到 59999，共 59999 次循环，加上循环外 U1，一共执行 60000 次 HMAC。外层循环负责派生轮次。
    for (int i = 1; i < 60000; ++i) {
        // 以密码 key 为 HMAC 密钥、上一轮 u 为输入，算出下一轮 u，形成依赖前一轮的计算链。
        u = QMessageAuthenticationCode::hash(u, key, QCryptographicHash::Sha256);
        // j 是字节下标，内层循环遍历 32 字节，将本轮 u 合并到对应结果位置；与外层轮次 i 区分。
        for (int j = 0; j < result.size(); ++j)
            // ^ 为逐位异或，不是乘方；先按无符号字节计算，再转回 QByteArray 使用的 char，累积 U1 到 U60000。
            result[j] = char(uchar(result[j]) ^ uchar(u[j]));
    }
    // 32 字节派生结果转为 64 个十六进制 ASCII 字符，便于存 SQLite TEXT。不是可逆密码加密，也不提供 TCP 传输加密。
    return QString::fromLatin1(result.toHex());
}
// 轻量同步自检，仅验证帧长度与 JSON 往返。它不启动 TCP 服务，也不检查密码派生或真实拆包粘包。
bool protocolSelfTest() {
    // object 是测试 JSON，含中文与字面量反斜杠 n；注意源码双反斜杠表示文本中的反斜杠，不是真实换行字符。
    const QJsonObject object{{"type", "message"}, {"body", QStringLiteral("你好\nLAN")}};
    // encoded 是自检生成的完整帧；前四字节是头，其余为原对象的 UTF-8 JSON。
    QByteArray encoded = encodeFrame(object);
    // 读取头部，检查是否等于实际包体长度；下方 && 要求长度检查与内容检查同时通过。
    return qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(encoded.constData())) ==
               // 整帧减四字节头才是实际包体长度；若前项失败，逻辑与短路，不再执行后项。
               quint32(encoded.size() - 4) &&
           // 跳过头部解析 JSON，并与原对象比较字段和值；两项都成立则返回 true。
           QJsonDocument::fromJson(encoded.mid(4)).object() == object;
}
