#include "rocksdb/remote_flush_service.h"

#include <alloca.h>
#include <arpa/inet.h>
#include <byteswap.h>
#include <endian.h>
#include <getopt.h>
#include <infiniband/verbs.h>
#include <inttypes.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <sys/poll.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <array>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <ratio>
#include <thread>
#include <utility>

#include "db/tcprw.h"
#include "memory/remote_memtable_service.h"
#include "rocksdb/logger.hpp"
#include "rocksdb/macro.hpp"

#define MAX_POLL_CQ_TIMEOUT 2000
#if __BYTE_ORDER == __LITTLE_ENDIAN
static inline uint64_t htonll(uint64_t x) { return bswap_64(x); }
static inline uint64_t ntohll(uint64_t x) { return bswap_64(x); }
#elif __BYTE_ORDER == __BIG_ENDIAN
static inline uint64_t htonll(uint64_t x) { return x; }
static inline uint64_t ntohll(uint64_t x) { return x; }
#else
#error __BYTE_ORDER is neither __LITTLE_ENDIAN nor __BIG_ENDIAN
#endif

namespace ROCKSDB_NAMESPACE {

bool TCPNode::send(const void *buf, size_t size) {
  ASSERT_RW(writen(connection_info_.client_sockfd, &size, sizeof(size)) ==
            sizeof(size));
  ASSERT_RW(writen(connection_info_.client_sockfd, buf, size) == size);
  return true;
}

// recv(buf!=nullptr,size!=0) => receive size bytes data to specific address
// recv(buf==nullptr,size==0) => receive n bytes data to new allocated address
bool TCPNode::receive(void **buf, size_t *size) {
  char *buf_ = reinterpret_cast<char *>(*buf);
  size_t package_size = 0;
  ASSERT_RW(readn(connection_info_.client_sockfd, &package_size,
                  sizeof(package_size)) == sizeof(package_size));

  if (*size == 0)
    *size = package_size;
  else
    assert(package_size == *size);

  if (buf_ == nullptr) {
    buf_ = reinterpret_cast<char *>(memory_.allocate(package_size));
    *buf = buf_;
  }
  ASSERT_RW(readn(connection_info_.client_sockfd, buf_, package_size) ==
            package_size);
  return true;
}

bool RemoteFlushJobPD::closetcp() {
  if (server_info_.tcp_server_sockfd_ == -1) {
    LOG("tcp server not opened");
    return false;
  }
  assert(false);
  return true;
}

bool RemoteFlushJobPD::opentcp(int port, int heartbeat_port) {
  std::thread listen_thread{
      [this, heartbeat_port]() { pd_.poll_events(heartbeat_port); }};
  listen_thread.detach();
  if (server_info_.tcp_server_sockfd_ != -1) {
    LOG("tcp server already opened");
    return false;
  }
  int opt = ~SOCK_NONBLOCK;  // debug
  server_info_.tcp_server_sockfd_ = socket(AF_INET, SOCK_STREAM, 0);
  assert(server_info_.tcp_server_sockfd_ != -1);
  setsockopt(server_info_.tcp_server_sockfd_, SOL_SOCKET, SO_REUSEADDR, &opt,
             sizeof(opt));
  server_info_.server_address.sin_family = AF_INET;
  server_info_.server_address.sin_addr.s_addr = htonl(INADDR_ANY);
  server_info_.server_address.sin_port = htons(port);
  assert(bind(server_info_.tcp_server_sockfd_,
              (struct sockaddr *)&server_info_.server_address,
              sizeof(server_info_.server_address)) >= 0);
  assert(listen(server_info_.tcp_server_sockfd_, 10) >= 0);
  while (true) {
    struct sockaddr_in client_address;
    socklen_t client_address_len = sizeof(client_address);
    int client_sockfd =
        accept(server_info_.tcp_server_sockfd_,
               (struct sockaddr *)&client_address, &client_address_len);
    if (client_sockfd < 0) {
      LOG("tcp server accept error");
      continue;
    }
    {
      char client_ip[INET_ADDRSTRLEN];
      inet_ntop(AF_INET, &client_address.sin_addr, client_ip, INET_ADDRSTRLEN);
      int client_port = ntohs(client_address.sin_port);
      LOG_CERR("MemNode receive package from: ", client_ip, ':', client_port);
    }
    auto *node = new TCPNode(client_address, client_sockfd);
    register_flush_job_generator(client_sockfd, node);
    LOG("tcp server accept success");
    std::thread([client_sockfd, node, this]() {
      // BGworkRemoteFlush
      LOG("remote flush job generator connected. start receiving.");
      flushjob_package *package = receive_remote_flush_job(node);
      LOG("remote flush job received from generator.");
      TCPNode *worker_tcpnode = nullptr;
      while (worker_tcpnode == nullptr) {
        worker_tcpnode = choose_flush_job_executor();
      }
      assert(worker_tcpnode != nullptr);
      LOG("remote flush job executor chosen.");
      send_remote_flush_job(package, worker_tcpnode);
      LOG("remote flush job sent to worker.");
      setfree_flush_job_executor(worker_tcpnode);
      LOG("remote flush job executor set free.");
      TCPNode *generator = unregister_flush_job_generator(client_sockfd);
      LOG("remote flush job generator unregistered.");
      delete generator;
      delete package;
    }).detach();
  }
  close(server_info_.tcp_server_sockfd_);
  return true;
}

RemoteFlushJobPD::flushjob_package *RemoteFlushJobPD::receive_remote_flush_job(
    TCPNode *generator_node) {
  auto *package = new flushjob_package();
  const char *bye = "byebyemessage";
  while (true) {
    void *buf_ = nullptr;
    size_t size = 0;
    generator_node->receive(&buf_, &size);
    assert(buf_ != nullptr);
    if (size == 0) assert(false);
    LOG("memnode recv data from generator:", size);
    if (size == strlen(bye) &&
        strncmp(reinterpret_cast<char *>(buf_), bye, size) == 0) {
      break;
    }
    package->package.push_back(std::make_pair(buf_, size));
  }
  LOG("receive_remote_flush_job: receive bye message");
  return package;
}

void RemoteFlushJobPD::send_remote_flush_job(flushjob_package *package,
                                             TCPNode *worker_node) {
  for (auto &it : package->package) {
    worker_node->send(it.first, it.second);
  }
  worker_node->send("byebyemessage", strlen("byebyemessage"));
}

void RemoteFlushJobPD::setfree_flush_job_executor(TCPNode *worker_node) {
  std::lock_guard<std::mutex> lock(mtx_);
  flush_job_executors_status_.at(worker_node) = true;
  flush_job_executors_in_use_.erase(
      worker_node->connection_info_.client_sockfd);
  worker_node->connection_info_.client_sockfd = {};
}
TCPNode *RemoteFlushJobPD::choose_flush_job_executor() {
  std::lock_guard<std::mutex> lock(mtx_);
  if (!pd_.available_workers_.empty()) {
    TCPNode *choose_by_policy = nullptr;
    choose_by_policy = pd_.available_workers_.front();
    pd_.available_workers_.pop();
    int client_sockfd = socket(AF_INET, SOCK_STREAM, 0);
    assert(client_sockfd != -1);
    char choose_client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &choose_by_policy->connection_info_.sin_addr.sin_addr,
              choose_client_ip, INET_ADDRSTRLEN);

    // find TCPNode by ip
    for (auto &it : flush_job_executors_status_) {
      char client_ip[INET_ADDRSTRLEN];
      inet_ntop(AF_INET, &it.first->connection_info_.sin_addr.sin_addr,
                client_ip, INET_ADDRSTRLEN);
      if (strcmp(client_ip, choose_client_ip) == 0 && it.second == true) {
        if (connect(client_sockfd,
                    reinterpret_cast<struct sockaddr *>(
                        &it.first->connection_info_.sin_addr),
                    sizeof(it.first->connection_info_.sin_addr)) < 0) {
          LOG("remote flushjob worker connect error");
          close(client_sockfd);
          continue;
        }
        it.second = false;
        it.first->connection_info_.client_sockfd = client_sockfd;
        flush_job_executors_in_use_.insert(
            std::make_pair(client_sockfd, it.first));
        return it.first;
      }
    }
    // not found
    printf("chosen worker not found, fallback to default\n");
  }

