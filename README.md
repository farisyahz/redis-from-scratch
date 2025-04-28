# redis-from-scratch

A Redis-compatible server implementation written in C++17, built from the ground up with performance in mind.

## About
This project implements core Redis functionality with a focus on performance optimization and low-level systems programming. The server currently handles the essential Redis commands (`PING`, `GET`, `SET`, `ECHO`) and includes key expiration support.

## Key Features
- **Lightweight C++17 implementation** with minimal dependencies  
- **Custom RESP protocol parser** optimized for performance  
- **Efficient key–value storage** using a custom hash function  
- **TCP socket optimizations** with tuned buffer settings  
- **Docker support** via multi-stage builds  

## Performance
- Achieves approximately **30%** of Redis 7’s throughput  
  - ~18K RPS for `PING`  
  - ~5K RPS for `GET`/`SET`  
- Active development with ongoing tuning to narrow the gap with Redis 7

## Goals
- Continue optimizing toward Redis 7 performance metrics  
- Implement additional Redis commands (e.g., `DEL`, `INCR`, `EXPIRE`)  
- Add persistence options (RDB/AOF)  
- Improve concurrency handling (thread pools, sharding)
