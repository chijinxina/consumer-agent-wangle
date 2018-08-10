#include <folly/init/Init.h>
#include <folly/concurrency/ConcurrentHashMap.h>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/executors/IOThreadPoolExecutor.h>

#include <wangle/service/Service.h>
#include <wangle/service/ExecutorFilter.h>
#include <wangle/service/ExpiringFilter.h>
#include <wangle/service/ClientDispatcher.h>
#include <wangle/service/ServerDispatcher.h>
#include <wangle/bootstrap/ClientBootstrap.h>
#include <wangle/channel/AsyncSocketHandler.h>
#include <wangle/codec/LengthFieldBasedFrameDecoder.h>
#include <wangle/codec/LengthFieldPrepender.h>
#include <wangle/codec/StringCodec.h>
#include <wangle/channel/EventBaseHandler.h>

#include "etcdclient.h"   //etcd client
#include "httpcodec.h"    //HTTP协议编码解码
#include "utility.h"


using namespace std;
using namespace folly;
using namespace wangle;

std::atomic_long request_id{0L};

DEFINE_string(etcdurl,"http://127.0.0.1:2379","ETCD Server Url.");          //ETCD服务器URL
DEFINE_int32(httpServerPort, 20000, "Async Http Server Listen Port.");      //异步HTTP服务器监听端口
DEFINE_int32(ClientIOWorkThreadPool, 1, "Agent Client IO Work Thread Pool Size.");             //Client IO工作线程池线程数量
DEFINE_int32(ServerIOWorkThreadPool, 1, "Async Http Server IO Work Thread Pool Size.");        //AsyncHttpServer IO工作线程池线程数量
DEFINE_int32(timeout,100,"http request timeout(/ms).");                     //AsyncHTTP请求超时时间，毫秒

/*
 * 服务注册器（服务注册与发现）
 * consumer-agent从etcd server查询提供服务的provider-agent endpoint列表
 * 2018/05/28
 */
//Endpoint结构体
struct Endpoint{
public:
    Endpoint(string ip, int p):ipAddress(ip),port(p){}
    string ipAddress;  //ip地址
    int port;       //端口

    string getUrl()    //endpoint url
    {
        ostringstream oss;
        oss<<ipAddress<<":"<<port;
        return oss.str();
    }
};

////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////Agent RPC客户端///////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////

//1. Agent Request
class AgentRequest{
public:
    int64_t req_id;             //dubbo request id
    string interfaceName;       //Service interface name
    string method;              //service method
    string parameterTypesString;//service method parameter type
    string parameter;           //service method parameter
};
//2. Agent response
class AgentResponse{
public:
    int64_t resp_id; //dubbo response id
    string result;   //response result
};

//3. Agent Client Pipeline
using AgentRpcClientPipeline  = wangle::Pipeline<IOBufQueue&, AgentRequest>;

//4. Consumer-agent -> provider-agent 协议序列化
class AgentRpcClientSerializeHandler : public wangle::Handler<
        std::unique_ptr<folly::IOBuf>,   //
        AgentResponse,                   //  Agent RPC 响应
        AgentRequest,                    //  Agent RPC 请求
        std::unique_ptr<folly::IOBuf> >  //
{
public:
    //1. 上游：下游发来的IOBuf消息解码成Agent response消息，发往上游处理
    void read(Context* ctx, std::unique_ptr<folly::IOBuf> ioBuf) override
    {
        if(ioBuf)
        {
            ioBuf->coalesce();  //移动IO缓冲区链中的所有数据到一个单一的连续缓冲区内
            AgentResponse received;
            for(int i=0;i<8;i++) { *( (char*)&received.resp_id + 7 - i ) = *( (const char*)ioBuf->data() + 4 + i ); }
            received.result.assign((const char*)ioBuf->data()+16,ioBuf->length()-16);
            ctx->fireRead(received);
        }
    }
    //2. 下游：将上游句柄发来的Agent RPC请求消息序列化，发往Provider-agent Server
    folly::Future<folly::Unit> write(Context* ctx, AgentRequest b) override
    {
        string request;
        //1. Dubbo version
        request.append("\"2.0.1\"");  request.append("\n");
        //2. InterfaceName
        request.append("\"");
        request.append(b.interfaceName);
        request.append("\"");         request.append("\n");
        //3. Service version
        request.append("null");       request.append("\n");
        //4. Method name
        request.append("\"");
        request.append(b.method);
        request.append("\"");         request.append("\n");
        //5. Method parameter types
        request.append("\"");
        request.append(b.parameterTypesString);
        request.append("\"");         request.append("\n");
        //6. parameter args
        request.append("\"");
        request.append(b.parameter);
        request.append("\"");         request.append("\n");
        //7. attachment
        request.append("{\"path\":\"");
        request.append(b.interfaceName);
        request.append("\"}");        request.append("\n");

        int len = request.length();

        // char headData[16];
        headData[0]=0xda;
        headData[1]=0xbb; //Magic
        headData[2]=0x06;  headData[2] |= (1<<7);  headData[2] |= (1<<6);
        headData[3]=0x00; //Status(not use in request)
        //request id (8 byte)
        for(int i=0;i<8;i++) { headData[4+i] = *((char*)&b.req_id + 7 - i); }
        //data length (4 byte)
        for(int i=15;i>=12;i--) { headData[i] = *(((char*)&len)+(15-i)); }

        auto buf = folly::IOBuf::copyBuffer(headData,16);

        buf->appendChain(folly::IOBuf::copyBuffer(request.data(),request.length()));

        return ctx->fireWrite(std::move(buf));
    }

private:
    char headData[16];
};

