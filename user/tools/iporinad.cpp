#include <cstdlib>
#include <cstring>
#include <iostream>
#include <fstream>
#include <string>
#include <list>
#include <vector>
#include <sstream>
#include <algorithm>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <poll.h>
#include <rina/api.h>
#include <pthread.h>

#include <rina/cdap.hpp>
#include "iporina.pb.h"

using namespace std;

/*
 * Internal data structures.
 */

struct IPSubnet {
    string      repr;
    uint32_t    netaddr;
    unsigned    netbits;

    IPSubnet() : netaddr(0), netbits(0) { }
    IPSubnet(const string &p);
};

struct Local {
    string app_name;
    string dif_name;

    Local(const string &a, const string &d) : app_name(a), dif_name(d) { }
};

struct Remote {
    string app_name;
    string dif_name;
    IPSubnet tun_subnet;
    string tun_name;
    int tun_fd;

    /* Flow for control connection. */
    int rfd;

    Remote() : tun_fd(-1) { }
    Remote(const string &a, const string &d, const IPSubnet &i) : app_name(a),
                        dif_name(d), tun_subnet(i), tun_fd(-1), rfd(-1) { }
};

struct Route {
    IPSubnet subnet;

    Route(const IPSubnet &i) : subnet(i) { }
};

struct IPoRINA {
    /* Enable verbose mode */
    int verbose;

    /* Control device to listen for incoming connections. */
    int rfd;

    list<Local>     locals;
    list<Remote>    remotes;
    list<Route>     routes;

    IPoRINA() : verbose(0), rfd(-1) { }
};

/*
 * CDAP objects with their serialization and deserialization routines
 */

struct Msg {
    virtual int serialize(char *buf, unsigned int size) const = 0;
    virtual ~Msg() { }
};

static int
ser_common(::google::protobuf::MessageLite &gm, char *buf,
           int size)
{
    if (gm.ByteSize() > size) {
        fprintf(stderr, "User buffer too small [%u/%u]\n",
                gm.ByteSize(), size);
        return -1;
    }

    gm.SerializeToArray(buf, size);

    return gm.ByteSize();
}

struct HelloMsg : public Msg {
    string myname;      /* Name of the sender. */
    string yourname;    /* Name of the receiver. */
    string tun_subnet;  /* Subnet to be used for the tunnel */
    uint32_t num_routes; /* How many route to exchange */

    HelloMsg() : num_routes(0) { }
    HelloMsg(const char *buf, unsigned int size);
    int serialize(char *buf, unsigned int size) const;
};

static void
gpb2HelloMsg(HelloMsg &m, const gpb::hello_msg_t &gm)
{
    m.myname = gm.myname();
    m.yourname = gm.yourname();
    m.tun_subnet = gm.tun_subnet();
    m.num_routes = gm.num_routes();
}

static int
HelloMsg2gpb(const HelloMsg &m, gpb::hello_msg_t &gm)
{
    gm.set_myname(m.myname);
    gm.set_yourname(m.yourname);
    gm.set_tun_subnet(m.tun_subnet);
    gm.set_num_routes(m.num_routes);

    return 0;
}

HelloMsg::HelloMsg(const char *buf, unsigned int size) : num_routes(0)
{
    gpb::hello_msg_t gm;

    gm.ParseFromArray(buf, size);

    gpb2HelloMsg(*this, gm);
}

int
HelloMsg::serialize(char *buf, unsigned int size) const
{
    gpb::hello_msg_t gm;

    HelloMsg2gpb(*this, gm);

    return ser_common(gm, buf, size);
}

struct RouteMsg : public Msg {
    string route;   /* Route represented as a string. */

    RouteMsg() { }
    RouteMsg(const char *buf, unsigned int size);
    int serialize(char *buf, unsigned int size) const;
};

static void
gpb2RouteMsg(RouteMsg &m, const gpb::route_msg_t &gm)
{
    m.route = gm.route();
}

static int
RouteMsg2gpb(const RouteMsg &m, gpb::route_msg_t &gm)
{
    gm.set_route(m.route);

    return 0;
}

RouteMsg::RouteMsg(const char *buf, unsigned int size)
{
    gpb::route_msg_t gm;

    gm.ParseFromArray(buf, size);

    gpb2RouteMsg(*this, gm);
}

