#pragma once
// Shared local Narwal transport and protobuf primitives.
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include "../vendor/miniz/miniz_tinfl.h"

namespace narwal {

using Bytes = std::vector<uint8_t>;
using Clock = std::chrono::steady_clock;
inline thread_local Clock::time_point deadline;
static constexpr size_t max_message = 512 * 1024;

inline void require(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
}
inline void wait_socket(int fd, short event) {
  for (;;) {
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    require(ms > 0, "connection/status timeout");
    pollfd p{fd, event, 0};
    int n = poll(&p, 1, static_cast<int>(ms));
    if (n < 0 && errno == EINTR) continue;
    require(n > 0, "connection/status timeout");
    require((p.revents & event) != 0, "socket closed or unavailable");
    return;
  }
}
inline Bytes read_bytes(int fd, size_t n) {
  require(n <= max_message, "WebSocket message exceeds diagnostic limit");
  Bytes b(n);
  size_t pos = 0;
  while (pos < n) {
    wait_socket(fd, POLLIN);
    ssize_t got = recv(fd, b.data() + pos, n - pos, 0);
    if (got < 0 && (errno == EINTR || errno == EAGAIN)) continue;
    require(got > 0, "peer closed connection");
    pos += static_cast<size_t>(got);
  }
  return b;
}
inline void write_bytes(int fd, const Bytes& b) {
  size_t pos = 0;
  while (pos < b.size()) {
    wait_socket(fd, POLLOUT);
    ssize_t n = send(fd, b.data() + pos, b.size() - pos, 0);
    if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
    require(n > 0, "socket write failed");
    pos += static_cast<size_t>(n);
  }
}
inline Bytes random_bytes(size_t n) {
  int fd = open("/dev/urandom", O_RDONLY);
  require(fd >= 0, "cannot open random source");
  Bytes b(n);
  size_t pos = 0;
  while (pos < n) {
    ssize_t got = read(fd, b.data() + pos, n - pos);
    if (got < 0 && errno == EINTR) continue;
    if (got <= 0) { close(fd); throw std::runtime_error("random source failed"); }
    pos += static_cast<size_t>(got);
  }
  close(fd);
  return b;
}
inline std::string base64(const Bytes& b) {
  static const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string s;
  for (size_t i = 0; i < b.size(); i += 3) {
    uint32_t v = uint32_t(b[i]) << 16;
    if (i + 1 < b.size()) v |= uint32_t(b[i + 1]) << 8;
    if (i + 2 < b.size()) v |= b[i + 2];
    s += alphabet[(v >> 18) & 63]; s += alphabet[(v >> 12) & 63];
    s += i + 1 < b.size() ? alphabet[(v >> 6) & 63] : '=';
    s += i + 2 < b.size() ? alphabet[v & 63] : '=';
  }
  return s;
}
inline uint32_t rotate(uint32_t v, unsigned n) { return (v << n) | (v >> (32 - n)); }
// SHA-1 is used only to validate the RFC 6455 upgrade response, not for secrets.
inline Bytes sha1(const std::string& text) {
  Bytes b(text.begin(), text.end());
  uint64_t bits = uint64_t(b.size()) * 8;
  b.push_back(128);
  while (b.size() % 64 != 56) b.push_back(0);
  for (int i = 7; i >= 0; --i) b.push_back(uint8_t(bits >> (8 * i)));
  std::array<uint32_t, 5> h{0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};
  for (size_t p = 0; p < b.size(); p += 64) {
    uint32_t w[80];
    for (unsigned i = 0; i < 16; ++i)
      w[i] = (uint32_t(b[p+4*i])<<24) | (uint32_t(b[p+4*i+1])<<16) |
             (uint32_t(b[p+4*i+2])<<8) | b[p+4*i+3];
    for (unsigned i = 16; i < 80; ++i) w[i] = rotate(w[i-3]^w[i-8]^w[i-14]^w[i-16], 1);
    uint32_t a=h[0], c=h[2], d=h[3], e=h[4], bb=h[1];
    for (unsigned i = 0; i < 80; ++i) {
      uint32_t f, k;
      if (i < 20) { f=(bb&c)|((~bb)&d); k=0x5a827999; }
      else if (i < 40) { f=bb^c^d; k=0x6ed9eba1; }
      else if (i < 60) { f=(bb&c)|(bb&d)|(c&d); k=0x8f1bbcdc; }
      else { f=bb^c^d; k=0xca62c1d6; }
      uint32_t t=rotate(a,5)+f+e+k+w[i]; e=d; d=c; c=rotate(bb,30); bb=a; a=t;
    }
    h[0]+=a; h[1]+=bb; h[2]+=c; h[3]+=d; h[4]+=e;
  }
  Bytes out;
  for (auto v : h) for (int i=3; i>=0; --i) out.push_back(uint8_t(v >> (8*i)));
  return out;
}
inline std::string lower(std::string s) {
  for (char& c : s) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
  return s;
}
inline std::string trim(std::string s) {
  size_t a=s.find_first_not_of(" \t\r\n"), b=s.find_last_not_of(" \t\r\n");
  return a == std::string::npos ? "" : s.substr(a, b-a+1);
}
inline void upgrade(int fd, const std::string& host, int port) {
  std::string key=base64(random_bytes(16));
  std::string req="GET / HTTP/1.1\r\nHost: "+host+":"+std::to_string(port)+
    "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: "+key+"\r\n\r\n";
  write_bytes(fd, Bytes(req.begin(),req.end()));
  std::string response;
  while (response.size() < 8192 && (response.size()<4 || response.substr(response.size()-4)!="\r\n\r\n"))
    response += char(read_bytes(fd,1)[0]);
  size_t pos=response.find("\r\n");
  require(pos != std::string::npos && (response.substr(0,pos)=="HTTP/1.1 101 Switching Protocols" ||
          response.substr(0,pos)=="HTTP/1.1 101"), "WebSocket upgrade refused");
  std::map<std::string,std::string> headers;
  for (size_t start=pos+2; start<response.size(); start=pos+2) {
    pos=response.find("\r\n",start);
    require(pos!=std::string::npos,"incomplete HTTP headers");
    if (pos==start) break;
    size_t colon=response.find(':',start);
    require(colon<pos,"invalid HTTP header");
    headers[lower(response.substr(start,colon-start))]=trim(response.substr(colon+1,pos-colon-1));
  }
  require(lower(headers["upgrade"])=="websocket", "missing WebSocket upgrade header");
  require(lower(headers["connection"]).find("upgrade")!=std::string::npos, "missing connection upgrade header");
  require(headers["sec-websocket-accept"]==base64(sha1(key+"258EAFA5-E914-47DA-95CA-C5AB0DC85B11")),
          "invalid WebSocket accept proof");
}
inline void send_frame(int fd, const Bytes& payload, uint8_t opcode=2) {
  require(payload.size()<=max_message,"outbound frame too large");
  Bytes out{uint8_t(0x80|opcode)};
  if (payload.size()<126) out.push_back(uint8_t(0x80|payload.size()));
  else if (payload.size()<=65535) {
    out.push_back(0x80|126); out.push_back(uint8_t(payload.size()>>8)); out.push_back(uint8_t(payload.size()));
  } else throw std::runtime_error("outbound frame too large");
  Bytes mask=random_bytes(4); out.insert(out.end(),mask.begin(),mask.end());
  for (size_t i=0;i<payload.size();++i) out.push_back(payload[i]^mask[i%4]);
  write_bytes(fd,out);
}
inline Bytes receive_message(int fd, size_t limit=65536) {
  Bytes message;
  bool fragmented=false;
  for (;;) {
    Bytes h=read_bytes(fd,2);
    bool fin=(h[0]&128)!=0;
    uint8_t opcode=h[0]&15;
    require(!(h[0]&0x70) && !(h[1]&128), "invalid server WebSocket flags");
    uint64_t n=h[1]&127;
    if (n==126) { auto b=read_bytes(fd,2); n=(uint64_t(b[0])<<8)|b[1]; }
    else if (n==127) { auto b=read_bytes(fd,8); n=0; for(auto v:b) n=(n<<8)|v; }
    require(n<=limit && message.size()+n<=limit,"inbound frame too large");
    if (opcode>=8) require(fin && n<=125,"invalid control frame");
    Bytes b=read_bytes(fd,static_cast<size_t>(n));
    if (opcode==8) throw std::runtime_error("robot closed WebSocket connection");
    if (opcode==9) { send_frame(fd,b,10); continue; }
    if (opcode==10) continue;
    require((opcode==2 && !fragmented) || (opcode==0 && fragmented), "unexpected WebSocket opcode");
    message.insert(message.end(),b.begin(),b.end());
    if (fin) return message;
    fragmented=true;
  }
}
struct Field { unsigned number, type; uint64_t value; Bytes bytes; };
inline uint64_t varint(const Bytes& b,size_t& p) {
  uint64_t v=0;
  for (unsigned shift=0;shift<70;shift+=7) {
    require(p<b.size(),"truncated protobuf varint"); uint8_t c=b[p++];
    require(shift<63 || c<=1,"overflowed protobuf varint");
    v|=uint64_t(c&127)<<shift;
    if (!(c&128)) return v;
  }
  throw std::runtime_error("invalid protobuf varint");
}
inline std::vector<Field> decode(const Bytes& b) {
  std::vector<Field> fields;
  size_t p=0;
  while (p<b.size()) {
    uint64_t tag=varint(b,p); require(tag>>3 && tag>>3<=0x1fffffff,"invalid protobuf field number");
    Field f{unsigned(tag>>3),unsigned(tag&7),0,{}};
    if (f.type==0) f.value=varint(b,p);
    else {
      size_t n;
      if (f.type==1) n=8;
      else if (f.type==5) n=4;
      else if (f.type==2) {
        uint64_t len=varint(b,p); require(len<=b.size()-p,"truncated protobuf bytes"); n=size_t(len);
      } else throw std::runtime_error("unsupported protobuf wire type");
      require(n<=b.size()-p,"truncated protobuf field");
      f.bytes.assign(b.begin()+p,b.begin()+p+n); p+=n;
    }
    fields.push_back(f);
  }
  return fields;
}
inline std::string text_field(const std::vector<Field>& fields,unsigned number) {
  for (const auto& f:fields) if(f.number==number && f.type==2)
    return std::string(f.bytes.begin(),f.bytes.end());
  return "";
}
inline void request(int fd,const std::string& prefix,const std::string& command,const Bytes& payload={}) {
  // All callers below are identity/status queries or short passive subscriptions.
  require(command=="common/get_device_info" || command=="status/get_device_base_status" ||
          command=="common/notify_app_event" || command=="common/active_robot_publish",
          "command outside diagnostic allowlist");
  std::string topic=prefix+"/"+command;
  require(topic.size()<=253,"topic too long");
  Bytes data{1,uint8_t(topic.size()+2),0x22,uint8_t(topic.size())};
  data.insert(data.end(),topic.begin(),topic.end()); data.insert(data.end(),payload.begin(),payload.end());
  send_frame(fd,data);
}
inline int integer(const char* s,int low,int high) {
  char* end=nullptr; long n=strtol(s,&end,10);
  require(s[0] && end && !*end && n>=low && n<=high,"invalid numeric argument");
  return int(n);
}
inline bool valid_identity(const std::string& id) {
  return id.size()==32 && id.find_first_not_of("0123456789abcdefABCDEF")==std::string::npos;
}
inline std::string read_identity(const std::string& path) {
  if(path.empty()) return "";
  FILE* f=fopen(path.c_str(),"rb");
  if(!f && errno==ENOENT) return "";
  require(f!=nullptr,"cannot read identity cache");
  char data[66]; size_t n=fread(data,1,sizeof(data),f); bool ok=!ferror(f); fclose(f);
  require(ok && n<sizeof(data),"invalid identity cache");
  std::string id=trim(std::string(data,n));
  require(valid_identity(id),"invalid identity cache");
  return id;
}
inline void save_identity(const std::string& path,const std::string& id) {
  if(path.empty()) return;
  std::string tmp=path+".tmp."+std::to_string(getpid());
  int fd=open(tmp.c_str(),O_WRONLY|O_CREAT|O_EXCL,0600);
  require(fd>=0,"cannot create identity cache");
  std::string text=id+"\n"; size_t p=0; bool ok=true;
  while(p<text.size()) {
    ssize_t n=write(fd,text.data()+p,text.size()-p);
    if(n<0 && errno==EINTR) continue;
    if(n<=0) { ok=false; break; }
    p+=size_t(n);
  }
  if(fsync(fd)!=0) ok=false;
  if(close(fd)!=0) ok=false;
  if(ok && rename(tmp.c_str(),path.c_str())==0) return;
  unlink(tmp.c_str());
  throw std::runtime_error("cannot save identity cache");
}

inline uint64_t number_field(const std::vector<Field>& fields,unsigned number,uint64_t fallback=0) {
  for(const auto& f:fields) if(f.number==number && f.type==0) return f.value;
  return fallback;
}
inline Bytes bytes_field(const std::vector<Field>& fields,unsigned number) {
  for(const auto& f:fields) if(f.number==number && f.type==2) return f.bytes;
  return {};
}
inline void append(Bytes& out,const Bytes& part) { out.insert(out.end(),part.begin(),part.end()); }
inline Bytes encode_varint(uint64_t v) {
  Bytes out;
  do { uint8_t b=v&127; v>>=7; out.push_back(uint8_t(b|(v ? 128 : 0))); } while(v);
  return out;
}
inline Bytes encode_number(unsigned field,uint64_t v) {
  Bytes out=encode_varint(uint64_t(field)<<3); append(out,encode_varint(v)); return out;
}
inline Bytes encode_bytes(unsigned field,const Bytes& b) {
  Bytes out=encode_varint((uint64_t(field)<<3)|2); append(out,encode_varint(b.size())); append(out,b); return out;
}
enum class State { Unknown, Idle, Docked, Cleaning, Paused, Returning, Mapping, Fault };
struct Status {
  int battery=-1; State state=State::Unknown;
  bool has_position=false;
  double robot_x=0,robot_y=0,progress=-1,cleaned_area=-1;
  uint64_t elapsed_seconds=0;
};
inline const char* state_label(State state) {
  switch(state) {
    case State::Idle: return "Idle";
    case State::Docked: return "Docked";
    case State::Cleaning: return "Cleaning";
    case State::Paused: return "Paused";
    case State::Returning: return "Returning";
    case State::Mapping: return "Mapping";
    case State::Fault: return "Needs attention";
    default: return "Status unknown";
  }
}
inline Status decode_status(const Bytes& data) {
  auto fields=decode(data); Status status;
  for(const auto& f:fields) if(f.number==2 && f.type==5) {
    uint32_t bits=uint32_t(f.bytes[0])|(uint32_t(f.bytes[1])<<8)|(uint32_t(f.bytes[2])<<16)|(uint32_t(f.bytes[3])<<24);
    float battery; memcpy(&battery,&bits,4);
    require(std::isfinite(battery) && battery>=0 && battery<=100,"invalid battery reading");
    status.battery=int(std::lround(battery));
  }
  require(status.battery>=0,"battery data missing");
  auto task=decode(bytes_field(fields,3));
  int code=int(number_field(task,1));
  if(number_field(task,2)==1) status.state=State::Paused;
  else if(number_field(task,7)==1 || code==19) status.state=State::Returning;
  else if(code==1) status.state=State::Idle;
  else if(code==2 && number_field(task,4)==3) status.state=State::Cleaning;
  else if(code==2 || code==10 || code==14) status.state=State::Docked;
  else if(code==3 || code==4 || code==5 || code==17) status.state=State::Cleaning;
  else if(code==7) status.state=State::Mapping;
  for(const auto& f:fields) if(f.number==1) {
    if(f.type==0 && f.value!=0) status.state=State::Fault;
    if(f.type==2) {
      size_t p=0;
      while(p<f.bytes.size()) if(varint(f.bytes,p)!=0) status.state=State::Fault;
    }
  }
  return status;
}
struct Room { uint64_t id=0; std::string name; double cx=0,cy=0; size_t cells=0; };
struct Map {
  uint64_t id=0;
  std::vector<uint64_t> rooms;
  std::vector<Room> details;
  int width=0,height=0,min_x=0,min_y=0,max_x=0,max_y=0,origin_x=0,origin_y=0;
  std::vector<uint32_t> pixels;
  bool has_dock=false;
  double dock_x=0,dock_y=0;
};
inline double float_field(const std::vector<Field>& fields,unsigned number,double fallback=0) {
  for(const auto& f:fields) if(f.number==number) {
    if(f.type==5) {
      uint32_t v=uint32_t(f.bytes[0])|(uint32_t(f.bytes[1])<<8)|(uint32_t(f.bytes[2])<<16)|(uint32_t(f.bytes[3])<<24);
      float n; memcpy(&n,&v,4); return std::isfinite(n) ? n : fallback;
    }
    if(f.type==0) return double(static_cast<int64_t>(f.value));
  }
  return fallback;
}
inline Bytes inflate_grid(const Bytes& compressed,size_t cells) {
  require(cells>0 && cells<=2000000 && !compressed.empty(),"invalid map grid size");
  if(compressed[0]==0x0a) return compressed; // already a protobuf pixel buffer
  Bytes out(cells*5+64);
  size_t n=tinfl_decompress_mem_to_mem(out.data(),out.size(),compressed.data(),compressed.size(),TINFL_FLAG_PARSE_ZLIB_HEADER);
  if(n==TINFL_DECOMPRESS_MEM_TO_MEM_FAILED)
    n=tinfl_decompress_mem_to_mem(out.data(),out.size(),compressed.data(),compressed.size(),0);
  require(n!=TINFL_DECOMPRESS_MEM_TO_MEM_FAILED,"map grid decompression failed");
  out.resize(n); return out;
}
inline Map decode_map(const Bytes& payload) {
  Map map; auto fields=decode(payload); map.id=number_field(fields,1);
  static const char* types[]={"Room","Master bedroom","Bedroom","Living room","Kitchen","Bathroom","Toilet","Balcony","Dining room","Closet","Corridor","Study","Kids room","Entertainment","Storage","Other"};
  for(const auto& f:fields) if(f.number==12 && f.type==2) {
    auto info=decode(f.bytes); uint64_t room=number_field(info,1);
    if(room && std::find(map.rooms.begin(),map.rooms.end(),room)==map.rooms.end()) {
      map.rooms.push_back(room); Room detail; detail.id=room; detail.name=text_field(info,3);
      if(detail.name.empty()) {
        auto type=number_field(info,2); detail.name=type<16 ? types[type] : "Room";
        auto index=number_field(info,8); if(index>1) detail.name+=" "+std::to_string(index);
      }
      if(detail.name.size()>120) detail.name.resize(120);
      map.details.push_back(detail);
    }
  }
  require(map.id>0 && !map.rooms.empty() && map.rooms.size()<=64,"current map has no cleanable rooms");
  auto width=number_field(fields,4),height=number_field(fields,5);
  require(width<=2048 && height<=2048,"map exceeds display limits");
  map.width=int(width); map.height=int(height);
  auto compressed=bytes_field(fields,17);
  if(map.width>0 && map.height>0 && !compressed.empty()) {
    require(map.width<=2048 && map.height<=2048 && size_t(map.width)*map.height<=2000000,"map exceeds display limits");
    auto packed=bytes_field(decode(inflate_grid(compressed,size_t(map.width)*map.height)),1);
    map.pixels.reserve(size_t(map.width)*map.height); size_t pos=0;
    while(pos<packed.size() && map.pixels.size()<size_t(map.width)*map.height) {
      uint64_t value=varint(packed,pos); require(value<=UINT32_MAX,"invalid map pixel");
      map.pixels.push_back(uint32_t(value));
    }
    require(map.pixels.size()==size_t(map.width)*map.height && pos==packed.size(),"map pixel count mismatch");
    map.min_x=map.width; map.min_y=map.height;
    for(int y=0;y<map.height;++y) for(int x=0;x<map.width;++x) {
      uint32_t value=map.pixels[size_t(y)*map.width+x]; if(!value) continue;
      map.min_x=std::min(map.min_x,x); map.min_y=std::min(map.min_y,y);
      map.max_x=std::max(map.max_x,x); map.max_y=std::max(map.max_y,y);
      for(auto& room:map.details) if(room.id==(value>>8) && !(value&0x10)) {
        room.cx+=x; room.cy+=y; ++room.cells; break;
      }
    }
    for(auto& room:map.details) if(room.cells) { room.cx/=room.cells; room.cy/=room.cells; }
    auto origin=decode(bytes_field(fields,6));
    map.origin_x=int(static_cast<int64_t>(number_field(origin,3)));
    map.origin_y=int(static_cast<int64_t>(number_field(origin,1)));
    auto dock=decode(bytes_field(decode(bytes_field(fields,8)),1));
    if(!dock.empty()) {
      map.dock_x=float_field(dock,1)-map.origin_x; map.dock_y=float_field(dock,2)-map.origin_y;
      map.has_dock=map.dock_x>=0 && map.dock_x<map.width && map.dock_y>=0 && map.dock_y<map.height;
    }
  }
  return map;
}
struct CleanOptions {
  int mode=1,fan=2,water=2,scrub=1,passes=1,coverage=1;
  bool rooms_only=false;
  uint64_t map_id=0;
  std::vector<uint64_t> rooms;
};
inline const char* mode_label(int mode) {
  static const char* names[]={"","Vacuum","Mop","Vacuum then mop","Vacuum and mop"};
  return mode>=1 && mode<=4 ? names[mode] : "Vacuum";
}
inline Bytes clean_task(const Map& map,const CleanOptions& options={}) {
  require(map.id>0 && !map.rooms.empty() && map.rooms.size()<=64,"current map has no cleanable rooms");
  require(options.mode>=1 && options.mode<=4 && options.fan>=1 && options.fan<=4 && options.water>=1 && options.water<=3 &&
          options.scrub>=1 && options.scrub<=2 && options.passes>=1 && options.passes<=3 && options.coverage>=1 && options.coverage<=2,"invalid cleaning options");
  const auto& rooms=options.rooms_only ? options.rooms : map.rooms;
  require(!rooms.empty() && rooms.size()<=64,"Select at least one room");
  if(options.rooms_only) require(options.map_id==map.id,"Map changed; select rooms again");
  Bytes task=encode_number(1,map.id); unsigned order=1;
  for(auto room:rooms) {
    require(room>0 && std::find(map.rooms.begin(),map.rooms.end(),room)!=map.rooms.end(),"Selected room is not on current map");
    Bytes zone=encode_number(1,1); append(zone,encode_number(2,room));
    static const int modes[]={0,2,3,5,4};
    Bytes params=encode_number(1,modes[options.mode]); append(params,encode_number(2,options.fan));
    append(params,encode_number(3,options.scrub)); append(params,encode_number(4,options.water));
    if(options.mode==1 || options.mode==3) append(params,encode_number(5,options.passes));
    if(options.mode==2 || options.mode==3) append(params,encode_number(6,options.passes));
    if(options.mode==4) append(params,encode_number(7,options.passes));
    append(params,encode_number(8,options.coverage));
    Bytes item=encode_bytes(1,zone); append(item,encode_bytes(2,params)); append(item,encode_number(3,order++));
    append(task,encode_bytes(2,item));
  }
  append(task,encode_bytes(3,{})); append(task,encode_number(5,options.mode));
  return encode_bytes(1,task);
}
inline Bytes vacuum_task(const Map& map) { return clean_task(map); }
enum class Command { Refresh, Start, Pause, Resume, Dock };
inline bool can_run(Command command,State state) {
  if(command==Command::Refresh) return true;
  if(command==Command::Start) return state==State::Idle || state==State::Docked;
  if(command==Command::Pause) return state==State::Cleaning;
  if(command==Command::Resume) return state==State::Paused;
  if(command==Command::Dock) return state!=State::Docked && state!=State::Unknown && state!=State::Mapping;
  return false;
}
inline const char* accepted_label(Command command) {
  switch(command) {
    case Command::Start: return "Cleaning start accepted";
    case Command::Pause: return "Pause accepted";
    case Command::Resume: return "Resume accepted";
    case Command::Dock: return "Return to dock accepted";
    default: return "Status refreshed";
  }
}
class Client {
  int fd_=-1;
  std::string prefix_;
  Status telemetry_;
  void observe(const Bytes& b) {
    if(b.size()<4 || b[2]!=0x22 || b.size()<size_t(4+b[3])) return;
    std::string topic(b.begin()+4,b.begin()+4+b[3]);
    if(topic.rfind(prefix_+"/",0)!=0) return;
    auto fields=decode(Bytes(b.begin()+4+b[3],b.end()));
    if(topic==prefix_+"/map/display_map") {
      auto position=decode(bytes_field(decode(bytes_field(fields,1)),1));
      if(!position.empty() && number_field(fields,10)>0) {
        telemetry_.robot_x=float_field(position,1,NAN); telemetry_.robot_y=float_field(position,2,NAN);
        telemetry_.has_position=std::isfinite(telemetry_.robot_x) && std::isfinite(telemetry_.robot_y);
      }
    } else if(topic==prefix_+"/status/working_status") {
      double progress=float_field(fields,1,-1);
      if(progress>=0 && progress<=1) telemetry_.progress=progress;
      telemetry_.cleaned_area=float_field(fields,2,-1); telemetry_.elapsed_seconds=number_field(fields,3);
    }
  }
 public:
  Client(const std::string& host,const std::string& identity,int port=9002) {
    require(valid_identity(identity),"vacuum identity missing"); prefix_="/CNbforyZWI/"+identity;
    sockaddr_in addr{}; addr.sin_family=AF_INET; addr.sin_port=htons(uint16_t(port));
    require(port>0 && port<=65535 && inet_pton(AF_INET,host.c_str(),&addr.sin_addr)==1,"invalid vacuum address");
    deadline=Clock::now()+std::chrono::seconds(4);
    fd_=socket(AF_INET,SOCK_STREAM,0); require(fd_>=0,"cannot create vacuum socket");
    try {
      int flags=fcntl(fd_,F_GETFL,0); require(flags>=0 && fcntl(fd_,F_SETFL,flags|O_NONBLOCK)==0,"cannot set socket flags");
#ifdef SO_NOSIGPIPE
      int no_sigpipe=1; setsockopt(fd_,SOL_SOCKET,SO_NOSIGPIPE,&no_sigpipe,sizeof(no_sigpipe));
#endif
      int rc=connect(fd_,reinterpret_cast<sockaddr*>(&addr),sizeof(addr));
      require(rc==0 || errno==EINPROGRESS,"vacuum connection refused");
      if(rc!=0) {
        wait_socket(fd_,POLLOUT); int error=0; socklen_t size=sizeof(error);
        require(getsockopt(fd_,SOL_SOCKET,SO_ERROR,&error,&size)==0 && error==0,"vacuum connection failed");
      }
      upgrade(fd_,host,port);
    } catch(...) { close(fd_); fd_=-1; throw; }
  }
  Client(const Client&)=delete;
  Client& operator=(const Client&)=delete;
  ~Client() { if(fd_>=0) close(fd_); }
  // Exactly one outstanding request. A timeout discards the entire session;
  // action commands are never retried, so a late ACK cannot repeat a clean.
  std::vector<Field> transact(const std::string& topic,const Bytes& payload={},size_t limit=65536) {
    deadline=Clock::now()+std::chrono::seconds(topic=="map/get_map" ? 8 : 4);
    std::string full=prefix_+"/"+topic;
    require(full.size()<=253,"topic too long");
    Bytes data{1,uint8_t(full.size()+2),0x22,uint8_t(full.size())};
    data.insert(data.end(),full.begin(),full.end()); append(data,payload); send_frame(fd_,data);
    for(;;) {
      Bytes b=receive_message(fd_,limit);
      require(b.size()>=4 && b[0]==1 && (b[2]==0x22 || b[2]==0x2a) &&
              b[1]==unsigned(b[3])+2 && b.size()>=size_t(4+b[3]),"invalid Narwal response");
      if(b[2]!=0x2a) { observe(b); continue; }
      return decode(Bytes(b.begin()+4+b[3],b.end()));
    }
  }
  static void accepted(const std::vector<Field>& fields) {
    auto code=number_field(fields,1,0);
    if(code==2) throw std::runtime_error("Vacuum is not ready");
    if(code==3) throw std::runtime_error("Vacuum is already busy");
    require(code==1,"Vacuum refused the request");
  }
  Status status() {
    auto response=transact("status/get_device_base_status"); accepted(response);
    return decode_status(bytes_field(response,2));
  }
  Map map() {
    auto response=transact("map/get_map",{},512*1024); accepted(response);
    return decode_map(bytes_field(response,2));
  }
  Status telemetry() {
    Bytes subscription;
    for(const char* topic:{"map/display_map","status/working_status"}) {
      std::string text=topic;
      Bytes item=encode_bytes(1,Bytes(text.begin(),text.end())); append(item,encode_number(2,8));
      append(subscription,encode_bytes(1,item));
    }
    // Firmware variants accept different passive subscription forms. This is
    // the last operation in a session, so optional/late ACKs cannot be mistaken
    // for an appliance command response. A quiet docked robot may send nothing.
    try {
      auto response=transact("common/active_robot_publish",subscription);
      if(number_field(response,1)!=1) transact("common/active_robot_publish",encode_number(1,30));
    } catch(const std::exception&) { return telemetry_; }
    deadline=Clock::now()+std::chrono::milliseconds(1700);
    while(Clock::now()<deadline) {
      auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-Clock::now()).count();
      pollfd p{fd_,POLLIN,0}; int rc=poll(&p,1,int(std::max<int64_t>(1,ms)));
      if(rc<0 && errno==EINTR) continue;
      require(rc>=0,"telemetry poll failed"); if(!rc) break;
      observe(receive_message(fd_,512*1024));
    }
    return telemetry_;
  }
  void command(Command command,const Status& fresh,const CleanOptions& options={}) {
    require(can_run(command,fresh.state),"Control unavailable in current state");
    if(command==Command::Refresh) return;
    std::string topic; Bytes payload;
    switch(command) {
      case Command::Start: payload=clean_task(map(),options); topic="clean/start_clean"; break;
      case Command::Pause: topic="task/pause"; break;
      case Command::Resume: topic="task/resume"; break;
      case Command::Dock: topic="supply/recall"; break;
      default: throw std::runtime_error("Unknown vacuum control");
    }
    accepted(transact(topic,payload));
  }
};

} // namespace narwal