//5. 自定义Agent RPC客户端服务分发规则，带超时机制
class MultiAgentRpcClientDispatcher : public ClientDispatcherBase<AgentRpcClientPipeline, AgentRequest, AgentResponse>
{
public:
    // void read(Context*, Xtruct in) override
    void read(Context*, AgentResponse in) override
    {
        auto search = requests_.find(in.resp_id);
        if(search==requests_.cend())
        {
            //cout<<"not found request!!!"<<endl;
            return;
        }
        std::shared_ptr<Promise<AgentResponse>> p = search->second;
        requests_.erase(search);
        p->setValue(in);
    }
    //服务请求
    Future<AgentResponse> operator()(AgentRequest arg) override
    {

        std::shared_ptr<Promise<AgentResponse>> p = std::make_shared<Promise<AgentResponse>>();
        auto f = p->getFuture();
        requests_.insert(arg.req_id,p);

        this->pipeline_->write(arg);
        //return f;
        return f.onTimeout(std::chrono::milliseconds(FLAGS_timeout),   //设置请求超时时间（超时回复）
                           [arg,this]()
                           {
                               this->requests_.erase(arg.req_id);
                               AgentResponse timeoutResp;
                               timeoutResp.resp_id = arg.req_id;
                               timeoutResp.result = "111";
                               return std::move(timeoutResp);
                           });
    }
    // Print some nice messages for close
    Future<Unit> close() override
    {
        printf("Channel closed\n");
        return ClientDispatcherBase::close();
    }
    Future<Unit> close(Context* ctx) override
    {
        printf("Channel closed\n");
        return ClientDispatcherBase::close(ctx);
    }

private:
    folly::ConcurrentHashMap<int64_t, std::shared_ptr<Promise<AgentResponse>>> requests_;
    //std::mutex mu;
    //std::unordered_map<int64_t, Promise<AgentResponse>> requests_;
};

//6. Agent RPC client Pipeline
class AgentRpcClientPipelineFactory : public PipelineFactory<AgentRpcClientPipeline>
{
public:
    AgentRpcClientPipeline::Ptr  newPipeline(std::shared_ptr<AsyncTransportWrapper> socket) override
    {
        auto pipeline = AgentRpcClientPipeline::create();
        pipeline->addBack(AsyncSocketHandler(socket));
        pipeline->addBack(EventBaseHandler()); //ensure we can write from any thread
        pipeline->addBack(LengthFieldBasedFrameDecoder(4,4096,12,0,0));
        pipeline->addBack(AgentRpcClientSerializeHandler());
        pipeline->finalize();
        return pipeline;
    }
};
////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////异步HTTP服务器/////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////
using AsyncHttpServerPipeline = wangle::Pipeline<IOBufQueue&, std::string>;

/*
 * HTTPService
 * edit by chijinxin
 * 2018/06/15
 */