  int client_sockfd = socket(AF_INET, SOCK_STREAM, 0);
  assert(client_sockfd != -1);
  for (auto &it : flush_job_executors_status_) {
    if (it.second) {
      if (connect(client_sockfd,
                  reinterpret_cast<struct sockaddr *>(
                      &it.first->connection_info_.sin_addr),
                  sizeof(it.first->connection_info_.sin_addr)) < 0) {
        LOG("remote flushjob worker connect error");
        close(client_sockfd);
        return nullptr;
      }
      {
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &it.first->connection_info_.sin_addr.sin_addr,
                  client_ip, INET_ADDRSTRLEN);
        int client_port = ntohs(it.first->connection_info_.sin_addr.sin_port);
        LOG_CERR("MemNode send package to worker: ", client_ip, ':',
                 client_port);
      }
      it.second = false;
      it.first->connection_info_.client_sockfd = client_sockfd;
      flush_job_executors_in_use_.insert(
          std::make_pair(client_sockfd, it.first));
      return it.first;
    }
  }
  LOG("no available worker");
  close(client_sockfd);
  return nullptr;
}

RDMANode::RDMANode() {
  config = (config_t){"",  // dev_name
                      1,   // ib_port
                      -1,  // gid_idx
                      100, 100, 100};
  res = new resources();
  conns_mtx = std::make_unique<std::mutex>();
  executor_table_mtx = std::make_unique<std::mutex>();
}

RDMANode::~RDMANode() {
  resources_destroy();
  conns_mtx.reset(nullptr);
  executor_table_mtx.reset(nullptr);
  delete res;
}

