#include "avl.h"
#include "constants.h"
#include "hashtable.h"
#include "utils.h"
#include "zset.h"
#include <arpa/inet.h>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <errno.h>
#include <map>
#include <netinet/ip.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

static void do_something(int conn_fd) {
    /*
     * `read()` and `write()` returns the number of read/written bytes
     */
    char read_buf[64]{};
    ssize_t n = read(conn_fd, read_buf, sizeof(read_buf) - 1);

    if (n < 0) {
        msg("read() error");
        return;
    }

    printf("client says: %s\n", read_buf);

    char write_buf[] = "world";
    write(conn_fd, write_buf, strlen(write_buf));
}

static int32_t parse_single_request(int conn_fd) {
    // 4-byte header
    char read_buf[4 + K_MAX_MSG + 1]{};
    errno = 0;
    int32_t err = read_full(conn_fd, read_buf, 4);
    if (err) {
        if (errno == 0) {
            msg("EOF");
        } else {
            msg("read() error");
        }
        return err;
    }

    uint32_t len{};
    memcpy(&len, read_buf, 4); // assume little endian
    if (len > K_MAX_MSG) {
        msg("request too long");
        return -1;
    }

    // request body
    err = read_full(conn_fd, &read_buf[4], len);
    if (err) {
        msg("read() error");
        return err;
    }

    // do work
    read_buf[4 + len] = '\0';
    printf("client says: %s\n", &read_buf[4]);

    // reply using the same protocol
    const char reply[] = "world";
    char write_buf[4 + sizeof(reply)]{};

    len = (uint32_t)strlen(reply);
    memcpy(write_buf, &len, 4);
    memcpy(&write_buf[4], reply, len);

    return write_all(conn_fd, write_buf, 4 + len);
}

enum {
    RES_OK = 0,
    RES_ERR = 1,
    RES_NX = 2,
};

enum {
    STATE_REQ = 0,
    STATE_RES = 1,
    STATE_END = 2, // mark the connection for deletion
};

enum {
    T_STR = 0,
    T_ZSET = 1,
};

struct Conn {
    int fd = -1;
    uint32_t state = STATE_REQ;

    // buffer for reading
    size_t rbuf_size = 0;
    uint8_t rbuf[4 + K_MAX_MSG]{};

    // buffer for writing
    size_t wbuf_size = 0;
    size_t wbuf_sent = 0;
    uint8_t wbuf[4 + K_MAX_MSG]{};
};

struct Entry {
    struct HNode node;
    std::string key;
    std::string val;
    uint32_t type = 0;
    ZSet *zset = nullptr;
};

/*
 * Flushes the write buffer until `EAGAIN` is returned;
 * or transits back to `STATE_REQ` if the flushing is done
 */
static bool try_flush_buffer(Conn *conn) {
    ssize_t rv{};
    do {
        size_t remaining = conn->wbuf_size - conn->wbuf_sent;
        rv = write(conn->fd, &conn->wbuf[conn->wbuf_sent], remaining);
    } while (rv < 0 && errno == EINTR);

    if (rv < 0 && errno == EAGAIN) {
        return false;
    }

    if (rv < 0) {
        msg("write() error");
        conn->state = STATE_END;
        return false;
    }

    conn->wbuf_sent += (size_t)rv;
    assert(conn->wbuf_sent <= conn->wbuf_size);
    if (conn->wbuf_sent == conn->wbuf_size) {
        // response fully sent, change state back
        conn->state = STATE_REQ;
        conn->wbuf_sent = 0;
        conn->wbuf_size = 0;
        return false;
    }

    // still some data in wbuf;
    // could try to write again
    return true;
}

static void state_res(Conn *conn) {
    while (try_flush_buffer(conn)) {
    }
}

static std::map<std::string, std::string> g_map{};
static struct { HMap db; } g_data;

static bool entry_eq(HNode *lhs, HNode *rhs) {
    struct Entry *le = container_of(lhs, struct Entry, node);
    struct Entry *re = container_of(rhs, struct Entry, node);
    return lhs->hcode == rhs->hcode && le->key == re->key;
}

// static uint32_t do_get(const std::vector<std::string> &cmd, uint8_t *res,
//                        uint32_t *reslen) {
//     if (!g_map.count(cmd[1])) {
//         return RES_NX;
//     }
//
//     std::string &val = g_map[cmd[1]];
//     assert(val.size() <= K_MAX_MSG);
//     memcpy(res, val.data(), val.size());
//     *reslen = (uint32_t)val.size();
//     return RES_OK;
// }

