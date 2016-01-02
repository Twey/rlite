#include <iostream>
#include <map>
#include <fstream>
#include <sstream>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <cerrno>
#include <unistd.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include "rlite/common.h"
#include "rlite/utils.h"
#include "rlite/appl.h"

using namespace std;


struct InetName {
    struct sockaddr_in addr;

    InetName() { memset(&addr, 0, sizeof(addr)); }
    InetName(const struct sockaddr_in& a) : addr(a) { }

    bool operator<(const InetName& other) const;
    operator std::string() const;
};

bool
InetName::operator<(const InetName& other) const
{
    if (addr.sin_addr.s_addr != other.addr.sin_addr.s_addr) {
        return addr.sin_addr.s_addr < other.addr.sin_addr.s_addr;
    }

    return addr.sin_port < other.addr.sin_port;
}

InetName::operator std::string() const
{
    char strbuf[20];
    stringstream ss;

    inet_ntop(AF_INET, &addr.sin_addr, strbuf, sizeof(strbuf));

    ss << strbuf << ":" << ntohs(addr.sin_port);

    return ss.str();
}

struct RinaName {
    string name_s;
    struct rina_name name_r;
    string dif_name_s;
    struct rina_name dif_name_r;

    RinaName();
    RinaName(const string& n, const string& d);
    RinaName(const RinaName& other);
    RinaName& operator=(const RinaName& other);
    ~RinaName();

    bool operator<(const RinaName& other) const {
        return name_s < other.name_s;
    }

    operator std::string() const { return dif_name_s + ":" + name_s; }
};

RinaName::RinaName()
{
    memset(&name_r, 0, sizeof(name_r));
    memset(&dif_name_r, 0, sizeof(dif_name_r));
}

RinaName::~RinaName()
{
    rina_name_free(&name_r);
    rina_name_free(&dif_name_r);
}

RinaName::RinaName(const string& n, const string& d) : name_s(n), dif_name_s(d)
{
    memset(&name_r, 0, sizeof(name_r));
    memset(&dif_name_r, 0, sizeof(dif_name_r));

    if (rina_name_from_string(n.c_str(), &name_r)) {
        throw std::bad_alloc();
    }

    if (rina_name_from_string(d.c_str(), &dif_name_r)) {
        throw std::bad_alloc();
    }
}

RinaName::RinaName(const RinaName& other)
{
    memset(&name_r, 0, sizeof(name_r));

    name_s = other.name_s;
    dif_name_s = other.dif_name_s;
    if (rina_name_copy(&name_r, &other.name_r)) {
        throw std::bad_alloc();
    }
    if (rina_name_copy(&dif_name_r, &other.dif_name_r)) {
        throw std::bad_alloc();
    }
}

RinaName&
RinaName::operator=(const RinaName& other)
{
    if (this == &other) {
        return *this;
    }

    name_s = other.name_s;
    dif_name_s = other.dif_name_s;
    if (rina_name_copy(&name_r, &other.name_r)) {
        throw std::bad_alloc();
    }
    if (rina_name_copy(&dif_name_r, &other.dif_name_r)) {
        throw std::bad_alloc();
    }

    return *this;
}

struct Gateway {
    struct rlite_appl appl;
    struct rina_name appl_name;

    /* Used to map IP:PORT --> RINA_NAME, when
     * receiving TCP connection requests from the INET world
     * towards the RINA world. */
    map<InetName, RinaName> srv_map;
    map<int, RinaName> srv_fd_map;

    /* Used to map RINA_NAME --> IP:PORT, when
     * receiving flow allocation requests from the RINA world
     * towards the INET world. */
    map<RinaName, InetName> dst_map;

    /* Pending flow allocation requests issued by accept_inet_conn().
     * fa_req_event_id --> tcp_client_fd */
    map<unsigned int, int> pending_fa_reqs;

    Gateway();
    ~Gateway();
};

Gateway::Gateway()
{
    rina_name_fill(&appl_name, "rina-gw", "1", NULL, NULL);

    if (rlite_appl_init(&appl)) {
        throw std::exception();
    }

    rlite_ipcps_fetch(&appl.loop);
}

Gateway::~Gateway()
{
    for (map<int, RinaName>::iterator mit = srv_fd_map.begin();
                                    mit != srv_fd_map.end(); mit++) {
        close(mit->first);
    }

    rlite_appl_fini(&appl);
}

Gateway gw;

static int
parse_conf(const char *confname)
{
    ifstream fin(confname);

    if (fin.fail()) {
        PE("Failed to open configuration file '%s'\n", confname);
        return -1;
    }

    for (unsigned int lines_cnt = 1; !fin.eof(); lines_cnt++) {
        vector<string> tokens;
        string token;
        string line;
        int ret;

        getline(fin, line);

        {
            istringstream iss(line);
            struct sockaddr_in inet_addr;

            while (iss >> token) {
                tokens.push_back(token);
            }

            if (tokens.size() < 5) {
                if (tokens.size()) {
                    PI("Invalid configuration entry at line %d\n", lines_cnt);
                }
                continue;
            }

            try {
                memset(&inet_addr, 0, sizeof(inet_addr));
                inet_addr.sin_family = AF_INET;
                inet_addr.sin_port = atoi(tokens[4].c_str());
                if (inet_addr.sin_port >= 65536) {
                    PI("Invalid configuration entry at line %d: "
                       "invalid port number '%s'\n", lines_cnt,
                       tokens[4].c_str());
                }
                ret = inet_pton(AF_INET, tokens[3].c_str(),
                                &inet_addr.sin_addr);
                if (ret != 1) {
                    PI("Invalid configuration entry at line %d: "
                       "invalid IP address '%s'\n", lines_cnt,
                       tokens[3].c_str());
                    continue;
                }

                InetName inet_name(inet_addr);
                RinaName rina_name(tokens[2], tokens[1]);

                if (tokens[0] == "SRV") {
                    gw.srv_map.insert(make_pair(inet_name, rina_name));

                } else if (tokens[0] == "DST") {
                    gw.dst_map.insert(make_pair(rina_name, inet_name));

                } else {
                    PI("Invalid configuration entry at line %d: %s is "
                       "unknown\n", lines_cnt, tokens[0].c_str());
                    continue;
                }
            } catch (std::bad_alloc) {
                PE("Out of memory while processing configuration file\n");
            }
        }
    }

    return 0;
}

