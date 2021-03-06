// Templated client that does what we want.
//
// lockbox::UserID user_id =
//     client.Exec<lockbox::UserID, const lockbox::UserAuth&>(
//         &lockbox::LockboxServiceClient::RegisterUser,
//         auth);
// lockbox::DeviceID device_id =
//     client.Exec<lockbox::DeviceID, const lockbox::UserAuth&>(
//         &lockbox::LockboxServiceClient::RegisterDevice,
//         auth);
// lockbox::TopDirID top_dir_id =
//     client.Exec<lockbox::TopDirID, const lockbox::UserAuth&>(
//         &lockbox::LockboxServiceClient::RegisterTopDir,
//         auth);
#pragma once

#include <mutex>

#include "LockboxService.h"
#include <thrift/transport/TSocket.h>
#include <thrift/transport/TBufferTransports.h>
#include <thrift/protocol/TBinaryProtocol.h>

#include "scoped_mutex.h"
#include "db_manager_client.h"

using std::string;
using std::vector;
using std::mutex;
using std::unique_lock;

namespace lockbox {

const int kNumTransportAttempts = 3;

class Client {
 public:
  struct ConnInfo {
    ConnInfo(std::string host, int port) : host(host), port(port) {}

    std::string host;
    int port;
  };

  // Does not take ownership of |user_auth| or |dbm|.
  Client(const ConnInfo& conn_info, UserAuth* user_auth, DBManagerClient* dbm)
    : socket_(new apache::thrift::transport::TSocket(conn_info.host,
                                                       conn_info.port)),
      transport_(new apache::thrift::transport::TFramedTransport(socket_)),
      protocol_(new apache::thrift::protocol::TBinaryProtocol(transport_)),
      service_client_(protocol_),
      host_(conn_info.host),
      port_(conn_info.port),
      user_auth_(user_auth),
      dbm_(dbm) {
  }

  virtual ~Client() {}

  void RegisterUser();

  void RegisterTopDir();

  void Share();

  void Start();

  // Driver for the LockboxServiceClient code.
  template <typename R, typename... Args>
  R Exec(R(LockboxServiceClient::*func)(Args...), Args... args) {
    return execer<R(Args...)>::exec(&socket_mutex_, transport_,
                                    service_client_, func,
                                    std::forward<Args>(args)...);
  }

 private:
  // Specialized driver for the Thrift API. In particular, the template wrappers
  // are necessary to account for some void return types given that the Thrift
  // code offers functions with void and non-void return types.
  template <typename F>
  struct execer {};

  // These execer functions use a mutex to enforce only one method per process
  // is using this client. Thrift is not able to support multi-threaded sockets.

  // Executes for non-void return type service calls.
  template<typename R, typename... Args>
  struct execer<R(Args...)> {
    static R exec(
        mutex* socket_mutex,
        boost::shared_ptr<apache::thrift::transport::TTransport> transport_,
        LockboxServiceClient service_client_,
        R(LockboxServiceClient::*func)(Args...), Args... args) {
      unique_lock<mutex> lock(*socket_mutex);
      int attempts = 0;
      R ret;
      for (attempts = 0; attempts < kNumTransportAttempts; attempts++) {
        try {
          transport_->open();
          ret = (service_client_.*func)(std::forward<Args>(args)...);
          transport_->close();
          break;
        } catch (apache::thrift::transport::TTransportException& e) {
          LOG(ERROR) << "Problem with transport: " << e.what();
          sleep(7);
        }
      }
      CHECK(attempts < kNumTransportAttempts);
      lock.unlock();
      return ret;
    }
  };
  // Executes void return type service calls.
  template<typename... Args>
  struct execer<void(Args...)> {
    static void exec(
        mutex* socket_mutex,
        boost::shared_ptr<apache::thrift::transport::TTransport> transport_,
        LockboxServiceClient service_client_,
        void(LockboxServiceClient::*func)(Args...), Args... args) {
      unique_lock<mutex> lock(*socket_mutex);
      int attempts = 0;
      for (attempts = 0; attempts < kNumTransportAttempts; attempts++) {
        try {
          transport_->open();
          (service_client_.*func)(std::forward<Args>(args)...);
          transport_->close();
          break;
        } catch (apache::thrift::transport::TTransportException& e) {
          LOG(ERROR) << "Problem with transport: " << e.what();
          sleep(7);
        }
      }
      CHECK(attempts < kNumTransportAttempts);

      lock.unlock();
    }
  };

  boost::shared_ptr<apache::thrift::transport::TSocket> socket_;
  boost::shared_ptr<apache::thrift::transport::TTransport> transport_;
  boost::shared_ptr<apache::thrift::protocol::TProtocol> protocol_;

  LockboxServiceClient service_client_;
  mutex socket_mutex_;

  std::string host_;
  int port_;
  UserAuth* user_auth_;
  DBManagerClient* dbm_;
};

} // namespace lockbox