static void do_get(std::vector<std::string> &cmd, std::string &out) {
    Entry entry;
    entry.key.swap(cmd[1]); // set cmd[1] to be the key in entry
    entry.node.hcode = str_hash((uint8_t *)entry.key.data(), entry.key.size());

    HNode *node = hm_lookup(&g_data.db, &entry.node, &entry_eq);
    if (!node) {
        return out_nil(out);
    }

    const std::string &val = container_of(node, Entry, node)->val;
    assert(val.size() <= K_MAX_MSG);

    out_str(out, val);
}

/* static uint32_t do_set(const std::vector<std::string> &cmd, uint8_t *res,
                       uint32_t *reslen) {
    (void)res;
    (void)reslen;
    g_map[cmd[1]] = cmd[2];
    return RES_OK;
} */
static void do_set(std::vector<std::string> &cmd, std::string &out) {
    Entry entry;
    entry.key.swap(cmd[1]);
    entry.node.hcode = str_hash((uint8_t *)entry.key.data(), entry.key.size());

    HNode *node = hm_lookup(&g_data.db, &entry.node, &entry_eq);
    if (node) {
        // node already exists
        container_of(node, Entry, node)->val.swap(cmd[2]);
    } else {
        Entry *new_entry = new Entry();
        new_entry->key.swap(entry.key);
        new_entry->node.hcode = entry.node.hcode;
        new_entry->val.swap(cmd[2]);
        hm_insert(&g_data.db, &new_entry->node);
    }

    return out_nil(out);
}

/* static uint32_t do_del(const std::vector<std::string> &cmd, uint8_t *res,
                       uint32_t *reslen) {
    (void)res;
    (void)reslen;
    g_map.erase(cmd[1]);
    return RES_OK;
} */

static void entry_del(Entry *entry) {
    switch (entry->type) {
    case T_ZSET:
        zset_dispose(entry->zset);
        delete entry->zset;
        break;
    }
    delete entry;
}

static void do_del(std::vector<std::string> &cmd, std::string &out) {
    Entry entry;
    entry.key.swap(cmd[1]);
    entry.node.hcode = str_hash((uint8_t *)entry.key.data(), entry.key.size());

    HNode *node = hm_pop(&g_data.db, &entry.node, &entry_eq);
    if (node) {
        entry_del(container_of(node, Entry, node));
    }
    return out_int(out, node ? 1 : 0);
}

static int32_t parse_req(const uint8_t *data, size_t len,
                         std::vector<std::string> &out) {
    if (len < 4) {
        return -1;
    }

    uint32_t n{};
    memcpy(&n, &data[0], 4); // number of the commands - `nstr`
    if (n > K_MAX_ARGS) {
        return -1;
    }

    size_t pos = 4;
    while (n--) {
        if (pos + 4 > len) {
            return -1;
        }
        uint32_t sz{}; // size of the actual command string
        memcpy(&sz, &data[pos], 4);
        if (pos + 4 + sz > len) {
            return -1;
        }
        out.push_back(std::string((char *)&data[pos + 4], sz));
        pos += 4 + sz;
    }

    if (pos != len) {
        return -1; // trailing garbage
    }

    return 0;
}

void cb_scan(HNode *node, void *arg) {
    std::string &out = *(std::string *)arg;
    out_str(out, container_of(node, Entry, node)->key);
}

static bool str2double(const std::string &s, double &out) {
    char *endp = nullptr;
    out = strtod(s.c_str(), &endp);
    return endp == s.c_str() + s.size() && !std::isnan(out);
}

static bool str2int(const std::string &s, int64_t &out) {
    char *endp = nullptr;
    out = strtoll(s.c_str(), &endp, 10);
    return endp == s.c_str() + s.size();
}

static void do_keys(std::vector<std::string> &cmd, std::string &out) {
    (void)cmd;
    out_arr(out, (uint32_t)hm_size(&g_data.db));
    h_scan(&g_data.db.ht_to, &cb_scan, &out);
    h_scan(&g_data.db.ht_from, &cb_scan, &out);
}

/**
 * command: `zadd zset <score> <string>`
 */