class HttpService : public Service<Request, Response>
{
public:
    //1. HttpService构造函数
    HttpService(std::shared_ptr<IOThreadPoolExecutor> ioExecutor)
            :timeout_(FLAGS_timeout),ioExecutor_(ioExecutor)
    {
        cout<<"Construct the HttpService Class!!!"<<endl;

        std::vector<Endpoint> endpoints;
//        Endpoint x1("10.10.10.3",30000);
//        Endpoint x2("10.10.10.4",30000);
//        Endpoint x3("10.10.10.5",30000);
//        endpoints.push_back(x1); endpoints.push_back(x2); endpoints.push_back(x3);
        Endpoint x1("127.0.0.1",20880);
        endpoints.push_back(x1);
        for(int i=0;i<endpoints.size();i++)
        {
            cout<<endpoints[i].getUrl()<<endl;

            SocketAddress addr(endpoints[i].ipAddress, endpoints[i].port);

            std::shared_ptr<ClientBootstrap<AgentRpcClientPipeline>> agentRpcClient
                    = std::make_shared<ClientBootstrap<AgentRpcClientPipeline>>();
            agentRpcClient->group(ioExecutor_);             //指定Agent Client工作线程池
            agentRpcClient->pipelineFactory(std::make_shared<AgentRpcClientPipelineFactory>());

            this->vAgentClient_.push_back(agentRpcClient);  //保存Agent Client到数组中

            //连接远程Provider-agent服务器
            agentRpcClient->connect(addr).via(ioExecutor_->getEventBase())
                    .then([this, addr](AgentRpcClientPipeline* pipeline)
                          {
                              cout<<"connect to "<<addr<<"success!!!"<<endl;
                              std::shared_ptr<MultiAgentRpcClientDispatcher> dispatcher
                                      = std::make_shared<MultiAgentRpcClientDispatcher>();
                              dispatcher->setPipeline(pipeline);
                              this->ProviderAgentClientDispather_.push_back(dispatcher);  //保存RPC客户端到数组中去
                          });
        }
    }

    //2. ServiceName(Request)->Future<Response>
    Future<Response> operator()(Request request) override
    {
        //1. 解析请求数据，获取content-type:application/x-www.form-url请求表单数据
        vector<string> FormDataSet;
        StringSplit(request.body(), "&", 0, FormDataSet); //字符串分割“&”

        if (FormDataSet.size() != 4) {
            LOG(ERROR) << "[class HttpService]:FormDataSet!=4";
            Response resp("HTTP/1.1 200 OK\ncontent-length:3\ncontent-type:application/x-www-form-urlencoded\n\n123\n");
            return std::move(resp);
        }
        //2. 用表单数据填充AgentRequest Body
        AgentRequest agentRequest;
        agentRequest.req_id = request_id++;  //request_id ++
        agentRequest.interfaceName        = FormDataSet[0].substr(FormDataSet[0].find("=")+1,FormDataSet[0].npos);
        agentRequest.method               = FormDataSet[1].substr(FormDataSet[1].find("=")+1,FormDataSet[1].npos);
        agentRequest.parameterTypesString = UrlDecode(FormDataSet[2].substr(FormDataSet[2].find("=")+1,FormDataSet[2].npos));
        agentRequest.parameter            = UrlDecode(FormDataSet[3].substr(FormDataSet[3].find("=")+1,FormDataSet[3].npos));

        int index = agentRequest.req_id % ProviderAgentClientDispather_.size();  //轮询负载均衡
        if(index<0 || index >=ProviderAgentClientDispather_.size() ) index=0;

        return futures::sleep(std::chrono::milliseconds(80))
                .then([]()
                      {
                          Response resp("HTTP/1.1 200 OK\ncontent-length:3\ncontent-type:application/x-www-form-urlencoded\n\n123\n");
                          return std::move(resp);
                      });
        return (*(ProviderAgentClientDispather_[index]))(std::move(agentRequest)).via(ioExecutor_->getEventBase())
                .then([this](AgentResponse response)
                      {
                          //cout<<"["<<agentRequest.req_id<<","<<response.resp_id<<"]"<<endl;
                          if(response.result.at(0)!='1')
                          {
                              Response resp("HTTP/1.1 200 OK\ncontent-length:3\ncontent-type:application/x-www-form-urlencoded\n\n123\n");
                              return std::move(resp);
                          }
                          else
                          {
                              ostringstream oss;
                              oss<<"HTTP/1.1 200 OK\ncontent-length:";
                              oss<<response.result.length()-3;
                              oss<<"\ncontent-type:application/x-www-form-urlencoded\n\n";
                              oss<<response.result.substr(2,response.result.length()-3);
                              return oss.str();
                          }
                      })
                .onError([](const std::exception& e)
                         {
                             cerr << "rpc request error: " << exceptionStr(e);
                             Response resp("HTTP/1.1 200 OK\ncontent-length:3\ncontent-type:application/x-www-form-urlencoded\n\n123\n");
                             return std::move(resp);
                         });
    }

private:
    std::vector<std::shared_ptr<MultiAgentRpcClientDispatcher>> ProviderAgentClientDispather_;
    std::vector<std::shared_ptr<ClientBootstrap<AgentRpcClientPipeline>>> vAgentClient_;

    int timeout_;              //请求超时时间
    std::shared_ptr<IOThreadPoolExecutor> ioExecutor_;


public:
    /*
    * content-type:application/x-www.form-url
    * application/x-www.form-url的C++实现
    * edit by chijinxin
    * 2018/05/30
    */
    unsigned char ToHex(unsigned char x)
    {
        return  x > 9 ? x + 55 : x + 48;
    }

