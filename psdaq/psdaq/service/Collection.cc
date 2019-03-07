#include <iostream>
#include <iomanip>
#include <unistd.h>
#include <limits.h>
#include <ifaddrs.h>
#include <linux/if_packet.h>
#include <netinet/ether.h>
#include <netdb.h>
#include "Collection.hh"

json createMsg(const std::string& key, const std::string& msg_id, size_t sender_id, json& body)
{
    json msg;
    msg["header"] = { {"key", key}, {"msg_id", msg_id}, {"sender_id", sender_id} };
    msg["body"] = body;
    return msg;
}

std::string getNicIp()
{
    struct ifaddrs* ifaddr;
    getifaddrs(&ifaddr);

    char host[NI_MAXHOST];
    char* interface_name = nullptr;
    char* ethernet_name  = nullptr;
    // find name of first infiniband, otherwise fall back ethernet
    for (struct ifaddrs* ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL) {
            continue;
        }
        int family = ifa->ifa_addr->sa_family;
        if (family == AF_PACKET) {
            struct sockaddr_ll* s = (struct sockaddr_ll*)ifa->ifa_addr;
            if (s->sll_hatype == ARPHRD_INFINIBAND) {
                if (!interface_name) interface_name = ifa->ifa_name;
            }
            else if (s->sll_hatype == ARPHRD_ETHER) {
                if (!ethernet_name) ethernet_name  = ifa->ifa_name;
            }
        }
    }
    if (interface_name == nullptr) {
        printf("Warning: No infiniband device found!");
        if (ethernet_name == nullptr) {
            printf("  And no ethernet either!\n");
            return std::string();
        }
        printf("  Falling back to ethernet.\n");
        interface_name = ethernet_name;
    }

    // get address of the first infiniband device found above
    for (struct ifaddrs* ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL) {
            continue;
        }
        int family = ifa->ifa_addr->sa_family;

        if ((family == AF_INET) && (strcmp(ifa->ifa_name, interface_name)==0)) {
            int s = getnameinfo(ifa->ifa_addr, sizeof(struct sockaddr_in),
                                 host, NI_MAXHOST, NULL, 0, NI_NUMERICHOST);
            if (s != 0) {
                printf("getnameinfo() failed: %s\n", gai_strerror(s));
            }
            printf("address %s: <%s>\n", ifa->ifa_name, host);
        }
    }
    freeifaddrs(ifaddr);
    return std::string(host);
}

ZmqSocket::ZmqSocket(ZmqContext* context, int type) : m_context(context)
{
    socket = zmq_socket((*m_context)(), type);
}

void ZmqSocket::connect(const std::string& host)
{
    int rc = zmq_connect(socket, host.c_str());
    if (rc != 0) {
        throw std::runtime_error{"zmq_connect failed with error " + std::to_string(rc)};
    }
}

void ZmqSocket::bind(const std::string& host)
{
    int rc = zmq_bind(socket, host.c_str());
    if (rc != 0) {
        throw std::runtime_error{"zmq_bind failed with error " + std::to_string(rc)};
    }
}

void ZmqSocket::setsockopt(int option, const void* optval, size_t optvallen)
{
    int rc = zmq_setsockopt(socket, option, optval, optvallen);
    if (rc != 0) {
        throw std::runtime_error{"zmq_setsockopt failed with error " + std::to_string(rc)};
    }
}


json ZmqSocket::recvJson()
{
    ZmqMessage frame;
    int rc = zmq_msg_recv(&frame.msg, socket, 0);
    assert (rc != -1);
    char* begin = (char*)frame.data();
    char* end   = begin + frame.size();
    return json::parse(begin, end);
}

std::vector<ZmqMessage> ZmqSocket::recvMultipart()
{
    std::vector<ZmqMessage> frames;
    int more;
    size_t more_size = sizeof(more);
    do {
        ZmqMessage frame;
        int rc = zmq_msg_recv(&frame.msg, socket, 0);
        assert (rc != -1);
        frames.emplace_back(std::move(frame));
        rc = zmq_getsockopt(socket, ZMQ_RCVMORE, &more, &more_size);
        assert(rc == 0);
    } while (more);
    return frames;
}

void ZmqSocket::send(const std::string& msg)
{
    int ret = zmq_send(socket, msg.c_str(), msg.length(), 0);
    if (ret == -1) {
        std::cout<<"Error sending zmq message:  "<<msg<<'\n';
    }
}

int ZmqSocket::poll(short events, long timeout)
{
    zmq_pollitem_t item;
    item.socket = socket;
    item.events = events;
    return zmq_poll(&item, 1, timeout);
}


CollectionApp::CollectionApp(const std::string &managerHostname,
                             int platform,
                             const std::string &level) :
    m_level(level),
    m_pushSocket{&m_context, ZMQ_PUSH},
    m_subSocket{&m_context, ZMQ_SUB}
{
    const int base_port = 29980;

    m_pushSocket.connect({"tcp://" + managerHostname + ":" + std::to_string(base_port + platform)});

    m_subSocket.connect({"tcp://" + managerHostname + ":" + std::to_string(base_port + 10 + platform)});
    m_subSocket.setsockopt(ZMQ_SUBSCRIBE, "", 0);
    std::cout<<std::string{"tcp://" + managerHostname + ":" + std::to_string(base_port + 10 + platform)}<<std::endl;

    // register callbacks
    m_handleMap["plat"] = std::bind(&CollectionApp::handlePlat, this, std::placeholders::_1);
    m_handleMap["alloc"] = std::bind(&CollectionApp::handleAlloc, this, std::placeholders::_1);
    m_handleMap["connect"] = std::bind(&CollectionApp::handleConnect, this, std::placeholders::_1);
    m_handleMap["configure"] = std::bind(&CollectionApp::handleConfigure, this, std::placeholders::_1);
    m_handleMap["reset"] = std::bind(&CollectionApp::handleReset, this, std::placeholders::_1);
}

void CollectionApp::handlePlat(const json &msg)
{
    char hostname[HOST_NAME_MAX];
    gethostname(hostname, HOST_NAME_MAX);
    int pid = getpid();
    m_id = std::hash<std::string>{}(std::string(hostname) + std::to_string(pid));
    json body;
    body[m_level] = {{"proc_info", {{"host", hostname}, {"pid", pid}}}};
    json answer = createMsg("plat", msg["header"]["msg_id"], m_id, body);
    reply(answer);
}

void CollectionApp::handleAlloc(const json &msg)
{
    std::string nicIp = getNicIp();
    std::cout<<"nic ip  "<<nicIp<<'\n';
    json body = {{m_level, {{"connect_info", {{"nic_ip", nicIp}}}}}};
    json answer = createMsg("alloc", msg["header"]["msg_id"], m_id, body);
    reply(answer);
}

void CollectionApp::reply(const json& msg)
{
    m_pushSocket.send(msg.dump());
}

void CollectionApp::run()
{
    while (1) {
        json msg = m_subSocket.recvJson();
        std::string key = msg["header"]["key"];
        std::cout<<"received key = "<<key<<'\n';
        std::cout << std::setw(4) << msg << "\n\n";
        if (m_handleMap.find(key) == m_handleMap.end()) {
            std::cout<<"unknown key  "<<key<<'\n';
        }
        else {
            m_handleMap[key](msg);
        }
    }
}
