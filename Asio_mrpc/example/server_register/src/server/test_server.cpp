#include <logger.hpp>
#include <mrpc/server.hpp>
using namespace mrpc;
//可选，设置异常回调

void exception_callback(connection::cptr conn, int error,
         msg_id_t id, const std::string& buffer){
             if (error == mrpc::not_implemented) {
                conn->async_call([](uint32_t err_code, const std::string& err_msg, const nlohmann::json& ret){
                    LOG_DEBUG("query_funcname response: {}", ret.dump());
                }, "query_funcname", id.msg_id);
    } 
}
int test_mul(int i, int j) {
    // LOG_DEBUG("\nrecv test mul: {} * {}", i, j);
    std::this_thread::sleep_for(1s);
    return i * j;
}
int test_add(int i, int j) {
    // std::this_thread::sleep_for(5ms);
    // LOG_DEBUG("\nrecv test add: {} + {}", i, j);
    return i + j;
}
auto query_name(connection::cptr conn, uint64_t msg_id) {//可以直接调用对端函数
        auto funcname = conn->router().query_msg_name(msg_id);
        LOG_DEBUG("\nremote query message name: {}-{}", funcname, msg_id);
        return std::make_tuple(msg_id, funcname);
}

class Echo{
    public:
        int echo(int i) {
            // LOG_DEBUG("\nrecv echo: {}", i);
            return i;
        }
};
int main() {
    spdlog::set_level(spdlog::level::off);
    wlog::logger::get().init("logs/_test.log");
    auto& server = server::get();
    server.set_ip_port("127.0.0.1", 3333);
    server.set_server_name("test_server");
    server.run();
    //非成员函数无指定工作线程
    server.reg_func("test_add", test_add);

    server.reg_func("other_echo",[](const std::string& str){
        // LOG_DEBUG("other_echo: {}", str);
        // std::this_thread::sleep_for(5ms);
        return str;
    });
    //双向调试函数
    server.reg_func("query_funcname", query_name);
    // 非类函数投递到工作线程执行，主要用于执行io或者长时间操作的函数，避免阻塞io线程
    server.reg_func("test_mul", test_mul );
    //注册类函数
    server.reg_func("echo", &Echo::echo, std::make_shared<Echo>());

    //设置异常处理（找不到的情况）
    server.router().set_exception_callback(exception_callback);
    
    server.accept();//启动监听上下文

    server.wait_shutdown();
    wlog::logger::get().shutdown();
    return 0;
}