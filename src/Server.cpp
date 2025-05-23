#include <iostream>
#include <cstdlib>
#include <string>
#include <cstring>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <thread>
#include <vector>
#include <regex>
#include <map>
#include <chrono>
#include <unordered_map>
#include <functional>
#include <string_view>
#include <atomic>
#include <memory>
#include <fcntl.h>

#define BUFFER_SIZE 32768
#define READ_BUFFER_POOL_SIZE 32
#define MAX_CLIENTS 1000
#define INITIAL_STORAGE_SIZE 100000

struct StringHash {
  std::size_t operator()(const std::string& str) const {
    std::size_t hash = 14695981039346656037ULL;
    for (char c : str) {
      hash ^= static_cast<size_t>(c);
      hash *= 1099511628211ULL;
    }
    return hash;
  }
};

struct RedisObj {
  std::string value;
  int64_t expiry = -1;
  std::chrono::steady_clock::time_point start = {};
};

std::unordered_map<std::string, RedisObj, StringHash> storage;

const std::string PONG_RESPONSE = "+PONG\r\n";
const std::string OK_RESPONSE = "+OK\r\n";
const std::string NIL_RESPONSE = "$-1\r\n";
const std::string ERR_UNKNOWN_CMD = "-ERR unknown command\r\n";
const std::string ERR_WRONG_ARGS_SET = "-ERR wrong number of arguments for 'set' command\r\n";
const std::string ERR_WRONG_ARGS_GET = "-ERR wrong number of arguments for 'get' command\r\n";
const std::string ERR_WRONG_ARGS_ECHO = "-ERR wrong number of arguments for 'echo' command\r\n";

std::vector<std::string_view> redisDecoder(const char* buf, size_t len) {
  std::vector<std::string_view> res;
  res.reserve(8);
  size_t pos = 0;

  if (len >= 4 && buf[0] == 'P' && buf[1] == 'I' && buf[2] == 'N' && buf[3] == 'G') {
    res.emplace_back("PING");
    return res;
  }

  if (pos < len && (buf[pos] >= 'a' && buf[pos] <= 'z') || (buf[pos] >= 'A' && buf[pos] <= 'Z')) {
    size_t end = pos;
    while (end < len && buf[end] != '\r' && buf[end] != '\n') end++;
    if (end < len) {
      res.emplace_back(buf + pos, end - pos);
      return res;
    }
  }

  if (pos < len && buf[pos] == '*') {
    size_t end = pos + 1;
    while (end < len && buf[end] != '\r') end++;
    if (end + 1 >= len || buf[end + 1] != '\n') return res;
    
    int arrLen = 0;
    for (size_t i = pos + 1; i < end; i++) {
      arrLen = arrLen * 10 + (buf[i] - '0');
    }
    pos = end + 2;

    res.reserve(arrLen);

    for (int i = 0; i < arrLen && pos < len; i++) {
      if (buf[pos] != '$') break;
      
      end = pos + 1;
      while (end < len && buf[end] != '\r') end++;
      if (end + 1 >= len || buf[end + 1] != '\n') return res;
      
      int strLen = 0;
      for (size_t i = pos + 1; i < end; i++) {
        strLen = strLen * 10 + (buf[i] - '0');
      }
      pos = end + 2;
      
      if (pos + strLen + 2 > len) return res;
      
      res.emplace_back(buf + pos, strLen);
      pos += strLen + 2;
    }
  }

  return res;
}

inline std::string redisEncoder(const std::string_view& str) {
  const size_t respLen = str.length() + 15;
  std::string res;
  res.reserve(respLen);
  
  res.push_back('$');
  
  size_t len = str.length();
  char lenBuf[20];
  size_t lenDigits = 0;
  
  if (len == 0) {
    lenBuf[0] = '0';
    lenDigits = 1;
  } else {
    while (len > 0) {
      lenBuf[lenDigits++] = '0' + (len % 10);
      len /= 10;
    }
  }
  
  for (size_t i = lenDigits; i > 0; i--) {
    res.push_back(lenBuf[i-1]);
  }
  
  res.append("\r\n", 2);
  res.append(str.data(), str.length());
  res.append("\r\n", 2);
  
  return res;
}

