#include <logger.hpp>
#include <mrpc/client.hpp>

#include <iostream>
#include <thread>
#include <chrono>
#include <bitset>
using namespace std::chrono_literals;
using namespace mrpc;

int main() {
    wlog::logger::get().init("logs/" PROJECT_NAME ".log");

    auto& client = client::get();
    client.run();

    client.router().reg_handle("query_funcname", [](connection::cptr conn, uint64_t msg_id) {
        auto funcname = conn->router().query_msg_name(msg_id);
        LOG_DEBUG("remote query message name: {}({})", funcname, msg_id);
        return std::make_tuple(msg_id, funcname);
    });

    client.router().set_exception_callback([](connection::cptr conn, int error,
         msg_id_t id, const std::string& buffer){
             if (error == mrpc::not_implemented) {
                conn->async_call([](uint32_t err_code, const std::string& err_msg, const nlohmann::json& ret){
                    LOG_DEBUG("query_funcname response: {}", ret.dump());
                }, "query_funcname", id.msg_id);
             }
    });

    auto ret = client.call<uint32_t>("test_not_impl", 11, 12);
    if (ret.error_code() == mrpc::ok) {
        std::cout << "return: " << ret.value() << std::endl;
    } else {
        std::cout << "return error: " << ret.error_msg() << std::endl;
    }//最终会因为服务端没有实现这个函数，在双向交互后因为超时导致调用失败

    client.wait_shutdown();
    wlog::logger::get().shutdown();
    return 0;
}