#include "server.h"
#include "client.h"
#include <QApplication>
#include <QCoreApplication>
#include <QStandardPaths>
#include <QDir>
#include <QDebug>
#include <QTimer>

// 程序入口。argc 是命令行参数数量；argv 是参数字符串数组，argv[0] 通常是程序路径。
// 本程序一个可执行文件兼任服务端和客户端；返回 0 通常表示正常结束，1 表示失败。
int main(int argc, char **argv) {
    // serverMode 记录是否请求服务端模式；testMode 记录是否请求协议自检。
    // 两者先设为 false，再扫描参数更新；普通启动时都为 false，进入 GUI 模式。
    bool serverMode = false, testMode = false;
    // 逐个扫描命令行参数：从 1 开始跳过程序路径，i 是当前参数下标。
    // 此循环只识别模式开关，具体端口和数据库目录在后面解析。
    for (int i = 1; i < argc; ++i) {
        // 把当前本地编码参数转成 QString 并比较。对 bool 使用 |= 表示保留此前的 true；
        // 任意位置出现 --server 都会将 serverMode 设为 true。
        serverMode |= QString::fromLocal8Bit(argv[i]) == "--server";
        // 任意位置出现 --self-test 即请求协议自检；与后面的界面自检开关不同。
        testMode |= QString::fromLocal8Bit(argv[i]) == "--self-test";
    }
    // 无界面的两种模式进入此分支；两开关同时出现时，下方优先执行协议自检。
    if (serverMode || testMode) {
        // 建立非 GUI Qt 应用环境，支持参数、事件循环、网络和定时器。
        // app 是栈对象，其生命周期覆盖这个分支；这里不需要创建窗口。
        QCoreApplication app(argc, argv);
        // 设置组织名称，参与 QSettings 与标准应用数据目录的定位。
        app.setOrganizationName("LanChat");
        // 设置应用名称，使数据库路径和设置项具有稳定的应用标识。
        app.setApplicationName("LanChat");
        // 协议自检只做同步检查，不监听 TCP，也不进入长期事件循环。
        if (testMode) {
            // ok 保存编码长度与 JSON 往返检查的结果；true 代表这项自检通过。
            bool ok = protocolSelfTest();
            // 通过日志打印自检结论，三目运算符按 ok 选择成功或失败文本。
            qInfo() << (ok ? "Protocol self-test passed" : "Protocol self-test failed");
            // 把检查结果转换为进程退出码，便于测试脚本判断；这里立即结束程序。
            return ok ? 0 : 1;
        }
        // args 是 Qt 提供的完整命令行参数列表，用于按开关位置读取后续值。
        QStringList args = app.arguments();
        // 服务端默认监听端口。quint16 是无符号 16 位整数，范围为 0 到 65535；
        // 本程序要求用户显式传入的端口在 1 到 65535 之间。
        quint16 port = 45454;
        // index 是 --port 的首次出现位置；找不到返回 -1，避免误读其他参数。
        int index = args.indexOf("--port");
        // 只有找到对应开关才读取其下一个参数；本文件分别用于 --port 和 --data。
        if (index >= 0) {
            // toInt 的输出标记：稍后的转换调用会写入成功或失败状态，不是端口数值。
            bool ok;
            // value 暂用 int 保存端口输入，先校验再缩窄到 quint16。
            // value(index+1) 在参数缺失时返回空字符串，toInt 将转换失败写入 ok。
            int value = args.value(index + 1).toInt(&ok);
            // 拒绝非数字、缺少参数、0、负数或超出上限的端口，避免截断后监听错误端口。
            if (!ok || value < 1 || value > 65535) {
                // 输出端口错误日志；下面返回非零值，不继续初始化服务端。
                qCritical() << "Invalid--port";
                return 1;
            }
            // 通过范围检查后才进行类型转换，保留合法端口值。
            port = quint16(value);
        }
        // data 是服务端数据库目录。先取得此应用的标准可写数据路径，
        // 下一行 filePath 再拼接 server 子目录；此处只计算路径，不创建目录。
        QString data = QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
                           .filePath("server");
        // 复用 index 查找数据库目录开关；没有 --data 就保留默认目录。
        index = args.indexOf("--data");
        // 只有找到对应开关才读取其下一个参数；本文件分别用于 --port 和 --data。
        if (index >= 0) {
            // 使用 --data 后面的文本覆盖目录；允许相对路径，由启动时工作目录解释。
            data = args.value(index + 1);
            // 开关后缺少目录或目录为空时失败返回，防止把空值当作有效配置。
            if (data.isEmpty())
                return 1;
        }
        // server 是本分支内的服务端实例，持有监听器、会话表和数据库句柄。
        // 它必须在 app.exec 运行期间存活，不能创建后立刻离开作用域。
        ChatServer server;
        // 创建目录、打开数据库、建表并监听；任一步失败就结束进程。
        if (!server.start(port, data))
            return 1;
        // 进入 Qt 事件循环，持续分发网络、定时器和其他事件。
        // exec 不是一个空转循环；退出事件循环后把退出码交给操作系统。
        return app.exec();
    }
    // GUI 模式需要 QApplication 而不是只有 QCoreApplication。
    // 它在创建窗口之前初始化控件、样式等图形应用环境。
    QApplication app(argc, argv);
    // 设置组织名称，参与 QSettings 与标准应用数据目录的定位。
    app.setOrganizationName("LanChat");
    // 设置应用名称，使数据库路径和设置项具有稳定的应用标识。
    app.setApplicationName("LanChat");
    // 界面自检分支：内部建立测试窗口和内存数据库，结束后直接返回测试结果。
    if (app.arguments().contains("--ui-self-test"))
        // 调用静态界面自检函数；它与上方的 protocolSelfTest 覆盖范围不同。
        return ChatWindow::selfTest() ? 0 : 1;
    // window 是客户端主窗口栈对象；构造时创建登录页、聊天页和网络回调。
    ChatWindow window;
    // 请求显示窗口；随后进入事件循环后，窗口才能持续绘制和接收用户操作。
    window.show();
    // screenshot 保存截图开关的参数位置，找不到为 -1。
    // 使用 --screenshot 后跟输出路径，可以保存窗口图像后退出。
    // 此参数仅用于截图检查，不影响正常聊天模式。
    int screenshot = app.arguments().indexOf("--screenshot");
    // 仅在找到截图开关时，安排延迟截图；普通启动跳过本段。
    if (screenshot >= 0) {
        // path 是截图输出文件名或路径；save 会根据扩展名选择支持的图片格式。
        QString path = app.arguments().value(screenshot + 1);
        // 500 毫秒后执行一次回调，让窗口先有时间布局和绘制；不是阻塞等待。
        // &app 是回调上下文，应用对象销毁后不会再执行该上下文的回调。
        QTimer::singleShot(500, &app,
                           // lambda 按引用使用仍然存活的 window/app，按值保存路径 path。
                           // grab 获取窗口图像，save 返回保存结果；app.exit 设置退出码并结束事件循环。
                           [&window, &app, path] { app.exit(window.grab().save(path) ? 0 : 1); });
    }
    // 进入 Qt 事件循环，持续分发网络、定时器和其他事件。
    // exec 不是一个空转循环；退出事件循环后把退出码交给操作系统。
    return app.exec();
}
