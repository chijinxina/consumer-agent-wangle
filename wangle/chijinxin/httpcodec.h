//
// Created by chijinxin on 18-6-15.
//

#ifndef WANGLE_HTTPCODEC_H
#define WANGLE_HTTPCODEC_H

#include <folly/io/IOBufQueue.h>
#include <folly/io/IOBuf.h>
#include <wangle/channel/Handler.h>
#include <boost/beast/http.hpp>   //http parser：利用boost::beast::http组件解析http报文
#include <boost/optional.hpp>

using namespace std;
using namespace folly;
using namespace wangle;
/*
 * boost beast http parse
 * 在wangle框架下利用boost::beast::http实现http报文解析
 */
using Request = boost::beast::http::message<true, boost::beast::http::string_body>;
using Response = std::string;  //boost::beast::http::message<false, boost::beast::http::string_body>;
using RequestParser = boost::beast::http::request_parser<boost::beast::http::string_body>;
using ResponseSerializer = boost::beast::http::response_serializer<boost::beast::http::string_body>;


/*
 * socket缓冲区数据透传
 * edit by chijinxin
 * 2018/06/06
 */
class GreedyInputDecoder : public InboundHandler<IOBufQueue&, std::unique_ptr<IOBuf>> {
public:
    void read(Context* ctx, IOBufQueue& q) override
    {
        ctx->fireRead(q.move());
    }

};

/*
 * Http编码 HTTPCodec
 * edit by chijinxin
 * 2018/06/06
 */
class HttpCodec : public wangle::Handler<std::string, Request, Response, std::string> {
public:
    //构造函数簇
    HttpCodec() = default;
    HttpCodec(const HttpCodec& ) = default;
    HttpCodec& operator= (const HttpCodec & ) = default;
    HttpCodec& operator= (HttpCodec&& other) = default;
    //析构函数
    ~HttpCodec()
    {
        if (parser_ptr)
        {
            delete parser_ptr;  //析构 释放http request parser
        }
    };

    typedef typename Handler<std::string, Request, Response, std::string>::Context Context;

    //1. 上游：读取下游handel发来的string消息，将其解析成HTTP::Request发往上游
    void read(Context* ctx, std::string line) override
    {
        //cout << "received line :" <<endl << line <<endl;

        RequestParser& parser = parser_ref();

        unparsed_buffer_ += line;

        auto buf = boost::asio::buffer(unparsed_buffer_);

        boost::beast::error_code ec;

        auto bytes_read = parser.put(buf, ec);

        if (ec &&  boost::beast::http::error::need_more != ec)
        {
            cerr << "http parser failed with message " << ec.message() <<endl;
            finish_parse();
            ctx->fireClose();
        }

        if (unparsed_buffer_.length() == bytes_read)
        {
            unparsed_buffer_.clear();
        }
        else if (bytes_read > 0)
        {
            unparsed_buffer_ = unparsed_buffer_.substr(bytes_read, unparsed_buffer_.length()-bytes_read);
        }
        if (parser.is_done()) //http::request报文解析完成
        {
            auto parsed = finish_parse();
            ctx->fireRead(std::move(parsed));
        }
    }

    //2. 下游：接收上游的HTTP::Response消息，将其序列化成字符串发往下游
    folly::Future<folly::Unit> write(Context* ctx, Response resp) override
    {
        //write信息透传
        return ctx->fireWrite(std::move(resp));
    }

    //重载其它事件
    void readException(Context* ctx, folly::exception_wrapper e) override
    {
        cout<<"[httpcodec]: readException!!!"<<endl;
        Response resp("HTTP/1.1 200 OK\ncontent-length:3\ncontent-type:application/x-www-form-urlencoded\n\n123\n");
        write(ctx,std::move(resp));
    }

private:
    //完成HTTP请求报文的解析
    Request finish_parse()
    {
        auto parsed = parser_ref().release();
        delete parser_ptr;
        parser_ptr = nullptr;
        unparsed_buffer_.clear();
        return parsed;
    }
    //获取http报文解析器的引用
    RequestParser& parser_ref()
    {
        if (!parser_ptr)
        {
            parser_ptr = new RequestParser{};
            parser_ptr->eager(true);
        }
        return *parser_ptr;
    }
    std::string unparsed_buffer_;   //未解析的报文缓冲字符串
    RequestParser *parser_ptr;      //boost::beast::http 请求报文解析器

};





#endif //WANGLE_HTTPCODEC_H