inline bool sendAll(int fd, const std::string& data) {
  const char* ptr = data.c_str();
  size_t remaining = data.length();
  
  while (remaining > 0) {
    ssize_t sent = send(fd, ptr, remaining, MSG_NOSIGNAL);
    if (sent <= 0) {
      if (errno == EINTR) continue;
      return false;
    }
    
    ptr += sent;
    remaining -= sent;
  }
  
  return true;
}

inline bool sendPong(int fd) {
  return sendAll(fd, PONG_RESPONSE);
}

inline bool sendOK(int fd) {
  return sendAll(fd, OK_RESPONSE);
}

inline bool sendNil(int fd) {
  return sendAll(fd, NIL_RESPONSE);
}

inline bool isCaseInsensitiveEqual(const std::string_view& a, const char* b) {
  if (a.length() != strlen(b)) return false;
  for (size_t i = 0; i < a.length(); i++) {
    if (toupper(a[i]) != toupper(b[i])) return false;
  }
  return true;
}

void setNonBlocking(int sock) {
  int flags = fcntl(sock, F_GETFL, 0);
  if (flags == -1) return;
  fcntl(sock, F_SETFL, flags | O_NONBLOCK);
}

void handle(int fd) {
  struct sockaddr_in peer;
  socklen_t peer_len = sizeof(peer);
  getpeername(fd, (struct sockaddr*)&peer, &peer_len);
  std::cout << "New client connected from " << inet_ntoa(peer.sin_addr) << ":" << ntohs(peer.sin_port) << std::endl;

  char buf[BUFFER_SIZE] = {0};
  size_t remaining_size = 0;

  int flag = 1;
  if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (char*)&flag, sizeof(flag)) < 0) {
    std::cerr << "Failed to set TCP_NODELAY on client socket\n";
  }

  int bufSize = 1024 * 1024;
  setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bufSize, sizeof(bufSize));
  setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bufSize, sizeof(bufSize));

  while (true) {
    ssize_t n = recv(fd, buf + remaining_size, sizeof(buf) - remaining_size, 0);
    if (n <= 0) {
      if (n < 0) {
        std::cerr << "Error receiving data: " << strerror(errno) << std::endl;
      }
      break;
    }

    std::cout << "Received " << n << " bytes from client" << std::endl;
    std::cout << "Raw data: ";
    for (size_t i = 0; i < n; i++) {
      if (buf[i] >= 32 && buf[i] <= 126) {
        std::cout << buf[i];
      } else {
        std::cout << "\\x" << std::hex << (int)(unsigned char)buf[i];
      }
    }
    std::cout << std::endl;
    
    n += remaining_size;
    auto decoded = redisDecoder(buf, n);
    if (decoded.empty()) {
      std::cout << "Incomplete command, buffering..." << std::endl;
      if (n < sizeof(buf)) {
        remaining_size = n;
      } else {
        remaining_size = 0;
      }
      continue;
    }

    remaining_size = 0;
    std::string_view& cmd = decoded[0];
    
    if (cmd.length() == 4 && 
        (cmd[0] == 'P' || cmd[0] == 'p') && 
        (cmd[1] == 'I' || cmd[1] == 'i') && 
        (cmd[2] == 'N' || cmd[2] == 'n') && 
        (cmd[3] == 'G' || cmd[3] == 'g')) {
      if (!sendPong(fd)) break;
      continue;
    }
    
    if (cmd.length() == 3 && 
        (cmd[0] == 'G' || cmd[0] == 'g') && 
        (cmd[1] == 'E' || cmd[1] == 'e') && 
        (cmd[2] == 'T' || cmd[2] == 't')) {
      if (decoded.size() < 2) {
        if (!sendAll(fd, ERR_WRONG_ARGS_GET)) break;
        continue;
      }

      auto it = storage.find(std::string(decoded[1]));
      if (it == storage.end()) {
        if (!sendNil(fd)) break;
        continue;
      }

      auto& obj = it->second;
      if (obj.expiry >= 0) {
        auto now = std::chrono::steady_clock::now();
        auto age = std::chrono::duration_cast<std::chrono::milliseconds>(now - obj.start).count();
        if (age >= obj.expiry) {
          storage.erase(it);
          if (!sendNil(fd)) break;
          continue;
        }
      }

      if (!sendAll(fd, redisEncoder(obj.value))) break;
      continue;
    }
    
    if (cmd.length() == 3 && 
        (cmd[0] == 'S' || cmd[0] == 's') && 
        (cmd[1] == 'E' || cmd[1] == 'e') && 
        (cmd[2] == 'T' || cmd[2] == 't')) {
      if (decoded.size() < 3) {
        if (!sendAll(fd, ERR_WRONG_ARGS_SET)) break;
        continue;
      }

      std::string key(decoded[1]);
      RedisObj r;
      r.value = std::string(decoded[2]);

      if (decoded.size() > 4 && isCaseInsensitiveEqual(decoded[3], "PX")) {
        r.start = std::chrono::steady_clock::now();
        
        const std::string_view& expStr = decoded[4];
        r.expiry = 0;
        for (char c : expStr) {
          if (c >= '0' && c <= '9') {
            r.expiry = r.expiry * 10 + (c - '0');
          }
        }
      }

      storage[std::move(key)] = std::move(r);
      if (!sendOK(fd)) break;
      continue;
    }
    
    if (cmd.length() == 4 && 
        (cmd[0] == 'E' || cmd[0] == 'e') && 
        (cmd[1] == 'C' || cmd[1] == 'c') && 
        (cmd[2] == 'H' || cmd[2] == 'h') && 
        (cmd[3] == 'O' || cmd[3] == 'o')) {
      if (decoded.size() < 2) {
        if (!sendAll(fd, ERR_WRONG_ARGS_ECHO)) break;
        continue;
      }
      if (!sendAll(fd, redisEncoder(decoded[1]))) break;
      continue;
    }
    
    if (!sendAll(fd, ERR_UNKNOWN_CMD)) break;
  }

  std::cout << "Client disconnected" << std::endl;
  close(fd);
}