int
RouteMsg::serialize(char *buf, unsigned int size) const
{
    gpb::route_msg_t gm;

    RouteMsg2gpb(*this, gm);

    return ser_common(gm, buf, size);
}

static int
cdap_send(int fd, CDAPMessage *m, int invoke_id, const Msg *obj)
{
    char objbuf[4096];
    int objlen;

    if (obj) {
        objlen = obj->serialize(objbuf, sizeof(objbuf));
        if (objlen < 0) {
            errno = EINVAL;
            fprintf(stderr, "serialization failed\n");
            return objlen;
        }

        m->set_obj_value(objbuf, objlen);
    }

    return 0;
}

/*
 * Global variables to hold the daemon state.
 */

static IPoRINA _g;
static IPoRINA *g = &_g;

#if 0
static string
int2string(int x)
{
    stringstream sstr;
    sstr << x;
    return sstr.str();
}
#endif

static int
string2int(const string& s, int& ret)
{
    char *dummy;
    const char *cstr = s.c_str();

    ret = strtoul(cstr, &dummy, 10);
    if (!s.size() || *dummy != '\0') {
        ret = ~0U;
        return -1;
    }

    return 0;
}

IPSubnet::IPSubnet(const string &_p) : repr(_p)
{
    stringstream ss;
    string p = _p;
    string digit;
    size_t slash;
    int m;

    slash = p.find("/");

    if (slash == string::npos) {
        goto ex;
    }

    /* Extract the mask m in "a.b.c.d/m" */
    if (string2int(p.substr(slash + 1), m)) {
        goto ex;
    }
    if (m < 1 || m > 30) {
        goto ex;
    }
    netbits = m;
    p = p.substr(0, slash);

    /* Extract a, b, c and d. */
    std::replace(p.begin(), p.end(), '.', ' ');
    ss = stringstream(p);

    netaddr = 0;
    while (ss >> digit) {
        int d;

        if (string2int(digit, d)) {
            goto ex;
        }

        if (d < 0 || d > 255) {
            goto ex;
        }
        netaddr <<= 8;
        netaddr |= (unsigned)d;
    }

    return;
ex:
    throw "Invalid IP prefix";
}

/* Arguments taken by the function:
 *
 * char *dev: the name of an interface (or '\0'). MUST have enough
 *            space to hold the interface name if '\0' is passed
 * int flags: interface flags (eg, IFF_TUN, IFF_NO_PI, IFF_TAP etc.)
 */
static int
tun_alloc(char *dev, int flags)
{
    struct ifreq ifr;
    int fd, err;

    if ((fd = open("/dev/net/tun", O_RDWR)) < 0) {
        perror("open(/dev/net/tun)");
        return fd;
    }

    memset(&ifr, 0, sizeof(ifr));

    ifr.ifr_flags = flags;   /* IFF_TUN or IFF_TAP, plus maybe IFF_NO_PI */

    if (*dev) {
        /* If a device name was specified, put it in the structure; otherwise,
         * the kernel will try to allocate the "next" device of the
         * specified type */
        strncpy(ifr.ifr_name, dev, IFNAMSIZ);
    }

    /* Try to create the device */
    if ((err = ioctl(fd, TUNSETIFF, (void *) &ifr)) < 0) {
        perror("ioctl(TUNSETIFF)");
        close(fd);
        return err;
    }

    /* Ff the operation was successful, write back the name of the
     * interface to the variable "dev", so the caller can know
     * it. Note that the caller MUST reserve space in *dev (see calling
     * code below) */
    strcpy(dev, ifr.ifr_name);

    /* this is the special file descriptor that the caller will use to talk
     * with the virtual interface */
    return fd;
}

