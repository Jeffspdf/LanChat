// 本文件声明“带消息边界的 TCP 通信封装”，由客户端和服务端共同使用。
// TCP 本身只提供字节流；本项目用“4 字节大端长度 + UTF-8 JSON”表示一条消息。
// 头文件负责告诉其他源文件有哪些类型、成员和函数，具体执行步骤见 protocol.cpp。

// 让本头文件在同一编译单元中只展开一次，避免类型重复定义。
#pragma once
// TCP 套接字类型；同时通过 Qt 头文件获得本类依赖的基础类型声明。
#include <QTcpSocket>
// JSON 对象类型：保存 type、账号、正文等带名称的字段。
#include <QJsonObject>
// 标准库函数包装相关头文件；当前头文件没有直接用到，保持原代码不删改。
#include <functional>

// 继承 QObject，才能使用 Qt 信号、连接上下文和父子对象管理。
// 一个 FramedSocket 对应一个 TCP 连接，并拥有独立的收包缓冲区。
class FramedSocket : public QObject {
    // 启用 Qt 元对象系统；moc 会为下面的信号生成必要代码。
    Q_OBJECT
  public:
    // 构造封装对象。socket 参数是需要包装的已创建套接字，不会在这里新建连接。
    // parent 是本封装对象的 Qt 父对象，默认 nullptr 表示不指定父对象。
    // explicit 禁止把 QTcpSocket* 隐式转换为 FramedSocket。
    // 构造函数连接 readyRead 与 disconnected；不会自动改变 socket 的父对象。
    explicit FramedSocket(QTcpSocket *socket, QObject *parent = nullptr);
    // 把 object 编码成完整帧并交给 socket 写出；const 引用避免不必要的复制。
    // 返回 true 表示整帧已被本地写入接口接受，不表示对方已保存或已读。
    // 连接未建立、帧太大或待写缓存过多时返回 false。
    bool send(const QJsonObject &object);
    // 实际收发字节的套接字指针；与构造参数同名，成员用于持续访问连接。
    // 服务端在 accept 中将套接字设为本封装的子对象；客户端把它设为窗口子对象。
    // 因此不要笼统认为 FramedSocket 在任何用法中都拥有 socket。
    QTcpSocket *socket;
  // 以下是信号声明：使用 emit 发出，由 connect 连接的接收方处理；不手写信号函数体。
  signals:
    // 成功解析一条完整 JSON 对象后发出。object 按值传递，承载这一条业务消息。
    // 一次 readyRead 可能解析并发出多条 received，也可能因半包暂时一条都不发。
    void received(QJsonObject object);
    // 发现非法长度或非法 JSON 时发出；reason 是可用于日志或提示的错误原因。
    // 当前 read 实现还会中止连接；仅声明此信号不意味着界面已连接它显示错误。
    void invalidFrame(QString reason);

  private:
    // 当前连接尚未消费的原始字节，跨多次 readyRead 保留半包。
    // 解析完一帧只移除该帧，后面的字节继续等待解析；断开连接时清空。
    QByteArray buffer;
    // readyRead 触发后的内部处理函数，不是从磁盘读文件。
    // 把新字节追加进 buffer，再循环检查帧头、帧长、包体与 JSON。
    // 数据不足就保留缓冲并返回；每提取一条完整消息发出 received。
    void read();
};
// 独立编码函数，不属于 FramedSocket 类。输入 JSON 对象，返回带长度头的二进制帧。
// 长度统计 UTF-8 JSON 的字节数，不是 QString 的字符数，也不含头部自身的 4 字节。
QByteArray encodeFrame(const QJsonObject &object);
// 轻量协议自检：检查编码后的长度和 JSON 往返是否一致。
// 返回 true 为通过；它不启动服务器，也不能代替真实 TCP 拆包、粘包测试。
bool protocolSelfTest();
// 密码派生函数：password 为输入密码，salt 为每个账号的盐字节。
// 当前实现为 PBKDF2-HMAC-SHA256，60000 轮，返回 32 字节结果的十六进制文本。
// 注册时存盐和派生值，登录时用相同盐重算比较；这不是可逆加密，也不提供 TCP 加密。
// 此计算有开销，服务端通过线程池调用它，避免占用网络事件线程。
QString passwordHash(const QString &password, const QByteArray &salt);