struct RDMANode::rdma_connection *RDMANode::sock_connect(
    const std::string &server_name, u_int32_t tcp_port) {
  const char *servername = server_name != "" ? server_name.c_str() : nullptr;
  int port = tcp_port;
  struct addrinfo *resolved_addr = nullptr;
  struct addrinfo *iterator;
  char service[6];
  int sockfd = -1;
  int listenfd = 0;
  int tmp;
  std::vector<struct rdma_connection *> successful_conn;
  struct addrinfo hints = {
      .ai_flags = AI_PASSIVE, .ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
  if (sprintf(service, "%d", port) < 0) goto sock_connect_exit;
  // Resolve DNS address, use sockfd as temp storage
  sockfd = getaddrinfo(servername, service, &hints, &resolved_addr);
  if (sockfd < 0) {
    fprintf(stderr, "%s for %s:%d\n", gai_strerror(sockfd), servername, port);
    goto sock_connect_exit;
  }
  // Search through results and find the one we want
  for (iterator = resolved_addr; iterator; iterator = iterator->ai_next) {
    sockfd = socket(iterator->ai_family, iterator->ai_socktype,
                    iterator->ai_protocol);
    if (sockfd >= 0) {
      if (servername) {
        // Client mode. Initiate connection to remote
        if ((tmp = connect(sockfd, iterator->ai_addr, iterator->ai_addrlen))) {
          // LOG("failed connect \n");
          close(sockfd);
          sockfd = -1;
        }
      } else {
        // Server mode. Set up listening socket an accept a connection
        listenfd = sockfd;
        sockfd = -1;
        if (bind(listenfd, iterator->ai_addr, iterator->ai_addrlen))
          goto sock_connect_exit;
        listen(listenfd, 5);
        while (true) {
          struct sockaddr_in client_address;
          socklen_t client_address_len = sizeof(client_address);
          sockfd = accept(listenfd, (struct sockaddr *)&client_address,
                          &client_address_len);
          if (sockfd >= 0) {
            auto conn = connect_qp(sockfd);
            if (conn) {
              conn->addr = client_address;
              successful_conn.push_back(conn);
            }
          }
        }
      }
    }
  }
sock_connect_exit:
  if (listenfd) close(listenfd);
  if (resolved_addr) freeaddrinfo(resolved_addr);
  if (sockfd < 0) {
    if (servername) {
      fprintf(stderr, "Couldn't connect to %s:%d\n", servername, port);
      return nullptr;
    } else {
      perror("server accept");
      fprintf(stderr, "accept() failed\n");
      return nullptr;
    }
  } else {
    if (servername) {
      auto conn = connect_qp(sockfd);
      return conn;
    } else {
      fprintf(stderr, "server mode quit normally");
      return nullptr;
    }
  }
}

int RDMANode::sock_sync_data(int sock, int xfer_size, const char *local_data,
                             char *remote_data) {
  int rc = 0;
  int read_bytes = 0;
  int total_read_bytes = 0;
  rc = write(sock, local_data, xfer_size);
  if (rc < xfer_size)
    fprintf(stderr, "Failed writing data during sock_sync_data\n");
  else
    rc = 0;
  while (!rc && total_read_bytes < xfer_size) {
    read_bytes = read(sock, remote_data + total_read_bytes,
                      xfer_size - total_read_bytes);

    if (read_bytes > 0)
      total_read_bytes += read_bytes;
    else
      rc = read_bytes;
  }
  return rc;
}

int RDMANode::poll_completion(struct rdma_connection *conn) {
  struct ibv_wc wc;
  unsigned long start_time_msec;
  unsigned long cur_time_msec;
  struct timeval cur_time;
  int poll_result;
  int rc = 0;
  // poll the completion for a while before giving up of doing it ..
  gettimeofday(&cur_time, NULL);
  start_time_msec = (cur_time.tv_sec * 1000) + (cur_time.tv_usec / 1000);
  do {
    poll_result = ibv_poll_cq(conn->cq, 1, &wc);
    gettimeofday(&cur_time, NULL);
    cur_time_msec = (cur_time.tv_sec * 1000) + (cur_time.tv_usec / 1000);
  } while ((poll_result == 0) &&
           ((cur_time_msec - start_time_msec) < MAX_POLL_CQ_TIMEOUT));
  if (poll_result < 0) {
    // poll CQ failed
    fprintf(stderr, "poll CQ failed\n");
    rc = 1;
  } else if (poll_result == 0) {  // the CQ is empty
    fprintf(stderr, "completion wasn't found in the CQ after timeout\n");
    // std::this_thread::sleep_for(std::chrono::milliseconds(100));
    rc = 1;
  } else {
    // CQE found
    // LOG("completion was found in CQ with status 0x%x\n", wc.status);
    // check the completion status (here we don't care about the completion
    // opcode
    if (wc.status != IBV_WC_SUCCESS) {
      fprintf(stderr,
              "got bad completion with status: 0x%x, vendor syndrome: 0x%x\n",
              wc.status, wc.vendor_err);
      rc = 1;
    }
  }
  return rc;
}
int RDMANode::post_send(struct rdma_connection *conn, size_t msg_size,
                        ibv_wr_opcode opcode, long long local_offset,
                        long long remote_offset) {
  struct ibv_send_wr sr;
  struct ibv_sge sge;
  struct ibv_send_wr *bad_wr = nullptr;
  int rc = 0;
  // prepare the scatter/gather entry
  memset(&sge, 0, sizeof(sge));
  sge.addr = (uintptr_t)res->buf + local_offset;
  sge.length = msg_size;
  sge.lkey = res->mr->lkey;
  // prepare the send work request
  memset(&sr, 0, sizeof(sr));
  sr.next = nullptr;
  sr.wr_id = 0;
  sr.sg_list = &sge;
  sr.num_sge = 1;
  sr.opcode = opcode;
  sr.send_flags = IBV_SEND_SIGNALED;
  if (opcode != IBV_WR_SEND) {
    sr.wr.rdma.remote_addr = res->remote_props.addr + remote_offset;
    sr.wr.rdma.rkey = res->remote_props.rkey;
  }
  // there is a Receive Request in the responder side, so we won't get any into
  // RNR flow
  rc = ibv_post_send(conn->qp, &sr, &bad_wr);
  if (rc)
    fprintf(stderr, "failed to post SR\n");
  else {
    switch (opcode) {
      case IBV_WR_SEND:
        // LOG("Send Request was posted\n");
        break;
      case IBV_WR_RDMA_READ:
        // LOG("RDMA Read Request was posted\n");
        break;
      case IBV_WR_RDMA_WRITE:
        // LOG("RDMA Write Request was posted\n");
        break;
      default:
        // LOG("Unknown Request was posted\n");
        break;
    }
  }
  return rc;
}

int RDMANode::post_receive(struct rdma_connection *conn, size_t msg_size,
                           long long local_offset) {
  struct ibv_recv_wr rr;
  struct ibv_sge sge;
  struct ibv_recv_wr *bad_wr;
  int rc = 0;
  // prepare the scatter/gather entry
  memset(&sge, 0, sizeof(sge));
  sge.addr = (uintptr_t)res->buf + local_offset;
  sge.length = msg_size;
  sge.lkey = res->mr->lkey;
  // prepare the receive work request
  memset(&rr, 0, sizeof(rr));
  rr.next = nullptr;
  rr.wr_id = 0;
  rr.sg_list = &sge;
  rr.num_sge = 1;
  // post the Receive Request to the RQ
  rc = ibv_post_recv(conn->qp, &rr, &bad_wr);
  if (rc) fprintf(stderr, "failed to post RR\n");
  // else
  // 	LOG("Receive Request was posted\n");
  return rc;
}

int RDMANode::resources_create(size_t size) {
  struct ibv_device **dev_list = nullptr;
  struct ibv_device *ib_dev = nullptr;
  int i;
  int mr_flags = 0;
  int num_devices;
  int rc = 0;
  // LOG("TCP connection was established\n");
  // LOG("searching for IB devices in host\n");
  // get device names in the system
  dev_list = ibv_get_device_list(&num_devices);
  if (!dev_list) {
    fprintf(stderr, "failed to get IB devices list\n");
    rc = 1;
    goto resources_create_exit;
  }
  // if there isn't any IB device in host
  if (!num_devices) {
    fprintf(stderr, "found %d device(s)\n", num_devices);
    rc = 1;
    goto resources_create_exit;
  }
  // LOG("found %d device(s)\n", num_devices);
  // search for the specific device we want to work with
  for (i = 0; i < num_devices; i++) {
    if (config.dev_name == "") {
      config.dev_name = std::string(strdup(ibv_get_device_name(dev_list[i])));
      // LOG("device not specified, using first one found: %s\n",
      // config.dev_name.c_str());
    }
    if (config.dev_name == ibv_get_device_name(dev_list[i])) {
      ib_dev = dev_list[i];
      break;
    }
  }
  // if the device wasn't found in host
  if (!ib_dev) {
    fprintf(stderr, "IB device %s wasn't found\n", config.dev_name.c_str());
    rc = 1;
    goto resources_create_exit;
  }
  // get device handle
  res->ib_ctx = ibv_open_device(ib_dev);
  if (!res->ib_ctx) {
    fprintf(stderr, "failed to open device %s\n", config.dev_name.c_str());
    rc = 1;
    goto resources_create_exit;
  }
  // We are now done with device list, free it
  ibv_free_device_list(dev_list);
  dev_list = nullptr;
  ib_dev = nullptr;
  // query port properties
  if (ibv_query_port(res->ib_ctx, config.ib_port, &res->port_attr)) {
    fprintf(stderr, "ibv_query_port on port %u failed\n", config.ib_port);
    rc = 1;
    goto resources_create_exit;
  }
  // allocate Protection Domain
  res->pd = ibv_alloc_pd(res->ib_ctx);
  if (!res->pd) {
    fprintf(stderr, "ibv_alloc_pd failed\n");
    rc = 1;
    goto resources_create_exit;
  }
  // allocate the memory buffer that will hold the data
  buf_size = size;
  res->buf = new char[buf_size]();
  // register the memory buffer
  mr_flags =
      IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;
  res->mr = ibv_reg_mr(res->pd, res->buf, size, mr_flags);
  if (!res->mr) {
    fprintf(stderr, "ibv_reg_mr failed with mr_flags=0x%x\n", mr_flags);
    rc = 1;
    goto resources_create_exit;
  }
  // LOG("MR was registered with addr=%p, lkey=0x%x, rkey=0x%x, flags=0x%x\n",
  // 		res->buf, res->mr->lkey, res->mr->rkey, mr_flags);
resources_create_exit:
  if (rc) {
    // Error encountered, cleanup
    if (res->mr) {
      ibv_dereg_mr(res->mr);
      res->mr = nullptr;
    }
    if (res->buf) {
      delete[] res->buf;
      res->buf = nullptr;
    }
    if (res->pd) {
      ibv_dealloc_pd(res->pd);
      res->pd = nullptr;
    }
    if (res->ib_ctx) {
      ibv_close_device(res->ib_ctx);
      res->ib_ctx = nullptr;
    }
    if (dev_list) {
      ibv_free_device_list(dev_list);
      dev_list = nullptr;
    }
  }
  return rc;
}

struct RDMANode::rdma_connection *RDMANode::connect_qp(int sock) {
  int rc = 0;
  auto conn = new struct rdma_connection();
  conn->cq = nullptr;
  conn->qp = nullptr;
  conn->sock = sock;
  struct ibv_qp_init_attr qp_init_attr;
  // each side will send only one WR, so Completion Queue with 1 entry is enough
  conn->cq = ibv_create_cq(res->ib_ctx, config.max_cqe, nullptr, nullptr, 0);
  if (!conn->cq) {
    fprintf(stderr, "failed to create CQ with %u entries\n", config.max_cqe);
    rc = 1;
    goto connect_qp_exit;
  }
  // create the Queue Pair
  memset(&qp_init_attr, 0, sizeof(qp_init_attr));
  qp_init_attr.qp_type = IBV_QPT_RC;
  qp_init_attr.sq_sig_all = 1;
  qp_init_attr.send_cq = conn->cq;
  qp_init_attr.recv_cq = conn->cq;
  qp_init_attr.cap.max_send_wr = config.max_send_wr;
  qp_init_attr.cap.max_recv_wr = config.max_recv_wr;
  qp_init_attr.cap.max_send_sge = 1;
  qp_init_attr.cap.max_recv_sge = 1;
  conn->qp = ibv_create_qp(res->pd, &qp_init_attr);
  if (!conn->qp) {
    fprintf(stderr, "failed to create QP\n");
    rc = 1;
    goto connect_qp_exit;
  }
  // LOG("QP was created, QP number=0x%x\n", res->qp->qp_num);

  struct cm_con_data_t local_con_data;
  struct cm_con_data_t remote_con_data;
  struct cm_con_data_t tmp_con_data;
  char temp_char;
  union ibv_gid my_gid;
  if (config.gid_idx >= 0) {
    rc = ibv_query_gid(res->ib_ctx, config.ib_port, config.gid_idx, &my_gid);
    if (rc) {
      fprintf(stderr, "could not get gid for port %d, index %d\n",
              config.ib_port, config.gid_idx);
      goto connect_qp_exit;
    }
  } else
    memset(&my_gid, 0, sizeof my_gid);
  // exchange using TCP sockets info required to connect QPs
  local_con_data.addr = htonll((uintptr_t)res->buf);
  local_con_data.rkey = htonl(res->mr->rkey);
  local_con_data.qp_num = htonl(conn->qp->qp_num);
  local_con_data.lid = htons(res->port_attr.lid);
  memcpy(local_con_data.gid, &my_gid, 16);
  // LOG("\nLocal LID = 0x%x\n", res->port_attr.lid);
  if (sock_sync_data(conn->sock, sizeof(struct cm_con_data_t),
                     (char *)&local_con_data, (char *)&tmp_con_data) < 0) {
    fprintf(stderr, "failed to exchange connection data between sides\n");
    rc = 1;
    goto connect_qp_exit;
  }
  remote_con_data.addr = ntohll(tmp_con_data.addr);
  remote_con_data.rkey = ntohl(tmp_con_data.rkey);
  remote_con_data.qp_num = ntohl(tmp_con_data.qp_num);
  remote_con_data.lid = ntohs(tmp_con_data.lid);
  memcpy(remote_con_data.gid, tmp_con_data.gid, 16);
  // save the remote side attributes, we will need it for the post SR
  res->remote_props = remote_con_data;
  // LOG("Remote address = ", static_cast<unsigned long
  // long>(remote_con_data.addr), "\n"); LOG("Remote rkey = ",
  // static_cast<unsigned long long>(remote_con_data.rkey), "\n"); LOG("Remote
  // QP number = ", static_cast<unsigned long long>(remote_con_data.qp_num),
  // "\n"); LOG("Remote LID = ", static_cast<unsigned long
  // long>(remote_con_data.lid), "\n");
  if (config.gid_idx >= 0) {
    uint8_t *p = remote_con_data.gid;
    // LOG("Remote GID =", p[0], ":", p[1], ":", p[2], ":", p[3], ":", p[4],
    // ":", p[5], ":", p[6], ":", p[7], ":", p[8],
    // 	":", p[9], ":", p[10], ":", p[11], ":", p[12], ":", p[13], ":",
    // p[14],
    // ":", p[15], "\n");
  }
  // modify the QP to init
  rc = modify_qp_to_init(conn->qp);
  if (rc) {
    fprintf(stderr, "change QP state to INIT failed\n");
    goto connect_qp_exit;
  }
  // modify the QP to RTR
  rc = modify_qp_to_rtr(conn->qp, remote_con_data.qp_num, remote_con_data.lid,
                        remote_con_data.gid);
  if (rc) {
    fprintf(stderr, "failed to modify QP state to RTR\n");
    goto connect_qp_exit;
  }
  rc = modify_qp_to_rts(conn->qp);
  if (rc) {
    fprintf(stderr, "failed to modify QP state to RTR\n");
    goto connect_qp_exit;
  }
  // LOG("QP state was change to RTS\n");
  // sync to make sure that both sides are in states that they can connect to
  // prevent packet loose
  if (sock_sync_data(conn->sock, 1, "Q",
                     &temp_char)) {  // just send a dummy char back and forth
    fprintf(stderr, "sync error after QPs are were moved to RTS\n");
    rc = 1;
  }
connect_qp_exit:
  if (rc) {
    if (conn->qp) ibv_destroy_qp(conn->qp);
    if (conn->cq) ibv_destroy_cq(conn->cq);
    if (conn->sock >= 0) {
      if (close(conn->sock)) fprintf(stderr, "failed to close socket\n");
    }
    delete conn;
    return nullptr;
  } else {
    {
      std::lock_guard<std::mutex> lk(*conns_mtx);
      res->conns.push_back(conn);
    }
    after_connect_qp(conn);
  }
  return conn;
}

int RDMANode::resources_destroy() {
  int rc = 0;
  {
    std::lock_guard<std::mutex> lk(*conns_mtx);
    for (auto &conn : res->conns) {
      if (conn->qp)
        if (ibv_destroy_qp(conn->qp)) {
          fprintf(stderr, "failed to destroy QP\n");
          rc = 1;
        }
      if (conn->cq)
        if (ibv_destroy_cq(conn->cq)) {
          fprintf(stderr, "failed to destroy CQ\n");
          rc = 1;
        }
      if (conn->sock >= 0)
        if (close(conn->sock)) {
          fprintf(stderr, "failed to close socket\n");
          rc = 1;
        }
      delete conn;
    }
    res->conns.clear();
  }
  if (res->mr)
    if (ibv_dereg_mr(res->mr)) {
      fprintf(stderr, "failed to deregister MR\n");
      rc = 1;
    }
  if (res->buf) {
    delete[] res->buf;
    res->buf = nullptr;
  }
  if (res->pd)
    if (ibv_dealloc_pd(res->pd)) {
      fprintf(stderr, "failed to deallocate PD\n");
      rc = 1;
    }
  if (res->ib_ctx)
    if (ibv_close_device(res->ib_ctx)) {
      fprintf(stderr, "failed to close device context\n");
      rc = 1;
    }
  return rc;
}

int RDMANode::modify_qp_to_init(struct ibv_qp *qp) {
  struct ibv_qp_attr attr;
  int flags;
  int rc = 0;
  memset(&attr, 0, sizeof(attr));
  attr.qp_state = IBV_QPS_INIT;
  attr.port_num = config.ib_port;
  attr.pkey_index = 0;
  attr.qp_access_flags =
      IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;
  flags = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;
  rc = ibv_modify_qp(qp, &attr, flags);
  if (rc) fprintf(stderr, "failed to modify QP state to INIT\n");
  return rc;
}
int RDMANode::modify_qp_to_rtr(struct ibv_qp *qp, uint32_t remote_qpn,
                               uint16_t dlid, uint8_t *dgid) {
  struct ibv_qp_attr attr;
  int flags;
  int rc = 0;
  memset(&attr, 0, sizeof(attr));
  attr.qp_state = IBV_QPS_RTR;
  attr.path_mtu = IBV_MTU_256;
  attr.dest_qp_num = remote_qpn;
  attr.rq_psn = 0;
  attr.max_dest_rd_atomic = 1;
  attr.min_rnr_timer = 0x12;
  attr.ah_attr.is_global = 0;
  attr.ah_attr.dlid = dlid;
  attr.ah_attr.sl = 0;
  attr.ah_attr.src_path_bits = 0;
  attr.ah_attr.port_num = config.ib_port;
  if (config.gid_idx >= 0) {
    attr.ah_attr.is_global = 1;
    attr.ah_attr.port_num = 1;
    memcpy(&attr.ah_attr.grh.dgid, dgid, 16);
    attr.ah_attr.grh.flow_label = 0;
    attr.ah_attr.grh.hop_limit = 1;
    attr.ah_attr.grh.sgid_index = config.gid_idx;
    attr.ah_attr.grh.traffic_class = 0;
  }
  flags = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
          IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;
  rc = ibv_modify_qp(qp, &attr, flags);
  if (rc) fprintf(stderr, "failed to modify QP state to RTR\n");
  return rc;
}
int RDMANode::modify_qp_to_rts(struct ibv_qp *qp) {
  struct ibv_qp_attr attr;
  int flags;
  int rc = 0;
  memset(&attr, 0, sizeof(attr));
  attr.qp_state = IBV_QPS_RTS;
  attr.timeout = 0x12;
  attr.retry_cnt = 6;
  attr.rnr_retry = 0;
  attr.sq_psn = 0;
  attr.max_rd_atomic = 1;
  flags = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY |
          IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC;
  rc = ibv_modify_qp(qp, &attr, flags);
  if (rc) fprintf(stderr, "failed to modify QP state to RTS\n");
  return rc;
}

RDMAServer::RDMAServer() : RDMANode() {
  mempool_mtx = std::make_unique<std::mutex>();
  remote_memtable_pool_ = new RemoteMemTablePool();
}

RDMAServer::~RDMAServer() {
  delete remote_memtable_pool_;
  remote_memtable_pool_ = nullptr;
}

RDMAClient::RDMAClient() : RDMANode() {}

void RDMAServer::free_mem_service(struct rdma_connection *conn) {
  int64_t val[2] = {0, 0};
  read(conn->sock, reinterpret_cast<char *>(val), sizeof(int64_t) * 2);
  fprintf(stderr, "free_mem_service: %ld %ld\n", val[0], val[1]);
  if (!unpin_mem(val[0], val[1])) {
    fprintf(stderr, "unpin memory request not found\n");
  }
}

void RDMAClient::free_mem_request(struct rdma_connection *conn, int64_t addr,
                                  int64_t size) {
  int64_t val[2] = {addr, size};
  write(conn->sock, reinterpret_cast<char *>(val), sizeof(int64_t) * 2);
}

void RDMAServer::create_rmem_service(struct rdma_connection *conn,
                                     int64_t &meta_offset, int64_t &meta_size,
                                     int64_t &mem_offset, int64_t &mem_size) {
  fprintf(stderr, "Received request for rmemtable store\n");
  allocate_mem_service(conn, meta_offset, meta_size);
  allocate_mem_service(conn, mem_offset, mem_size);

  if (meta_size == 0 || mem_size == 0) {
    fprintf(stderr, "Failed to allocate memory\n");
    assert(false);
  }
}

void RDMAServer::receive_rmem_service(struct rdma_connection *conn,
                                      int64_t &meta_offset, int64_t &meta_size,
                                      int64_t &mem_offset, int64_t &mem_size) {
  remote_memtable_pool_->store(
      reinterpret_cast<void *>(get_buf() + mem_offset), mem_size,
      reinterpret_cast<void *>(get_buf() + meta_offset), meta_size);
  char ret = 1;
  ASSERT_RW(writen(conn->sock, reinterpret_cast<void *>(&ret), sizeof(char)) ==
            sizeof(char));
}

void RDMAServer::receive_remote_flush_service(struct rdma_connection *conn,
                                              int64_t &meta_offset,
                                              int64_t &meta_size) {
  int64_t meta_buf_offset = -1;
  int failed_try = 0;
  while (true) {
    meta_buf_offset = pin_mem(meta_size);
    if (meta_buf_offset == -1) {
      std::this_thread::sleep_for(
          std::chrono::milliseconds(100 * (failed_try++)));
      fprintf(stderr, "Failed to pin MR memory, retrying\n");
    } else {
      break;
    }
  }
  std::memcpy(get_buf() + meta_buf_offset, get_buf() + meta_offset, meta_size);
  char ret_op = 1;
  ASSERT_RW(writen(conn->sock, reinterpret_cast<void *>(&ret_op),
                   sizeof(char)) == sizeof(char));
  std::pair<int64_t, int64_t> job_mem_tobe_registered{
      meta_buf_offset, meta_size + meta_buf_offset};
  assert(choose_flush_job_executor(job_mem_tobe_registered) != nullptr);
}

// return remote_offset , remote_end
std::pair<int64_t, int64_t> RDMAClient::allocate_mem_request(
    struct rdma_connection *conn, int64_t size) {
  fprintf(stderr, "allocate_mem_request: size = %lld\n", size);
  write(conn->sock, reinterpret_cast<void *>(&size), sizeof(int64_t));
  int64_t ret[2];
  read(conn->sock, reinterpret_cast<char *>(ret), sizeof(int64_t) * 2);
  return std::make_pair(ret[0], ret[1]);
}

// return remote_offset , remote_end
void RDMAServer::allocate_mem_service(struct rdma_connection *conn,
                                      int64_t &ret_offset, int64_t &ret_size) {
  int64_t size = 0;
  int64_t ret[2];
  read(conn->sock, reinterpret_cast<char *>(&size), sizeof(int64_t));

  int failed_try = 1;
  while (true) {
    fprintf(stderr, "Trying to pin memory: %lld\n", size);
    int64_t pin_begin = pin_mem(size);
    if (pin_begin == -1) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100 * failed_try));
      fprintf(stderr, "Failed to pin MR memory, retrying\n");
    } else {
      ret[0] = pin_begin;
      ret[1] = pin_begin + size;
      ret_size = size;
      ret_offset = pin_begin;
      break;
    }
  }
  fprintf(stderr, "allocate remote mem: %ld %ld size=%ld\n", ret[0], ret[1],
          size);
  write(conn->sock, reinterpret_cast<void *>(ret), sizeof(int64_t) * 2);
}