static int
parse_conf(const char *path)
{
    ifstream fin(path);

    if (fin.fail()) {
        cerr << "Cannot open configuration file " << path << endl;
        return -1;
    }

    for (unsigned int lines_cnt = 1; !fin.eof(); lines_cnt++) {
        string line;

        getline(fin, line);

        istringstream iss(line);
        vector<string> tokens;
        string token;

        while (iss >> token) {
            tokens.push_back(token);
        }

        if (tokens.size() <= 0 || tokens[0][0] == '#') {
            /* Ignore comments and white spaces. */
            continue;
        }

        if (tokens[0] == "local") {
            if (tokens.size() != 3) {
                cerr << "Invalid 'local' directive at line " <<
                        lines_cnt << endl;
                return -1;
            }

            g->locals.push_back(Local(tokens[1], tokens[2]));

        } else if (tokens[0] == "remote") {
            IPSubnet subnet;

            if (tokens.size() != 4) {
                cerr << "Invalid 'remote' directive at line " <<
                        lines_cnt << endl;
                return -1;
            }

            try {
                subnet = IPSubnet(tokens[3]);
            } catch (...) {
                cerr << "Invalid IP prefix at line " << lines_cnt << endl;
                return -1;
            }

            g->remotes.push_back(Remote(tokens[1], tokens[2], subnet));

        } else if (tokens[0] == "route") {
            IPSubnet subnet;

            if (tokens.size() != 2) {
                cerr << "Invalid 'route' directive at line " <<
                        lines_cnt << endl;
                return -1;
            }

            try {
                subnet = IPSubnet(tokens[1]);
            } catch (...) {
                cerr << "Invalid IP prefix at line " << lines_cnt << endl;
                return -1;
            }

            g->routes.push_back(Route(subnet));
        }
    }

    fin.close();

    return 0;
}

static void
dump_conf(void)
{
    cout << "Locals:" << endl;
    for (list<Local>::iterator l = g->locals.begin();
                            l != g->locals.end(); l ++) {
        cout << "   " << l->app_name << " in DIF " << l->dif_name << endl;
    }

    cout << "Remotes:" << endl;
    for (list<Remote>::iterator l = g->remotes.begin();
                            l != g->remotes.end(); l ++) {
        cout << "   " << l->app_name << " in DIF " << l->dif_name
            << ", tunnel prefix " << l->tun_subnet.repr << endl;
    }

    cout << "Advertised routes:" << endl;
    for (list<Route>::iterator l = g->routes.begin();
                            l != g->routes.end(); l ++) {
        cout << "   " << l->subnet.repr << endl;
    }
}

static int
remote_tun_alloc(Remote &r)
{
    char tun_name[IFNAMSIZ];

    tun_name[0] = '\0';
    r.tun_fd = tun_alloc(tun_name, IFF_TUN | IFF_NO_PI);
    if (r.tun_fd < 0) {
        cerr << "Failed to create tunnel" << endl;
        return -1;
    }
    r.tun_name = tun_name;
    if (g->verbose) {
        cout << "Created tunnel device " << r.tun_name << endl;
    }

    return 0;
}

static int
setup(void)
{
    g->rfd = rina_open();
    if (g->rfd < 0) {
        perror("rina_open()");
        return -1;
    }

    /* Register us to one or more local DIFs. */
    for (list<Local>::iterator l = g->locals.begin();
                            l != g->locals.end(); l ++) {
        int ret;

        ret = rina_register(g->rfd, l->dif_name.c_str(),
                        l->app_name.c_str(), 0);
        if (ret) {
            perror("rina_register()");
            cerr << "Failed to register " << l->app_name << " in DIF "
                    << l->dif_name << endl;
            return -1;
        }
    }

    /* Create a TUN device for each remote. */
    for (list<Remote>::iterator l = g->remotes.begin();
                            l != g->remotes.end(); l ++) {
        if (remote_tun_alloc(*l)) {
            return -1;
        }
    }

    return 0;
}