static void do_zadd(std::vector<std::string> &cmd, std::string &out) {
    double score = 0;
    if (!str2double(cmd[2], score)) {
        return out_err(out, ERR_ARG, "expected fp number");
    }

    // lookup or create the zset
    Entry entry;
    entry.key.swap(cmd[1]);
    entry.node.hcode = str_hash((uint8_t *)entry.key.data(), entry.key.size());
    HNode *hnode = hm_lookup(&g_data.db, &entry.node, &entry_eq);

    Entry *ent = nullptr;
    if (!hnode) {
        ent = new Entry();
        ent->key.swap(entry.key);
        ent->node.hcode = entry.node.hcode;
        ent->type = T_ZSET;
        ent->zset = new ZSet();
        hm_insert(&g_data.db, &ent->node);
    } else {
        ent = container_of(hnode, Entry, node);
        if (ent->type != T_ZSET) {
            return out_err(out, ERR_TYPE, "expecting zset");
        }
    }

    // add/update the tuple
    const std::string &name = cmd[3];
    bool added = zset_add(ent->zset, name.data(), name.size(), score);

    return out_int(out, (int64_t)added);
}

static bool expect_zset(std::string &out, std::string &s, Entry **ent) {
    Entry entry;
    entry.key.swap(s);
    entry.node.hcode = str_hash((uint8_t *)entry.key.data(), entry.key.size());
    HNode *hnode = hm_lookup(&g_data.db, &entry.node, &entry_eq);

    if (!hnode) {
        out_nil(out);
        return false;
    }

    *ent = container_of(hnode, Entry, node);
    if ((*ent)->type != T_ZSET) {
        out_err(out, ERR_TYPE, "expecting zset");
        return false;
    }

    return true;
}

/**
 * command: `zrem zset <name>`
 * remove <name> from zset
 */
static void do_zrem(std::vector<std::string> &cmd, std::string &out) {
    Entry *ent = nullptr;

    // if removing a non-zset, do nothing and return
    if (!expect_zset(out, cmd[1], &ent)) {
        return;
    }

    const std::string &name = cmd[2];
    ZNode *znode = zset_pop(ent->zset, name.data(), name.size());
    if (znode) {
        znode_del(znode);
    }

    return out_int(out, znode ? 1 : 0);
}

/**
 * command: `zscore zset <name>`
 * Get the score of <name>
 */
static void do_zscore(std::vector<std::string> &cmd, std::string &out) {
    Entry *ent = nullptr;
    if (!expect_zset(out, cmd[1], &ent)) {
        return;
    }

    const std::string &name = cmd[2];
    ZNode *znode = zset_lookup(ent->zset, name.data(), name.size());
    return znode ? out_double(out, znode->score) : out_nil(out);
}

/**
 * command: `zquery zset <score> <name> <offset> <limit>`
 */
static void do_zquery(std::vector<std::string> &cmd, std::string &out) {
    // parse args
    double score = 0;
    if (!str2double(cmd[2], score)) {
        return out_err(out, ERR_ARG, "expecting fp number");
    }

    const std::string &name = cmd[3];
    int64_t offset = 0;
    int64_t limit = 0;
    if (!str2int(cmd[4], offset)) {
        return out_err(out, ERR_ARG, "expecting int");
    }
    if (!str2int(cmd[5], limit)) {
        return out_err(out, ERR_ARG, "expecting int");
    }

    // get the zset
    Entry *ent = nullptr;
    if (!expect_zset(out, cmd[1], &ent)) {
        if (out[0] == SER_NIL) {
            out.clear();
            out_arr(out, 0);
        }
        return;
    }

    // lookup the tuple
    if (limit <= 0) {
        return out_arr(out, 0);
    }

    ZNode *znode =
        zset_query(ent->zset, score, name.data(), name.size(), offset);

    // output
    out_arr(out, 0);
    uint32_t n = 0;
    while (znode && (int64_t)n < limit) {
        out_str(out, znode->name, znode->len);
        out_double(out, znode->score);
        znode = container_of(avl_offset(&znode->tree, +1), ZNode, tree);
        n += 2; // why += 2?
    }

    return out_update_arr(out, n);
}