struct RDMANode::rdma_connection *RDMAServer::choose_flush_job_executor(
    const std::pair<int64_t, int64_t> &job_mem_tobe_registered) {
  std::lock_guard<std::mutex> lk2(*executor_table_mtx);
  bool ret = true;
  if (!pd_.available_workers_.empty()) {
    // choose by scheduler
    TCPNode *choose_by_policy = pd_.available_workers_.front();
    pd_.available_workers_.pop();
    char choose_client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &choose_by_policy->connection_info_.sin_addr.sin_addr,
              choose_client_ip, INET_ADDRSTRLEN);
    // find executor by ip
    for (auto &it : executors_) {
      char client_ip[INET_ADDRSTRLEN];
      inet_ntop(AF_INET, &it.first->addr.sin_addr, client_ip, INET_ADDRSTRLEN);
      if (strcmp(client_ip, choose_client_ip) == 0) {
        LOG_CERR("choose executor by scheduler: ", client_ip, ' ',
                 job_mem_tobe_registered.first, ' ',
                 job_mem_tobe_registered.second);
        it.second.status++;
        it.second.flush_job_queue.push(job_mem_tobe_registered);
        return it.first;
      }
    }
  } else {
    // choose randomly
    int rd = rand() % executors_.size();
    auto it = executors_.begin();
    for (int cnt = 0; cnt < rd; cnt++) it++;
    LOG_CERR("choose executor randomly: ", job_mem_tobe_registered.first, ' ',
             job_mem_tobe_registered.second);
    it->second.status++;
    it->second.flush_job_queue.push(job_mem_tobe_registered);
    return it->first;
  }
  return nullptr;
}

