#include <logger.hpp>
#include <mrpc/server.hpp>
using namespace mrpc;
//可选，设置异常回调
int main() {
    wlog::logger::get().init("logs/_other.log");
    auto& server = server::get();
    server.set_ip_port("127.0.0.1", 3334);
    server.set_server_name("other_server");
    server.run();
    
    server.reg_func("other_echo",[](const std::string& str){
        // LOG_DEBUG("other_echo: {}", str);
        return str;
    });
    server.accept();//启动监听上下文

    server.wait_shutdown();
    wlog::logger::get().shutdown();
    return 0;
}