/* static int32_t do_request(const uint8_t *req, uint32_t reqlen,
                          uint32_t *rescode, uint8_t *res, uint32_t *reslen) {
    std::vector<std::string> cmd; // in header <string>, _NOT_ <string.h>
    if (0 != parse_req(req, reqlen, cmd)) {
        msg("bad req");
        return -1;
    }

    if (cmd.size() == 2 && cmd_is(cmd[0], "get")) {
        *rescode = do_get(cmd, res, reslen);
    } else if (cmd.size() == 3 && cmd_is(cmd[0], "set")) {
        *rescode = do_set(cmd, res, reslen);
    } else if (cmd.size() == 2 && cmd_is(cmd[0], "del")) {
        *rescode = do_del(cmd, res, reslen);
    } else {
        // cmd not recognized
        *rescode = RES_ERR;
        const char msg[] = "Unknown cmd";
        strncpy((char *)res, msg, strlen(msg));
        *reslen = strlen(msg);
        return 0;
    }
    return 0;
} */
static void do_request(std::vector<std::string> &cmd, std::string &out) {
    if (cmd.size() == 1 && cmd_is(cmd[0], "keys")) {
        do_keys(cmd, out);
    } else if (cmd.size() == 2 && cmd_is(cmd[0], "get")) {
        do_get(cmd, out);
    } else if (cmd.size() == 3 && cmd_is(cmd[0], "set")) {
        do_set(cmd, out);
    } else if (cmd.size() == 2 && cmd_is(cmd[0], "del")) {
        do_del(cmd, out);
    } else if (cmd.size() == 4 && cmd_is(cmd[0], "zadd")) {
        do_zadd(cmd, out);
    } else if (cmd.size() == 3 && cmd_is(cmd[0], "zrem")) {
        do_zrem(cmd, out);
    } else if (cmd.size() == 3 && cmd_is(cmd[0], "zscore")) {
        do_zscore(cmd, out);
    } else if (cmd.size() == 6 && cmd_is(cmd[0], "zquery")) {
        do_zquery(cmd, out);
    } else {
        // cmd not recognized
        out_err(out, ERR_UNKNOWN, "Unknown cmd");
    }
}

static bool try_one_request(Conn *conn) {
    // try to parse a request from buffer
    if (conn->rbuf_size < 4) {
        // not enough data in buffer
        // retry in the next iteration
        return false;
    }

    uint32_t len{};
    memcpy(&len, &conn->rbuf[0], 4);
    if (len > K_MAX_MSG) {
        msg("too long");
        conn->state = STATE_END;
        return false;
    }

    if (4 + len > conn->rbuf_size) {
        // not enough data in buffer
        return false;
    }

    printf("client says: %.*s\n", len,
           &conn->rbuf[4]); // `.*` specifies precision

    // parse the request
    std::vector<std::string> cmd;
    if (0 != parse_req(&conn->rbuf[4], len, cmd)) {
        msg("bad req");
        conn->state = STATE_END;
        return false;
    }

    // received one request,
    // generate the response
    std::string out;
    do_request(cmd, out);

    // pack response into the buffer
    if (4 + out.size() > K_MAX_MSG) {
        out.clear();
        out_err(out, ERR_2BIG, "response is too big");
    }
    // uint32_t rescode{};
    uint32_t wlen = (uint32_t)out.size();
    /* // 4 + 4 because: len + status code
    int32_t err =
        do_request(&conn->rbuf[4], len, &rescode, &conn->wbuf[4 + 4], &wlen);

    if (err) {
        conn->state = STATE_END;
        return false;
    }
    wlen += 4; */
    memcpy(&conn->wbuf[0], &wlen, 4);
    memcpy(&conn->wbuf[4], out.data(), out.size());
    conn->wbuf_size = 4 + wlen; // en echo

    // remove the request from buffer
    size_t remaining = conn->rbuf_size - 4 - len;
    if (remaining) {
        memmove(conn->rbuf, &conn->rbuf[4 + len], remaining);
    }
    conn->rbuf_size = remaining;

    // change state
    conn->state = STATE_RES;
    state_res(conn);

    // continue the outer loop (in its caller) if the request was fully
    // processed
    return (conn->state == STATE_REQ);
}

/*
 * process data immediately after reading,
 * to clear some read buffer space
 * then `try_fill_buffer()` is looped until `EAGAIN` is hit
 */
static bool try_fill_buffer(Conn *conn) {
    // try to fill the buffer
    assert(conn->rbuf_size < sizeof(conn->rbuf));
    ssize_t rv = 0;

    // fill `rbuf`
    do {
        size_t cap = sizeof(conn->rbuf) - conn->rbuf_size;
        rv = read(conn->fd, &conn->rbuf[conn->rbuf_size], cap);
        // retrying
        // EINTR: syscall was interrupted by a signal
    } while (rv < 0 && errno == EINTR);

    if (rv < 0 && errno == EAGAIN) {
        return false;
    }

    if (rv < 0) {
        msg("read() error");
        conn->state = STATE_END;
        return false;
    }

    if (rv == 0) {
        if (conn->rbuf_size > 0) {
            msg("unexpected EOF"); // ???
        } else {
            msg("EOF");
        }

        conn->state = STATE_END;
        return false;
    }

    conn->rbuf_size += (size_t)rv;
    assert(conn->rbuf_size <= sizeof(conn->rbuf));

    // try to process requests one by one
    while (try_one_request(conn)) {
    }

    return (conn->state == STATE_REQ);
}