bool RDMAClient::disconnect_request(struct rdma_connection *conn) {
  char req_type = 0;
  bool ret = false;
  int remote_size = sizeof(bool);
  ASSERT_RW(writen(conn->sock, reinterpret_cast<void *>(&req_type),
                   sizeof(char)) == sizeof(char));
  ASSERT_RW(readn(conn->sock, reinterpret_cast<char *>(&ret), sizeof(bool)) ==
            sizeof(bool));
  if (ret) {
    if (conn->qp) ibv_destroy_qp(conn->qp);
    conn->qp = nullptr;
    if (conn->cq) ibv_destroy_cq(conn->cq);
    conn->cq = nullptr;
    if (conn->sock >= 0) {
      if (close(conn->sock)) fprintf(stderr, "failed to close socket\n");
      conn->sock = -1;
    }
    std::lock_guard<std::mutex> lk(*conns_mtx);
    for (auto iter = res->conns.begin(); iter != res->conns.end(); iter++)
      if (*iter == conn) {
        res->conns.erase(iter);
        break;
      }
    delete conn;
  }
  return ret;
}
void RDMAServer::disconnect_service(struct rdma_connection *conn) {
  bool ret = true;
  int local_size = sizeof(bool);
  ASSERT_RW(writen(conn->sock, reinterpret_cast<void *>(&ret), local_size));
  if (conn->qp) ibv_destroy_qp(conn->qp);
  conn->qp = nullptr;
  if (conn->cq) ibv_destroy_cq(conn->cq);
  conn->cq = nullptr;
  if (conn->sock >= 0) {
    if (close(conn->sock)) fprintf(stderr, "failed to close socket\n");
    conn->sock = -1;
  }
  std::lock_guard<std::mutex> lk(*conns_mtx);
  for (auto iter = res->conns.begin(); iter != res->conns.end(); iter++)
    if (*iter == conn) {
      res->conns.erase(iter);
      break;
    }
  delete conn;
}
bool RDMAClient::register_executor_request(struct rdma_connection *conn) {
  char req_type = 3;
  bool ret = false;
  ASSERT_RW(writen(conn->sock, reinterpret_cast<void *>(&req_type),
                   sizeof(char)) == sizeof(char));
  ASSERT_RW(readn(conn->sock, reinterpret_cast<char *>(&ret), sizeof(bool)) ==
            sizeof(bool));
  return ret;
}

