//
// Created by chijinxin on 18-6-14.
//

#ifndef WANGLE_ETCDCLIENT_H
#define WANGLE_ETCDCLIENT_H

#include <thread>
#include <string>
#include "utility.h"
//etcd v3 cppClient
#include <etcd/SyncClient.hpp>
//boost
#include <boost/format.hpp>
//linux ip address
#include <unistd.h>
#include <ifaddrs.h>
#include <string.h>
#include <arpa/inet.h>
using namespace std;
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
//etcd服务注册与发现
class EtcdRegistry{
public:
    //etcd服务注册器构造函数
    EtcdRegistry(string etcdurl) :etcd_(etcdurl),rootPath("dubbomesh"),fmt_provider("/%s/%s/%s:%d"), fmt_consumer("/%s/%s") {}
    ~EtcdRegistry()
    {
        cout<<"EtcdRegistry destruct!!!"<<endl;
    }

    /*
     * Provider端注册服务
     */
    void registerService(string serviceName, int port)
    {
        int RetryNum = 10; //注册服务重试次数
        //服务注册的key为： /dubbomesh/com.alibaba.dubbo.performance.demo.provider.IHelloService
        string hostIP = getDocker0IPAddr(); //docker0网卡ip地址
        string strKey = (fmt_provider%rootPath%serviceName%hostIP%port).str();
        cout<<"register strKey:["<<strKey<<"] to etcd server =============================="<<endl;
        int retrycount = 0;
        while (retrycount<RetryNum)
        {
            retrycount++;
            etcd::Response resp = etcd_.add(strKey, "");  //目前只需要创建key，对应的value暂时不用，先留空
            int error_code = resp.error_code();
            if(error_code!=0)  //error_code == 0 正确
            {
                if(error_code==105)  //Key already exists
                {
                    cout<<"[etcd register error]:Key already exits!"<<endl;
                    break;
                }
            }
            else
            {
                if(0 == etcd_.get(strKey).error_code())  // Key not found
                {
                    cout<<"[etcd register success]: provider service register success!";
                    break;
                }
            }
            this_thread::sleep_for(std::chrono::milliseconds(50));
        }

    }


    /*
     * consumer端 获取可以提供服务的Endpoint列表
     */
    vector<Endpoint> findService(string serviceName)
    {
        vector<Endpoint> result;  //获取结果
        string strKey = (fmt_consumer%rootPath%serviceName).str(); //Key = rootPath + serviceName
        etcd::Response response = etcd_.ls(strKey);   //获取strKey的响应
        if(response.error_code()!=0)  //response error
        {
            LOG(ERROR)<<"[etcd get key error ]: error_code="<<response.error_code();
        }
        else
        {
            etcd::Keys keys = response.keys();  //获取以strKey为前缀的所有Keys
            for(int i=0;i<keys.size();i++)
            {
                int port;
                vector<string> vec1,vec2;
                StringSplit(keys[i],"/",0,vec1); //字符串分割"/"
                StringSplit(vec1[3],":",0,vec2); //字符串分割":"
                ss.clear();
                ss<<vec2[1];
                ss>>port;
                Endpoint ep(vec2[0],port);
                result.push_back(ep);
            }
        }
        return result;
    }

    /*
     * 获取docker0网卡的ip地址
     */
    string getDocker0IPAddr()
    {
        string result;
        struct ifaddrs *ifap0=NULL, *ifap=NULL;
        void * tmpAddrPtr=NULL;
        getifaddrs(&ifap0);
        ifap=ifap0;
        bool flag= false ;
        while (ifap!=NULL)
        {
            if (ifap->ifa_addr->sa_family==AF_INET)
            {
                tmpAddrPtr=&((struct sockaddr_in *)ifap->ifa_addr)->sin_addr;
                char addressBuffer[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, tmpAddrPtr, addressBuffer, INET_ADDRSTRLEN);
                if(strcmp(addressBuffer,"127.0.0.1")!=0)
                {
                    if(strcmp(ifap->ifa_name,"eth0")==0)
                    {
                        result = addressBuffer;
                        flag = true;
                    }
                }
            }
            ifap=ifap->ifa_next;
        }
        if (ifap0) { freeifaddrs(ifap0); ifap0 = NULL; }
        if(flag) return result;
        else
        {
            LOG(ERROR)<<"[ETCD ERROR]: Not found eth0 !";
            return result;
        }
    }


private:
    stringstream ss;
    etcd::SyncClient etcd_;   //etcd 客户端
    boost::format fmt_provider;    //boost格式化字符串(provider端)
    boost::format fmt_consumer;    //boost格式化字符串(consumer端)
    string rootPath;      //etcd key rootPath
};

#endif //WANGLE_ETCDCLIENT_H