/* Try to connect to all the user-specified remotes. */
static void *
connect_to_remotes(void *opaque)
{
    string myname = g->locals.front().app_name;

    for (;;) {
        for (list<Remote>::iterator l = g->remotes.begin();
                            l != g->remotes.end(); l ++) {
            struct rina_flow_spec spec;
            struct pollfd pfd;
            int ret;
            int wfd;

            if (l->rfd >= 0) {
                /* We are already connected to this remote. */
                continue;
            }

            /* Tyr to allocate a reliable flow. */
            rina_flow_spec_default(&spec);
            spec.max_sdu_gap = 0;
            spec.in_order_delivery = 1;
            spec.msg_boundaries = 1;
            spec.spare3 = 1;
            wfd = rina_flow_alloc(l->dif_name.c_str(), myname.c_str(),
                                  l->app_name.c_str(), &spec, RINA_F_NOWAIT);
            if (wfd < 0) {
                perror("rina_flow_alloc()");
                cout << "Failed to connect to remote " << l->app_name <<
                        " through DIF " << l->dif_name << endl;
                continue;
            }
            pfd.fd = wfd;
            pfd.events = POLLIN;
            ret = poll(&pfd, 1, 3000);
            if (ret <= 0) {
                if (ret < 0) {
                    perror("poll(wfd)");
                } else if (g->verbose) {
                    cout << "Failed to connect to remote " << l->app_name <<
                            " through DIF " << l->dif_name << endl;
                }
                close(wfd);
                continue;
            }

            l->rfd = rina_flow_alloc_wait(wfd);
            if (l->rfd < 0) {
                perror("rina_flow_alloc_wait()");
                cout << "Failed to connect to remote " << l->app_name <<
                        " through DIF " << l->dif_name << endl;
                continue;
            }

            if (g->verbose) {
                cout << "Connected to remote " << l->app_name <<
                        " through DIF " << l->dif_name << endl;
            }

            CDAPConn conn(l->rfd, 1);
            CDAPMessage m, *rm = NULL;

            m.m_connect(gpb::AUTH_NONE, NULL, /* src */ myname,
                                              /* dst */ l->app_name);
            if (conn.msg_send(&m, 0)) {
                cerr << "Failed to send M_CONNECT" << endl;
                goto abor;
            }

            rm = conn.msg_recv();
            if (rm->op_code != gpb::M_CONNECT_R) {
                cerr << "M_CONNECT_R expected" << endl;
                goto abor;
            }

            cout << "Connected to remote peer" << endl;

            //m.m_start(gpb::F_NO_FLAGS, "enrollment", "enrollment",
            //          0, 0, string());

abor:
            if (rm) {
                delete rm;
            }
            close(l->rfd);
        }

        sleep(5);
    }

    pthread_exit(NULL);
}

static void
usage(void)
{
    cout << "iporinad [OPTIONS]" << endl <<
        "   -h : show this help" << endl <<
        "   -c CONF_FILE: path to configuration file" << endl;
}

int main(int argc, char **argv)
{
    const char *confpath = "/etc/iporinad.conf";
    struct pollfd pfd[1];
    pthread_t fa_th;
    int opt;

    while ((opt = getopt(argc, argv, "hc:v")) != -1) {
        switch (opt) {
            case 'h':
                usage();
                return 0;

            case 'c':
		confpath = optarg;
                break;

            case 'v':
                g->verbose ++;
                break;

            default:
                printf("    Unrecognized option %c\n", opt);
                usage();
                return -1;
        }
    }

    if (parse_conf(confpath)) {
        return -1;
    }

    if (g->verbose) {
        dump_conf();
    }

    if (setup()) {
        return -1;
    }

    if (pthread_create(&fa_th, NULL, connect_to_remotes, NULL)) {
        perror("pthread_create()");
        return -1;
    }

    /* Wait for incoming control connections. */
    for (;;) {
        Remote r;
        int cfd;
        int ret;

        pfd[0].fd = g->rfd;
        pfd[0].events = POLLIN;
        ret = poll(pfd, 1, -1);
        if (ret < 0) {
            perror("poll(lfd)");
            return -1;
        }

        if (!pfd[0].revents & POLLIN) {
		continue;
	}

        cfd = rina_flow_accept(g->rfd, NULL, NULL, 0);
        if (cfd < 0) {
            if (errno == ENOSPC) {
                continue;
            }
            perror("rina_flow_accept(lfd)");
            return -1;
        }

        cout << "Flow accepted!" << endl;

        r.rfd = cfd;
        if (remote_tun_alloc(r)) {
            close(r.rfd);
            continue;
        }
        g->remotes.push_back(r);

        CDAPConn conn(r.rfd, 1);
        CDAPMessage *rm;
        CDAPMessage m;

        rm = conn.msg_recv();
        if (rm->op_code != gpb::M_CONNECT) {
            cerr << "M_CONNECT expected" << endl;
            goto abor;
        }

        r.app_name = rm->src_appl;
        r.dif_name = string();

        m.m_connect_r(rm, 0, string());
        if (conn.msg_send(&m, rm->invoke_id)) {
            cerr << "Failed to send M_CONNECT_R" << endl;
            goto abor;
        }

abor:
        close(r.rfd);
    }

    pthread_exit(NULL);

    return 0;
}