void RDMAClient::fetch_memtable_request(struct rdma_connection *conn,
                                        uint64_t mixed_id, void *&meta_ptr,
                                        int64_t &meta_size, void *&mem_ptr,
                                        int64_t &mem_size) {
  bool found = false;
  int64_t ret[4];
  while (!found) {
    ASSERT_RW(writen(conn->sock, reinterpret_cast<void *>(&mixed_id),
                     sizeof(uint64_t)) == sizeof(uint64_t));
    ASSERT_RW(readn(conn->sock, reinterpret_cast<char *>(ret),
                    sizeof(int64_t) * 4) == sizeof(int64_t) * 4);
    if (ret[0] == -1) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      LOG_CERR("refetch rmem: ", mixed_id);
    } else {
      break;
    }
  }
  meta_size = ret[2];
  mem_size = ret[3];
  int64_t local_meta_offset = rdma_mem_.allocate(meta_size);
  int64_t local_mem_offset = rdma_mem_.allocate(mem_size);
  rdma_read(conn, meta_size, local_meta_offset, ret[0]);
  assert(poll_completion(conn) == 0);
  rdma_read(conn, mem_size, local_mem_offset, ret[1]);
  assert(poll_completion(conn) == 0);
  ASSERT_RW(writen(conn->sock, reinterpret_cast<char *>(&found),
                   sizeof(bool)) == sizeof(bool));
  meta_ptr = malloc(meta_size);
  mem_ptr = malloc(mem_size);
  memcpy(meta_ptr, get_buf() + local_meta_offset, meta_size);
  memcpy(mem_ptr, get_buf() + local_mem_offset, mem_size);
  rdma_mem_.free(local_mem_offset);
  rdma_mem_.free(local_meta_offset);
}

