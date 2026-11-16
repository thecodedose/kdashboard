// Read-only X10 Pro LAN diagnostic. No cleaning, dock, map or settings commands.
// Protocol reference: https://github.com/sjmotew/NarwalIntegration/blob/master/docs/PROTOCOL.md
#include "narwal_client.hpp"
using namespace narwal;

static bool print_status(const std::vector<Field>& fields) {
  for(const auto& f:fields) if(f.number==2 && f.type==5) {
    uint32_t raw=uint32_t(f.bytes[0])|(uint32_t(f.bytes[1])<<8)|(uint32_t(f.bytes[2])<<16)|(uint32_t(f.bytes[3])<<24);
    float battery; memcpy(&battery,&raw,4);
    require(std::isfinite(battery) && battery>=0 && battery<=100,"invalid battery reading");
    puts("Model: Narwal Freo X10 Pro (AX15)");
    printf("Battery: %.0f%%\n",double(battery));
    puts("RESULT: live status received directly over local Wi-Fi");
    return true;
  }
  return false;
}
int main(int argc,char** argv) {
  setvbuf(stdout,nullptr,_IOLBF,0);
  std::signal(SIGPIPE,SIG_IGN);
  int fd=-1;
  bool connected=false,identified=false;
  try {
    require(argc>=2,"Usage: narwal-probe VACUUM_IPV4 [--port PORT] [--timeout SECONDS] [--identity-file PATH]");
    std::string host=argv[1],identity_file; int port=9002,seconds=20;
    for (int i=2;i<argc;++i) {
      require(i+1<argc,"missing option value"); std::string option=argv[i];
      if(option=="--port") port=integer(argv[++i],1,65535);
      else if(option=="--timeout") seconds=integer(argv[++i],1,60);
      else if(option=="--identity-file") identity_file=argv[++i];
      else throw std::runtime_error("unknown option");
    }
    std::string device_id=read_identity(identity_file);
    sockaddr_in addr{}; addr.sin_family=AF_INET; addr.sin_port=htons(uint16_t(port));
    require(inet_pton(AF_INET,host.c_str(),&addr.sin_addr)==1,"host must be an IPv4 address");
    deadline=Clock::now()+std::chrono::seconds(seconds);
    fd=socket(AF_INET,SOCK_STREAM,0); require(fd>=0,"cannot create socket");
    int flags=fcntl(fd,F_GETFL,0); require(flags>=0 && fcntl(fd,F_SETFL,flags|O_NONBLOCK)==0,"cannot set socket flags");
#ifdef SO_NOSIGPIPE
    int no_sigpipe=1; setsockopt(fd,SOL_SOCKET,SO_NOSIGPIPE,&no_sigpipe,sizeof(no_sigpipe));
#endif
    int rc=connect(fd,reinterpret_cast<sockaddr*>(&addr),sizeof(addr));
    require(rc==0 || errno==EINPROGRESS,"TCP connection refused");
    if(rc!=0) {
      wait_socket(fd,POLLOUT); int error=0; socklen_t size=sizeof(error);
      require(getsockopt(fd,SOL_SOCKET,SO_ERROR,&error,&size)==0 && error==0,"TCP connection failed");
    }
    puts("TCP: connected"); upgrade(fd,host,port); connected=true;
    puts("WebSocket: verified");
    std::string prefix="/CNbforyZWI/"+device_id;
    identified=!device_id.empty();
    auto remember=[&](const std::string& id) {
      require(valid_identity(id),"invalid device identity");
      if(id!=device_id) { save_identity(identity_file,id); device_id=id; }
      identified=true; prefix="/CNbforyZWI/"+id;
    };
    request(fd,prefix,"common/get_device_info");
    bool status_requested=identified;
    if(identified) request(fd,prefix,"status/get_device_base_status");
    auto retry=Clock::now()+std::chrono::seconds(3);
    while(Clock::now()<deadline) {
      if(Clock::now()>=retry) {
        request(fd,prefix,"common/get_device_info");
        request(fd,prefix,"common/notify_app_event",{8,1});
        request(fd,prefix,"common/active_robot_publish",{8,30});
        request(fd,prefix,"status/get_device_base_status");
        status_requested=true;
        retry=Clock::now()+std::chrono::seconds(3);
      }
      pollfd p{fd,POLLIN,0};
      int ready=poll(&p,1,200);
      if(ready<0 && errno==EINTR) continue;
      require(ready>=0,"socket poll failed");
      if(!ready) continue;
      Bytes b=receive_message(fd);
      require(b.size()>=4 && b[0]==1 && (b[2]==0x22 || b[2]==0x2a) &&
              b[1]==unsigned(b[3])+2 && b.size()>=size_t(4+b[3]),"invalid Narwal frame");
      std::string topic(b.begin()+4,b.begin()+4+b[3]);
      Bytes payload(b.begin()+4+b[3],b.end());
      auto fields=decode(payload);
      if(b[2]==0x2a) {
        auto key=text_field(fields,1),id=text_field(fields,2);
        if(key=="CNbforyZWI" && valid_identity(id)) {
          remember(id);
          auto fw=text_field(fields,3);
          // Avoid logging device IDs, account IDs, arbitrary strings or full payloads.
          if(!fw.empty() && fw.size()<40 && fw.find_first_not_of("vV0123456789.-_")==std::string::npos)
            printf("Firmware: %s\n",fw.c_str());
          request(fd,prefix,"status/get_device_base_status");
          status_requested=true;
        } else if(identified && status_requested) {
          // Query responses wrap RobotBaseStatus in protobuf field 2.
          // Only unwrap an accepted status response, never an identity reply.
          bool accepted=false;
          for(const auto& f:fields) if(f.number==1 && f.type==0 && f.value==1) accepted=true;
          if(accepted) for(const auto& f:fields) if(f.number==2 && f.type==2) {
            if(print_status(decode(f.bytes))) { send_frame(fd,{3,232},8); close(fd); return 0; }
          }
        }
      } else if(topic.rfind("/CNbforyZWI/",0)==0) {
        size_t slash=topic.find('/',12);
        if(slash!=std::string::npos && valid_identity(topic.substr(12,slash-12))) {
          remember(topic.substr(12,slash-12));
        }
      }
      if(identified && topic.size()>=25 && topic.substr(topic.size()-25)=="/status/robot_base_status") {
        if(print_status(fields)) {
          send_frame(fd,{3,232},8); close(fd); return 0;
        }
      }
    }
    throw std::runtime_error("no live X10 Pro status before timeout");
  } catch(const std::exception& e) {
    if(fd>=0) close(fd);
    fprintf(stderr,"RESULT: %s; %s\n",connected ? "WebSocket connected, live status unconfirmed" : "connection failed",e.what());
    return connected ? 2 : 1;
  }
}