int main(int argc, char **argv) {
  std::cout << std::unitbuf;
  std::cerr << std::unitbuf;
  
  std::cout << "Starting Redis server..." << std::endl;
  
  int server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd < 0) {
    std::cerr << "Failed to create server socket: " << strerror(errno) << std::endl;
    return 1;
  }
  
  int reuse = 1;
  if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
    std::cerr << "setsockopt SO_REUSEADDR failed: " << strerror(errno) << std::endl;
    return 1;
  }
  
  int flag = 1;
  if (setsockopt(server_fd, IPPROTO_TCP, TCP_NODELAY, (char*)&flag, sizeof(flag)) < 0) {
    std::cerr << "Failed to set TCP_NODELAY on server socket: " << strerror(errno) << std::endl;
  }
  
  int bufSize = 1024 * 1024;
  setsockopt(server_fd, SOL_SOCKET, SO_SNDBUF, &bufSize, sizeof(bufSize));
  setsockopt(server_fd, SOL_SOCKET, SO_RCVBUF, &bufSize, sizeof(bufSize));
  
  struct sockaddr_in server_addr;
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = INADDR_ANY;
  server_addr.sin_port = htons(6379);
  
  if (bind(server_fd, (struct sockaddr *) &server_addr, sizeof(server_addr)) != 0) {
    std::cerr << "Failed to bind to port 6379: " << strerror(errno) << std::endl;
    return 1;
  }
  
  int connection_backlog = 128;
  if (listen(server_fd, connection_backlog) != 0) {
    std::cerr << "listen failed: " << strerror(errno) << std::endl;
    return 1;
  }
  
  std::cout << "Server listening on port 6379" << std::endl;

  std::cout << "Logs from your program will appear here!\n";

  struct sockaddr_in client_addr;
  socklen_t client_addr_len = sizeof(client_addr);

  while (true) {
    int client_fd = accept(server_fd, (struct sockaddr *) &client_addr, &client_addr_len);
    if (client_fd < 0) {
      std::cerr << "accept failed: " << strerror(errno) << std::endl;
      continue;
    }

    std::cout << "Client connected\n";
    std::thread t(handle, client_fd);
    t.detach();
  }

  close(server_fd);
  return 0;
}