static int
gw_fa_req_arrived(struct rlite_evloop *loop,
                  const struct rina_msg_base_resp *b_resp,
                  const struct rina_msg_base *b_req)
{
    return 0;
}

static int
gw_fa_resp_arrived(struct rlite_evloop *loop,
                   const struct rina_msg_base_resp *b_resp,
                   const struct rina_msg_base *b_req)
{
    return 0;
}

static void
accept_inet_conn(struct rlite_evloop *loop, int lfd)
{
    struct rlite_appl *appl = container_of(loop, struct rlite_appl, loop);
    Gateway * gw = container_of(appl, struct Gateway, appl);
    struct sockaddr_in remote_addr;
    socklen_t addrlen = sizeof(remote_addr);
    map<int, RinaName>::iterator mit;
    struct rina_flow_spec flowspec;
    unsigned int event_id;
    unsigned int unused;
    int cfd;
    int ret;

    /* First of all let's call accept, so that we consume the event
     * on lfd, independently of what happen next. */
    cfd = accept(lfd, (struct sockaddr *)&remote_addr, &addrlen);
    if (cfd < 0) {
        PE("accept() failed\n");
        return;
    }

    mit = gw->srv_fd_map.find(lfd);
    if (mit == gw->srv_fd_map.end()) {
        PE("Internal error: Failed to lookup lfd %d into srv_fd_map\n", lfd);
        return;
    }

    strcpy(flowspec.cubename, "rel");
    event_id = rlite_evloop_get_id(loop);

    /* Issue a non-blocking flow allocation request. */
    ret = rlite_flow_allocate(appl, event_id, &mit->second.dif_name_r, NULL,
                              &gw->appl_name, &mit->second.name_r, &flowspec,
                              &unused, 0, 0);
    if (ret) {
        PE("Flow allocation failed");
        return;
    }

    gw->pending_fa_reqs[event_id] = cfd;

    PD("Flow allocation request issued, event id %d\n", event_id);
}

static int
inet_server_socket(const InetName& inet_name)
{
    int enable = 1;
    int fd;

    fd = socket(PF_INET, SOCK_STREAM, 0);

    if (fd < 0) {
        PE("socket() failed [%d]\n", errno);
        return -1;
    }

    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enable,
                   sizeof(enable))) {
        PE("setsockopt(SO_REUSEADDR) failed [%d]\n", errno);
        close(fd);
        return -1;
    }

    if (bind(fd, (struct sockaddr *)&inet_name.addr, sizeof(inet_name.addr))) {
        PE("bind() failed [%d]\n", errno);
        close(fd);
        return -1;
    }

    if (listen(fd, 10)) {
        PE("listen() failed [%d]\n", errno);
        close(fd);
        return -1;
    }

    if (rlite_evloop_fdcb_add(&gw.appl.loop, fd, accept_inet_conn)) {
        PE("rlite_evloop_fcdb_add() failed [%d]\n", errno);
        close(fd);
        return -1;
    }

    return 0;
}

static int
setup()
{
    int ret;

    /* Register the handler for incoming flow allocation requests and
     * response, since we'll not be using rlite/appl.h functionalities for
     * that. */
    ret = rlite_evloop_set_handler(&gw.appl.loop, RINA_KERN_FA_REQ_ARRIVED,
                                   gw_fa_req_arrived);
    ret |= rlite_evloop_set_handler(&gw.appl.loop, RINA_KERN_FA_RESP_ARRIVED,
                                    gw_fa_resp_arrived);
    if (ret) {
        return -1;
    }

    for (map<InetName, RinaName>::iterator mit = gw.srv_map.begin();
                                    mit != gw.srv_map.end(); mit++) {
        int fd = inet_server_socket(mit->first);

        if (fd < 0) {
            PE("Failed to open listening socket for '%s'\n",
               static_cast<string>(mit->first).c_str());
        } else {
            gw.srv_fd_map[fd] = mit->second;
        }
    }

    for (map<RinaName, InetName>::iterator mit = gw.dst_map.begin();
                                    mit != gw.dst_map.end(); mit++) {
        rlite_appl_register_wait(&gw.appl, 1, &mit->first.dif_name_r, NULL,
                                 &mit->first.name_r, 3000);
        if (ret) {
            PE("Registration of application '%s'\n",
               static_cast<string>(mit->first).c_str());
        }
    }

    return 0;
}

static void
print_conf()
{
    for (map<InetName, RinaName>::iterator mit = gw.srv_map.begin();
                                    mit != gw.srv_map.end(); mit++) {
        cout << "SRV: " << static_cast<string>(mit->first) << " --> "
                << static_cast<string>(mit->second) << endl;
    }

    for (map<RinaName, InetName>::iterator mit = gw.dst_map.begin();
                                    mit != gw.dst_map.end(); mit++) {
        cout << "DST: " << static_cast<string>(mit->first) << " --> "
                << static_cast<string>(mit->second) << endl;
    }
}

int main()
{
    const char *confname = "rina-gw.conf";
    int ret;

    ret = parse_conf(confname);
    if (ret) {
        return ret;
    }

    print_conf();
    setup();

    return 0;
}