void RDMAServer::fetch_memtable_service(struct rdma_connection *conn) {
  bool found = false;
  int64_t ret[4] = {-1, -1, -1, -1};
  RemoteMemTable *rmem = nullptr;
  uint64_t mixed_id = 0;
  while (!found) {
    ASSERT_RW(readn(conn->sock, reinterpret_cast<char *>(&mixed_id),
                    sizeof(uint64_t)) == sizeof(uint64_t));
    rmem = remote_memtable_pool_->get(mixed_id);
    if (rmem == nullptr) {
      fprintf(stderr, "Failed to find remote memtable:%lu\n", mixed_id);
      ASSERT_RW(writen(conn->sock, reinterpret_cast<void *>(ret),
                       sizeof(int64_t) * 4) == sizeof(int64_t) * 4);
      continue;
    } else {
      break;
    }
  }
  int failed_retry = 0;
  while (ret[0] == -1) {
    ret[0] = pin_mem(rmem->meta_size);
    std::this_thread::sleep_for(
        std::chrono::milliseconds(100 * (failed_retry++)));
  }
  failed_retry = 0;
  while (ret[1] == -1) {
    ret[1] = pin_mem(rmem->data_size);
    std::this_thread::sleep_for(
        std::chrono::milliseconds(100 * (failed_retry++)));
  }
  ret[2] = rmem->meta_size;
  ret[3] = rmem->data_size;
  memcpy(get_buf() + ret[0], rmem->meta, rmem->meta_size);
  memcpy(get_buf() + ret[1], rmem->data, rmem->data_size);
  ASSERT_RW(writen(conn->sock, reinterpret_cast<void *>(ret),
                   sizeof(int64_t) * 4) == sizeof(int64_t) * 4);
  //  need gc
  ASSERT_RW(readn(conn->sock, reinterpret_cast<char *>(&found), sizeof(bool)) ==
            sizeof(bool));
  unpin_mem(ret[0], rmem->meta_size);
  unpin_mem(ret[1], rmem->data_size);
}

bool RDMAClient::register_memtable_read_request(
    struct rdma_connection *conn, size_t &local_offset,
    std::pair<size_t, size_t> &remote_offset, uint64_t id) {
  char req_type = 6;
  bool ret = true;
  ASSERT_RW(writen(conn->sock, reinterpret_cast<void *>(&req_type),
                   sizeof(char)) == sizeof(char));
  size_t read_request_area = 100;
  size_t local_offset_ = rdma_mem_.allocate(read_request_area);
  if (local_offset_ < 0) {
    fprintf(stderr, "Failed to allocate memory for read request\n");
    return false;
  }
  local_offset = local_offset_;
  auto remote_read_reg = allocate_mem_request(conn, read_request_area);
  remote_offset = remote_read_reg;

  return ret;
}

void RDMAServer::register_memtable_read_service(struct rdma_connection *conn,
                                                std::thread *t,
                                                bool *should_close) {
  bool ret = true;
  int64_t ret_offset = 0, ret_size = 0;
  allocate_mem_service(conn, ret_offset, ret_size);
  *t = std::move(std::thread([&, this, conn]() {
    while (!*should_close) {
      post_receive(conn, ret_size, ret_offset);
      // poll_completion(conn);
      // todo: deal with read request
    }
  }));
}

void RDMAServer::register_executor_service(struct rdma_connection *conn) {
  bool ret = true;
  std::lock_guard<std::mutex> lk(*executor_table_mtx);
  executors_[conn].status = true;
  ASSERT_RW(writen(conn->sock, reinterpret_cast<void *>(&ret), sizeof(bool)) ==
            sizeof(bool));
}

// receive begin end
std::pair<int64_t, int64_t> RDMAClient::wait_for_job_request(
    struct rdma_connection *conn) {
  char req_type = 4;
  int64_t ret[2];
  int remote_size = sizeof(int64_t) * 2;
  ASSERT_RW(writen(conn->sock, reinterpret_cast<void *>(&req_type),
                   sizeof(char)) == sizeof(char));
  ASSERT_RW(readn(conn->sock, reinterpret_cast<char *>(&ret),
                  sizeof(int64_t) * 2) == sizeof(int64_t) * 2);
  return std::make_pair(ret[0], ret[1]);
}

void RDMAServer::wait_for_job_service(struct rdma_connection *conn) {
  int64_t ret[2];
  while (true) {
    std::lock_guard<std::mutex> lk(*executor_table_mtx);
    if (!executors_[conn].flush_job_queue.empty()) {
      ret[0] = std::get<0>(executors_[conn].flush_job_queue.front());
      ret[1] = std::get<1>(executors_[conn].flush_job_queue.front());
      executors_[conn].current_job = executors_[conn].flush_job_queue.front();
      executors_[conn].flush_job_queue.pop();
      break;
    }
  }
  ASSERT_RW(writen(conn->sock, reinterpret_cast<void *>(&ret),
                   sizeof(int64_t) * 2) == sizeof(int64_t) * 2);
}