    unsigned char FromHex(unsigned char x)
    {
        unsigned char y;
        if (x >= 'A' && x <= 'Z') y = x - 'A' + 10;
        else if (x >= 'a' && x <= 'z') y = x - 'a' + 10;
        else if (x >= '0' && x <= '9') y = x - '0';
        else assert(0);
        return y;
    }
    //application/x-www.form-url编码
    std::string UrlEncode(const std::string& str)
    {
        std::string strTemp = "";
        size_t length = str.length();
        for (size_t i = 0; i < length; i++)
        {
            if (isalnum((unsigned char)str[i]) ||
                (str[i] == '-') ||
                (str[i] == '_') ||
                (str[i] == '.') ||
                (str[i] == '~'))
                strTemp += str[i];
            else if (str[i] == ' ')
                strTemp += "+";
            else
            {
                strTemp += '%';
                strTemp += ToHex((unsigned char)str[i] >> 4);
                strTemp += ToHex((unsigned char)str[i] % 16);
            }
        }
        return strTemp;
    }
    //application/x-www.form-url解码
    std::string UrlDecode(const std::string& str)
    {
        std::string strTemp = "";
        size_t length = str.length();
        for (size_t i = 0; i < length; i++)
        {
            if (str[i] == '+') strTemp += ' ';
            else if (str[i] == '%')
            {
                assert(i + 2 < length);
                unsigned char high = FromHex((unsigned char)str[++i]);
                unsigned char low = FromHex((unsigned char)str[++i]);
                strTemp += high*16 + low;
            }
            else strTemp += str[i];
        }
        return strTemp;
    }
};

/*
 * AsyncHttpServer pipelineFactory
 * @ 异步HTTP服务器，consumer-agent监听20000端口截获Dubbo-consumer消息，向provider-agent端请求数据
 * @ AsyncSocketHandler(socket)->GreedyInputDecoder()->StringCodec()->HttpCodec()->HttpService process->finalize
 * edit by chijinxin
 * 2018/06/15
 */
class AsyncHttpServerPipelineFactory : public PipelineFactory<AsyncHttpServerPipeline>
{
public:
    //1. AsyncHttpServerPipelineFactory构造函数
    AsyncHttpServerPipelineFactory(std::shared_ptr<IOThreadPoolExecutor> serviceExecutor, std::shared_ptr<IOThreadPoolExecutor> ioExecutor)
           //: service_(serviceExecutor, std::make_shared<HttpService>(ioExecutor))
            : service_(ioExecutor)
    {}
    //2. Pipeline初始化
    AsyncHttpServerPipeline::Ptr newPipeline(std::shared_ptr<AsyncTransportWrapper> socket) override
    {
        auto pipeline = AsyncHttpServerPipeline::create();
        pipeline->addBack(AsyncSocketHandler(socket));  //底层异步socket消息处理句柄
        pipeline->addBack(EventBaseHandler());          //ensure we can write from any thread
        pipeline->addBack(GreedyInputDecoder());        //IOBufQueue -> IOBuf 数据透传
        pipeline->addBack(StringCodec());               //IOBuf -> std::string
        pipeline->addBack(HttpCodec());                 //std::string -> http::request
        pipeline->addBack(MultiplexServerDispatcher<Request, Response>(&service_)); //http::request -> http::Service
        pipeline->finalize();
        return pipeline;
    }

private:
    HttpService service_;     //AsyncHttpService
   //ExecutorFilter<Request, Response> service_;
};




////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////Consumer Agent///////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////
/*
 * Consumer-Agent主函数
 * edit by chijinxin
 * 2018/06/15
 */
int main(int argc, char** argv)
{
    folly::Init init(&argc, &argv);
    cout<<"Consumer Start!!!"<<endl;

    ServerBootstrap<AsyncHttpServerPipeline> asyncHttpServer;
    std::shared_ptr<IOThreadPoolExecutor> HttpIOWorkThreadPool = std::make_shared<IOThreadPoolExecutor>(2);  //4个IO工作线程池
 //   std::shared_ptr<IOThreadPoolExecutor> ClientIOWorkThreadPool = std::make_shared<IOThreadPoolExecutor>(2);  //4个IO工作线程池

    //启动Provider-Agent的异步HTTP服务器

    asyncHttpServer.childPipeline(std::make_shared<AsyncHttpServerPipelineFactory>(HttpIOWorkThreadPool,HttpIOWorkThreadPool));
    asyncHttpServer.group(std::make_shared<IOThreadPoolExecutor>(2),HttpIOWorkThreadPool);
    asyncHttpServer.bind(FLAGS_httpServerPort);     //listening port

    asyncHttpServer.waitForStop();
    return 0;
}
