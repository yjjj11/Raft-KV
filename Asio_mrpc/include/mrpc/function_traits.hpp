// mrpc_traits.hpp
#ifndef MRPC_TRAITS_HPP
#define MRPC_TRAITS_HPP

#pragma once

#include <functional>
#include <tuple>
#include <type_traits>
namespace mrpc {
// 前置声明：只声明需要的类型，不包含头文件，避免循环
class connection;

//定义一个「类型判断的标签」，不依赖 shared_ptr
template<typename T>
struct is_connection_cptr : std::false_type {};

template<typename T>
using remove_const_reference_t = std::remove_const_t<std::remove_reference_t<T>>;

template<typename>
struct function_traits;

template<typename Function>
struct function_traits : public function_traits<decltype(&Function::operator())> {
};


//能匹配是Function的模板，因为Function也就是由ReturnType(*)(Arg, Args...)组成的普通函数指针
template<typename ReturnType, typename... Args>
struct function_traits<ReturnType(*)(Args...)> {
    // 总参数个数：所有传入的参数数量
    static constexpr std::size_t total_argc = sizeof...(Args);

    using function_type = ReturnType(Args...);
    using stl_function_type = std::function<function_type>;
    using return_type = ReturnType;
    using pointer = ReturnType(*)(Args...); // 原始函数指针类型
    
    // 所有参数的元组（含可能的conn）
    using all_args_tuple_raw = std::tuple<Args...>;

    using first_arg_type = std::conditional_t< total_argc >= 1, std::tuple_element_t<0, all_args_tuple_raw>, void>;

    static constexpr bool first_arg_is_conn = (total_argc >= 1) && is_connection_cptr<first_arg_type>::value;
    static constexpr bool has_conn_first = first_arg_is_conn;
    // 业务参数的个数
    static constexpr std::size_t argc = first_arg_is_conn ? (total_argc - 1) : total_argc;
    //业务参数元组
    template <std::size_t N, typename Tuple>
    struct tuple_rest;
    template <typename T, typename... Ts>
    struct tuple_rest<1, std::tuple<T, Ts...>> { using type = std::tuple<Ts...>; };
    template <typename... Ts>
    struct tuple_rest<0, std::tuple<Ts...>> { using type = std::tuple<Ts...>; };
    template <std::size_t N, typename Tuple>
    using tuple_rest_t = typename tuple_rest<N, Tuple>::type;

    // 业务参数元组：有conn则去掉第一个，无conn则用原元组
    using all_args_tuple = std::tuple<remove_const_reference_t<Args>...>;
    using args_tuple = tuple_rest_t<first_arg_is_conn ? 1 : 0, all_args_tuple>;
};

template <typename ReturnType, typename... Args>
struct function_traits<std::function<ReturnType(Args...)>> : function_traits<ReturnType(*)(Args...)> {};

template <typename ReturnType, typename ClassType, typename... Args>
struct function_traits<ReturnType(ClassType::*)(Args...)> : function_traits<ReturnType(*)(Args...)> {};

template <typename ReturnType, typename ClassType, typename... Args>
struct function_traits<ReturnType(ClassType::*)(Args...) const> : function_traits<ReturnType(*)(Args...)> {};

} // namespace mrpc

#endif // MRPC_TRAITS_HPP