static void state_req(Conn *conn) {
    while (try_fill_buffer(conn)) {
    }
}

/*
 * accepts a new connection and creates a `struct Conn` object
 */
static void conn_put(std::vector<Conn *> &fd2conn, struct Conn *conn) {
    if (fd2conn.size() <= (size_t)conn->fd) {
        fd2conn.resize(conn->fd + 1);
    }
    fd2conn[conn->fd] = conn;
}

static int32_t accept_new_conn(std::vector<Conn *> &fd2conn, int fd) {
    // accept
    struct sockaddr_in client_addr {};
    socklen_t socklen = sizeof(client_addr);
    int conn_fd = accept(fd, (struct sockaddr *)&client_addr, &socklen);
    if (conn_fd < 0) {
        msg("accept() error");
        return -1;
    }

    // set the new connection fd to nonblocking mode
    fd_set_nb(conn_fd);
    struct Conn *conn = (struct Conn *)malloc(sizeof(struct Conn));
    if (!conn) {
        close(conn_fd);
        return -1;
    }

    conn->fd = conn_fd;
    conn->state = STATE_REQ;
    conn->rbuf_size = 0;
    conn->wbuf_size = 0;
    conn->wbuf_sent = 0;
    conn_put(fd2conn, conn);
    return 0;
}

/*
 * state machine for client connections
 */
static void connection_io(Conn *conn) {
    if (conn->state == STATE_REQ) {
        state_req(conn);
    } else if (conn->state == STATE_RES) {
        state_res(conn);
    } else {
        assert(0); // not expected
    }
}

// AF_INET - IPv4
// AF_INET6 - IPv6 or dual-stack socket
// SOCK_STREAM - for TCP
int main() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    int val = 1;
    // configure socket
    // SO_REUSEADDR - bind to the same address if restarted
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));

    // bind
    struct sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = ntohs(1234);
    addr.sin_addr.s_addr = ntohl(0); // 0.0.0.0
    int rv = bind(fd, (const struct sockaddr *)&addr, sizeof(addr));
    if (rv) {
        die("bind()");
    }

    // listen
    rv = listen(fd, SOMAXCONN);
    if (rv) {
        die("listen()");
    }

    // map of all client connections , keyed by fd
    std::vector<Conn *> fd2conn{};
    // set the listen fd to non-blocking
    fd_set_nb(fd);

    // the Event Loop
    /*
     * struct pollfd {
     *     int fd; // socket descriptor
     *     short events; // bitmap of events of interest
     *     short revents; // when poll() returns, bitmap of events that occurred
     * }
     */
    /*
     * POLLIN - alert when data is ready to `recv()` on this socket
     * POLLOUT - alert when data is ready to `send()` _to_ this socket _without
     * blocking_
     */
    std::vector<struct pollfd> poll_args{};
    while (true) {
        // prepare the arguments of the poll()
        poll_args.clear();
        // listening fd
        struct pollfd pfd {
            fd, POLLIN, 0
        };
        poll_args.push_back(pfd);
        // connection fds
        for (Conn *conn : fd2conn) {
            if (!conn) {
                continue;
            }

            struct pollfd pfd {};
            pfd.fd = conn->fd;
            pfd.events = (conn->state == STATE_REQ) ? POLLIN : POLLOUT;
            pfd.events = pfd.events | POLLERR;
            poll_args.push_back(pfd);
        }

        // poll for active fds
        int rv = poll(poll_args.data(), (nfds_t)poll_args.size(), 1000);
        if (rv < 0) {
            die("poll");
        }

        // process active connections
        for (size_t i = 1; i < poll_args.size(); ++i) {
            if (poll_args[i].revents) {
                Conn *conn = fd2conn[poll_args[i].fd];
                connection_io(conn);

                if (conn->state == STATE_END) {
                    // client closed normally, or something BAD happened
                    // destroy the connection
                    fd2conn[conn->fd] = nullptr;
                    (void)close(conn->fd);
                    free(conn);
                }
            }
        }

        // try to accept a new connection if the listening fd is active
        if (poll_args[0].revents) {
            (void)accept_new_conn(fd2conn, fd);
        }

        /*
        // accept
        struct sockaddr_in client_addr {};
        socklen_t socklen = sizeof(client_addr);
        int conn_fd = accept(fd, (struct sockaddr *)&client_addr, &socklen);
        if (conn_fd < 0) {
            continue; // error
        }

        while (true) {
            int32_t err = parse_single_request(conn_fd);
            if (err) {
                break;
            }
        }

        close(conn_fd);
        */
    }
    return 0;
}