bool RDMAServer::service(struct rdma_connection *conn) {
  bool should_close = false;
  int64_t meta_offset = 0, meta_size = 0, mem_offset = 0, mem_size = 0;
  while (!should_close) {
    char req_type;
    ASSERT_RW(readn(conn->sock, reinterpret_cast<char *>(&req_type),
                    sizeof(char)) == sizeof(char));
    switch (req_type) {
      case 0:
        LOG_CERR("SERVICE:disconnect service");
        should_close = true;
        if (meta_size > 0) unpin_mem(meta_offset, meta_size);
        if (mem_size > 0) unpin_mem(mem_offset, mem_size);
        disconnect_service(conn);
        break;
      case 1:
        LOG_CERR("SERVICE:allocate mem service");
        int64_t ret_offset, ret_size;
        allocate_mem_service(conn, ret_offset, ret_size);
        break;
      case 2:
        LOG_CERR("SERVICE:free mem service");
        free_mem_service(conn);
        break;
      case 3:
        LOG_CERR("SERVICE:register executor service");
        register_executor_service(conn);
        break;
      case 4:
        LOG_CERR("SERVICE:wait for job service");
        wait_for_job_service(conn);
        break;
      case 5:
        LOG_CERR("SERVICE:create rmem connection");
        create_rmem_service(conn, meta_offset, meta_size, mem_offset, mem_size);
        break;
      case 6:
        LOG_CERR("SERVICE:receive rmem");
        receive_rmem_service(conn, meta_offset, meta_size, mem_offset,
                             mem_size);
        break;
      case 7: {
        LOG_CERR("SERVICE:free rmem");
        // delete staled immutable tables
        uint64_t id;
        ASSERT_RW(readn(conn->sock, reinterpret_cast<char *>(&id),
                        sizeof(id)) == sizeof(id));
        Status s = remote_memtable_pool_->delete_remote_memtable(id);
        if (!s.ok()) {
          fprintf(
              stderr,
              "Failed to delete remote memtable %lu, might cause memory leak\n",
              id);
        } else {
          char ret = 1;
          ASSERT_RW(writen(conn->sock, reinterpret_cast<char *>(&ret),
                           sizeof(char)) == sizeof(char));
        }
        break;
      }
      case 8: {
        LOG_CERR("SERVICE:receive remote flush service");
        receive_remote_flush_service(conn, meta_offset, meta_size);
        break;
      }
      case 9: {
        LOG_CERR("SERVICE:create remote flush service");
        // create remote flush service
        allocate_mem_service(conn, meta_offset, meta_size);
        break;
      }
      case 10: {
        LOG_CERR("SERVICE:fetch memtable service");
        fetch_memtable_service(conn);
      }
      default:
        fprintf(stderr, "Unknown request type from client: %d\n", req_type);
    }
  }

  return true;
}

TCPNode *PlacementDriver::choose_worker(const placement_info &info) {
  double base =
      1.0 * info.current_background_job_num_ / max_background_job_num_ +
      1.0 * info.current_hdfs_io_ / max_hdfs_io_;
  LOG_CERR("generator: ", info.current_background_job_num_, " ",
           info.current_hdfs_io_);
  TCPNode *choose = nullptr;
  for (auto &worker : workers_) {
    auto val = peers_.at(worker);
    LOG_CERR("worker: ", val.current_background_job_num_, " ",
             val.current_hdfs_io_);
    double cal =
        1.0 * val.current_background_job_num_ / max_background_job_num_ +
        1.0 * val.current_hdfs_io_ / max_hdfs_io_;
    if (cal < base) {
      base = cal;
      choose = worker;
    }
  }
  return choose;
}

void PlacementDriver::step(bool from_generator, size_t id,
                           placement_info info) {
  if (from_generator) {
    // handle MsgFlushRequest
    assert(peers_.find(generators_[id - 1]) != peers_.end());
    peers_.at(generators_[id - 1]) = info;
    TCPNode *worker = choose_worker(info);
    if (worker == nullptr) {
      // fallback to local flush
      bool admit = false;
      generators_[id - 1]->send(&admit, sizeof(bool));
    } else {
      bool admit = true;
      generators_[id - 1]->send(&admit, sizeof(bool));
      available_workers_.push(worker);
      peers_.at(worker).current_background_job_num_++;
    }
  } else {
    // handle MsgHeartBeat
    assert(peers_.find(workers_[id - 1]) != peers_.end());
    peers_.at(workers_[id - 1]) = info;
  }
}

void PlacementDriver::poll_events(int port) {
  int sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0) {
    fprintf(stderr, "Failed creating socket\n");
    assert(false);
    return;
  }
  struct sockaddr_in serv_addr;
  serv_addr.sin_family = AF_INET;
  serv_addr.sin_addr.s_addr = INADDR_ANY;
  serv_addr.sin_port = htons(port);

  if (bind(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
    close(sock);
    fprintf(stderr, "Failed binding socket\n");
    assert(false);
    return;
  }
  if (::listen(sock, 10) < 0) {
    close(sock);
    fprintf(stderr, "Failed listening socket\n");
    assert(false);
    return;
  }

  while (true) {
    std::vector<struct pollfd> pollfds;
    pollfds.push_back({});
    pollfds.back().fd = sock;
    pollfds.back().events = POLLIN;
    for (auto &node : workers_) {
      pollfds.push_back({});
      pollfds.back().fd = node->connection_info_.client_sockfd;
      pollfds.back().events = POLLIN;
    }
    for (auto &node : generators_) {
      pollfds.push_back({});
      pollfds.back().fd = node->connection_info_.client_sockfd;
      pollfds.back().events = POLLIN;
    }

    int ret = poll(pollfds.data(), pollfds.size(), -1);
    if (ret < 0) {
      fprintf(stderr, "Poll error\n");
      return;
    }
    assert(pollfds.size() == workers_.size() + generators_.size() + 1);
    std::vector<std::thread> all_threads;
    for (int i = 1; i < (int)pollfds.size(); i++) {
      if (pollfds[i].revents & POLLIN) {
        if (i <= (int)workers_.size()) {
          placement_info val;
          workers_[i - 1]->receive(&val, sizeof(val));
          LOG_CERR("workerid:", i, " info:", val.current_background_job_num_,
                   " ", val.current_hdfs_io_);
          all_threads.emplace_back([this, i, val]() { step(false, i, val); });
        } else {
          placement_info val;
          generators_[i - 1 - workers_.size()]->receive(&val, sizeof(val));
          all_threads.emplace_back(
              [this, i, val]() { step(true, i - workers_.size(), val); });
        }
      }
    }
    for (auto &all_thread : all_threads) all_thread.join();
    if (pollfds[0].revents & POLLIN) {
      sockaddr_in client_addr;
      socklen_t client_addr_len = sizeof(client_addr);
      int client_sockfd =
          accept(sock, (struct sockaddr *)&client_addr, &client_addr_len);
      if (client_sockfd < 0) {
        fprintf(stderr, "Failed accepting socket\n");
        close(sock);
        return;
      }
      auto node = new TCPNode(client_addr, client_sockfd);
      assert(node != nullptr);
      bool is_worker = false;
      node->receive(&is_worker, sizeof(bool));
      if (is_worker) {
        size_t size = workers_.size() + 1;
        node->send(&size, sizeof(size_t));
        peers_.insert(std::make_pair(node, placement_info{0, 0}));
        workers_.push_back(node);
      } else {
        size_t size = generators_.size() + 1;
        node->send(&size, sizeof(size_t));
        peers_.insert(std::make_pair(node, placement_info{0, 0}));
        generators_.push_back(node);
      }
    }
  }
}

}  // namespace ROCKSDB_NAMESPACE
