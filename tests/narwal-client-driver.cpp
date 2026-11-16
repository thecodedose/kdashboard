#include "../kindle/native/src/narwal_client.hpp"

int main(int argc,char** argv) {
  signal(SIGPIPE,SIG_IGN);
  try {
    narwal::require(argc==5 || argc==6,"host port identity-file command [options] required");
    std::string action=argv[4]; narwal::Command command=narwal::Command::Refresh;
    if(action=="start") command=narwal::Command::Start;
    else if(action=="pause") command=narwal::Command::Pause;
    else if(action=="resume") command=narwal::Command::Resume;
    else if(action=="dock") command=narwal::Command::Dock;
    else narwal::require(action=="status" || action=="map" || action=="telemetry","unknown action");
    // Appliance commands from this test driver are limited to the mock peer.
    narwal::require(command==narwal::Command::Refresh || strcmp(argv[1],"127.0.0.1")==0,"action tests require loopback");
    narwal::Client client(argv[1],narwal::read_identity(argv[3]),narwal::integer(argv[2],1,65535));
    auto status=client.status();
    printf("battery=%d state=%s\n",status.battery,narwal::state_label(status.state));
    if(action=="map") {
      auto map=client.map(); auto task=narwal::vacuum_task(map);
      printf("Active map: %zu rooms; vacuum-only task encoded (%zu bytes); no action sent\n",map.rooms.size(),task.size());
    }
    if(action=="telemetry") {
      auto live=client.telemetry();
      printf("Optional live telemetry: position=%d progress_available=%d\n",live.has_position,live.progress>=0);
    }
    narwal::CleanOptions options;
    if(argc==6) {
      int room=0,map_id=0,end=0;
      narwal::require(sscanf(argv[5],"%d,%d,%d,%d,%d,%d,%d,%d%n",&options.mode,&options.fan,&options.water,&options.scrub,&options.passes,&options.coverage,&room,&map_id,&end)==8 && !argv[5][end],"invalid test options");
      if(room) { options.rooms_only=true; options.rooms={uint64_t(room)}; options.map_id=uint64_t(map_id); }
    }
    if(command!=narwal::Command::Refresh) client.command(command,status,options);
    return 0;
  } catch(const std::exception& e) { fprintf(stderr,"%s\n",e.what()); return 1; }
}
