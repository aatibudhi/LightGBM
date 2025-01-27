#ifdef USE_SOCKET
#include "linkers.h"

#include <LightGBM/utils/common.h>
#include <LightGBM/utils/text_reader.h>

#include <LightGBM/config.h>

#include <cstring>

#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <thread>
#include <chrono>
#include <string>

namespace LightGBM {

Linkers::Linkers(NetworkConfig config) {
  // start up socket
  TcpSocket::Startup();
  network_time_ = std::chrono::duration<double, std::milli>(0);
  num_machines_ = config.num_machines;
  local_listen_port_ = config.local_listen_port;
  socket_timeout_ = config.time_out;
  rank_ = -1;
  // parser clients from file
  ParseMachineList(config.machine_list_filename.c_str());

  if (num_machines_ <= 1) {
    return;
  }

  if (rank_ == -1) {
    // get ip list of local machine
    std::unordered_set<std::string> local_ip_list = TcpSocket::GetLocalIpList();
    // get local rank
    for (size_t i = 0; i < client_ips_.size(); ++i) {
      if (local_ip_list.count(client_ips_[i]) > 0 && client_ports_[i] == local_listen_port_) {
        rank_ = static_cast<int>(i);
        break;
      }
    }
  }
  if (rank_ == -1) {
    Log::Fatal("Machine list file doesn't contain local machine");
  }
  // construct listener
  listener_ = new TcpSocket();
  TryBind(local_listen_port_);

  for (int i = 0; i < num_machines_; ++i) {
    linkers_.push_back(nullptr);
  }
  
  // construct communication topo
  bruck_map_ = BruckMap::Construct(rank_, num_machines_);
  recursive_halving_map_ = RecursiveHalvingMap::Construct(rank_, num_machines_);

  // construct linkers
  Construct();
  // free listener
  listener_->Close();
  delete listener_;
}

Linkers::~Linkers() {
  for (size_t i = 0; i < linkers_.size(); ++i) {
    if (linkers_[i] != nullptr) {
      linkers_[i]->Close();
      delete linkers_[i];
    }
  }
  TcpSocket::Finalize();
  Log::Info("Network using %f seconds", network_time_ * 1e-3);
}

void Linkers::ParseMachineList(const char * filename) {
  TextReader<size_t> machine_list_reader(filename, false);
  machine_list_reader.ReadAllLines();
  if (machine_list_reader.Lines().size() <= 0) {
    Log::Fatal("Machine list file:%s doesn't exist", filename);
  }

  for (auto& line : machine_list_reader.Lines()) {
    line = Common::Trim(line);
    if (line.find_first_of("rank=") != std::string::npos) {
      std::vector<std::string> str_after_split = Common::Split(line.c_str(), '=');
      Common::Atoi(str_after_split[1].c_str(), &rank_);
      continue;
    }
    std::vector<std::string> str_after_split = Common::Split(line.c_str() , ' ');
    if (str_after_split.size() != 2) {
      continue;
    }
    if (client_ips_.size() >= static_cast<size_t>(num_machines_)) {
      Log::Warning("The #machine in machine_list is larger than parameter num_machines, the redundant will ignored");
      break;
    }
    str_after_split[0] = Common::Trim(str_after_split[0]);
    str_after_split[1] = Common::Trim(str_after_split[1]);
    client_ips_.push_back(str_after_split[0]);
    client_ports_.push_back(atoi(str_after_split[1].c_str()));
  }
  if (client_ips_.size() != static_cast<size_t>(num_machines_)) {
    Log::Warning("The world size is bigger the #machine in machine list, change world size to %d .", client_ips_.size());
    num_machines_ = static_cast<int>(client_ips_.size());
  }
}

void Linkers::TryBind(int port) {
  Log::Info("try to bind port %d.", port);
  if (listener_->Bind(port)) {
    Log::Info("Binding port %d success.", port);
  } else {
    Log::Fatal("Binding port %d failed.", port);
  }
}

void Linkers::SetLinker(int rank, const TcpSocket& socket) {
  linkers_[rank] = new TcpSocket(socket);
  // set timeout
  linkers_[rank]->SetTimeout(socket_timeout_ * 1000 * 60);
}

void Linkers::ListenThread(int incoming_cnt) {
  Log::Info("Listening...");
  char buffer[100];
  int connected_cnt = 0;
  while (connected_cnt < incoming_cnt) {
    // accept incoming socket
    TcpSocket handler = listener_->Accept();
    if (handler.IsClosed()) {
      continue;
    }
    // receive rank
    int read_cnt = 0;
    int size_of_int = static_cast<int>(sizeof(int));
    while (read_cnt < size_of_int) {
      int cur_read_cnt = handler.Recv(buffer + read_cnt, size_of_int - read_cnt);
      read_cnt += cur_read_cnt;
    }
    int* ptr_in_rank = reinterpret_cast<int*>(buffer);
    int in_rank = *ptr_in_rank;
    // add new socket
    SetLinker(in_rank, handler);
    ++connected_cnt;
  }
}

void Linkers::Construct() {
  // save ranks that need to connect with
  std::unordered_map<int, int> need_connect;
  for (int i = 0; i < bruck_map_.k; ++i) {
    need_connect[bruck_map_.out_ranks[i]] = 1;
    need_connect[bruck_map_.in_ranks[i]] = 1;
  }
  if (recursive_halving_map_.type != RecursiveHalvingNodeType::Normal) {
    need_connect[recursive_halving_map_.neighbor] = 1;
  }
  if (recursive_halving_map_.type != RecursiveHalvingNodeType::Other) {
    for (int i = 0; i < recursive_halving_map_.k; ++i) {
      need_connect[recursive_halving_map_.ranks[i]] = 1;
    }
  }

  int need_connect_cnt = 0;
  int incoming_cnt = 0;
  for (auto it = need_connect.begin(); it != need_connect.end(); ++it) {
    int machine_rank = it->first;
    if (machine_rank >= 0 && machine_rank != rank_) {
      ++need_connect_cnt;
    }
    if (machine_rank < rank_) {
      ++incoming_cnt;
    }
  }
  // start listener
  listener_->SetTimeout(socket_timeout_);
  listener_->Listen(incoming_cnt);
  std::thread listen_thread(&Linkers::ListenThread, this, incoming_cnt);
  const int connect_fail_retry_cnt = 20;
  const int connect_fail_delay_time = 10 * 1000;  // 10s
  // start connect
  for (auto it = need_connect.begin(); it != need_connect.end(); ++it) {
    int out_rank = it->first;
    // let smaller rank connect to larger rank
    if (out_rank > rank_) {
      TcpSocket cur_socket;
      for (int i = 0; i < connect_fail_retry_cnt; ++i) {
        if (cur_socket.Connect(client_ips_[out_rank].c_str(), client_ports_[out_rank])) {
          break;
        } else {
          Log::Warning("Connect to rank %d failed, wait for %d milliseconds", out_rank, connect_fail_delay_time);
          std::this_thread::sleep_for(std::chrono::milliseconds(connect_fail_delay_time));
        }
      }
      // send local rank
      cur_socket.Send(reinterpret_cast<const char*>(&rank_), sizeof(rank_));
      SetLinker(out_rank, cur_socket);
    }
  }
  // wait for listener
  listen_thread.join();
  // print connected linkers
  PrintLinkers();
}

bool Linkers::CheckLinker(int rank) {
  if (linkers_[rank] == nullptr || linkers_[rank]->IsClosed()) {
    return false;
  }
  return true;
}

void Linkers::PrintLinkers() {
  for (int i = 0; i < num_machines_; ++i) {
    if (CheckLinker(i)) {
      Log::Info("Connected to rank %d.", i);
    }
  }
}

}  // namespace LightGBM

#endif  // USE_SOCKET
