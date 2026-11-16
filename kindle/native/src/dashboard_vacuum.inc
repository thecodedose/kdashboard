// One worker owns all local vacuum sessions; rendering and touch never wait on Wi-Fi.
struct VacuumSnapshot {
  bool configured=false;
  bool online=false;
  bool busy=false;
  time_t start_pending_until=0;
  narwal::Status status;
  std::shared_ptr<const narwal::Map> map;
  narwal::CleanOptions plan;
  char notice[80] = "";
};
pthread_mutex_t g_vacuum_mutex=PTHREAD_MUTEX_INITIALIZER;
VacuumSnapshot g_vacuum;
bool g_vacuum_dirty=false;
bool g_vacuum_pending=false;
narwal::Command g_vacuum_command=narwal::Command::Refresh;
narwal::CleanOptions g_vacuum_pending_plan;
bool g_vacuum_detail_open=false;
char g_vacuum_host[64]="";
char g_vacuum_identity_file[256]="";

VacuumSnapshot vacuumSnapshot() {
  pthread_mutex_lock(&g_vacuum_mutex);
  auto snapshot=g_vacuum;
  pthread_mutex_unlock(&g_vacuum_mutex);
  return snapshot;
}
bool takeVacuumRepaint() {
  pthread_mutex_lock(&g_vacuum_mutex);
  bool repaint=g_vacuum_dirty; g_vacuum_dirty=false;
  pthread_mutex_unlock(&g_vacuum_mutex);
  return repaint;
}
bool queueVacuumCommand(narwal::Command command) {
  pthread_mutex_lock(&g_vacuum_mutex);
  bool allowed=g_vacuum.configured && !g_vacuum.busy &&
    (command!=narwal::Command::Start || (g_vacuum.map && (!g_vacuum.plan.rooms_only || !g_vacuum.plan.rooms.empty()))) &&
    (command!=narwal::Command::Start || time(nullptr)>=g_vacuum.start_pending_until) &&
    (command==narwal::Command::Refresh || (g_vacuum.online && narwal::can_run(command,g_vacuum.status.state)));
  if(allowed) {
    g_vacuum_command=command; g_vacuum_pending=true; g_vacuum.busy=true;
    g_vacuum_pending_plan=g_vacuum.plan;
    snprintf(g_vacuum.notice,sizeof(g_vacuum.notice),"%s",command==narwal::Command::Refresh ? "Refreshing..." : "Sending request...");
    g_vacuum_dirty=true;
  }
  pthread_mutex_unlock(&g_vacuum_mutex);
  return allowed;
}
void vacuumDetailOpen(bool open) {
  pthread_mutex_lock(&g_vacuum_mutex); g_vacuum_detail_open=open; pthread_mutex_unlock(&g_vacuum_mutex);
}
void vacuumWholeHome() {
  pthread_mutex_lock(&g_vacuum_mutex);
  if(!g_vacuum.busy) { g_vacuum.plan.rooms_only=false; g_vacuum.plan.rooms.clear(); g_vacuum_dirty=true; }
  pthread_mutex_unlock(&g_vacuum_mutex);
}
void toggleVacuumRoom(size_t index) {
  pthread_mutex_lock(&g_vacuum_mutex);
  if(!g_vacuum.busy && g_vacuum.map && index<g_vacuum.map->rooms.size()) {
    auto& plan=g_vacuum.plan;
    if(!plan.rooms_only) { plan.rooms_only=true; plan.rooms.clear(); }
    plan.map_id=g_vacuum.map->id;
    uint64_t id=g_vacuum.map->rooms[index];
    auto found=std::find(plan.rooms.begin(),plan.rooms.end(),id);
    if(found==plan.rooms.end()) plan.rooms.push_back(id); else plan.rooms.erase(found);
    g_vacuum_dirty=true;
  }
  pthread_mutex_unlock(&g_vacuum_mutex);
}
void setVacuumMode(int mode) {
  pthread_mutex_lock(&g_vacuum_mutex);
  if(!g_vacuum.busy && mode>=1 && mode<=4) { g_vacuum.plan.mode=mode; g_vacuum_dirty=true; }
  pthread_mutex_unlock(&g_vacuum_mutex);
}
void cycleVacuumOption(int option) {
  pthread_mutex_lock(&g_vacuum_mutex);
  if(!g_vacuum.busy) {
    auto& plan=g_vacuum.plan;
    if(option==1) plan.fan=plan.fan%4+1;
    else if(option==2) plan.water=plan.water%3+1;
    else if(option==3) plan.scrub=plan.scrub%2+1;
    else if(option==4) plan.passes=plan.passes%3+1;
    else if(option==5) plan.coverage=plan.coverage%2+1;
    g_vacuum_dirty=true;
  }
  pthread_mutex_unlock(&g_vacuum_mutex);
}
void updateVacuum(const narwal::Status& status,bool online,bool explicit_request,const char* notice) {
  pthread_mutex_lock(&g_vacuum_mutex);
  if(g_vacuum.start_pending_until &&
     (time(nullptr)>=g_vacuum.start_pending_until ||
      (status.state!=narwal::State::Idle && status.state!=narwal::State::Docked && status.state!=narwal::State::Unknown))) {
    g_vacuum.start_pending_until=0; g_vacuum_dirty=true;
  }
  if(g_vacuum.start_pending_until && !notice[0]) notice="Start accepted; waiting...";
  bool changed=g_vacuum.online!=online || g_vacuum.status.battery!=status.battery || g_vacuum.status.state!=status.state ||
    g_vacuum.status.has_position!=status.has_position || g_vacuum.status.robot_x!=status.robot_x || g_vacuum.status.robot_y!=status.robot_y || g_vacuum.status.progress!=status.progress;
  if(changed || strcmp(g_vacuum.notice,notice)!=0 || explicit_request) g_vacuum_dirty=true;
  if(online && (changed || explicit_request))
    fprintf(stderr,"vacuum=status online=1 battery=%d state=%s\n",status.battery,narwal::state_label(status.state));
  g_vacuum.online=online;
  g_vacuum.status=status;
  // A poll can complete after a touch queues a command; keep its pending state.
  if(explicit_request) g_vacuum.busy=false;
  if(!g_vacuum_pending) snprintf(g_vacuum.notice,sizeof(g_vacuum.notice),"%s",notice);
  pthread_mutex_unlock(&g_vacuum_mutex);
}
void* vacuumWorkerMain(void*) {
  time_t next_poll=0;
  time_t next_map=0;
  while(g_running) {
    bool requested=false,detail=false; narwal::Command command=narwal::Command::Refresh; narwal::CleanOptions plan;
    pthread_mutex_lock(&g_vacuum_mutex);
    if(g_vacuum_pending) {
      requested=true; command=g_vacuum_command; g_vacuum_pending=false;
      plan=g_vacuum_pending_plan;
    }
    detail=g_vacuum_detail_open;
    pthread_mutex_unlock(&g_vacuum_mutex);
    if(!requested && time(nullptr)<next_poll) { sleep(1); continue; }
    auto old=vacuumSnapshot(); narwal::Status status=old.status; bool got_status=false;
    try {
      auto identity=narwal::read_identity(g_vacuum_identity_file);
      narwal::Client client(g_vacuum_host,identity);
      status=client.status(); got_status=true;
      if(requested && command!=narwal::Command::Refresh) {
        client.command(command,status,plan);
        fprintf(stderr,"vacuum=request-accepted result=%s\n",narwal::accepted_label(command));
        pthread_mutex_lock(&g_vacuum_mutex);
        if(command==narwal::Command::Start) g_vacuum.start_pending_until=time(nullptr)+30;
        else if(command==narwal::Command::Dock) g_vacuum.start_pending_until=0;
        pthread_mutex_unlock(&g_vacuum_mutex);
      }
      if(detail && (requested || time(nullptr)>=next_map || !old.map)) {
        try {
          auto map=std::make_shared<narwal::Map>(client.map());
          pthread_mutex_lock(&g_vacuum_mutex);
          if(g_vacuum.map && g_vacuum.map->id!=map->id && g_vacuum.plan.rooms_only) {
            g_vacuum.plan.rooms.clear(); g_vacuum.plan.map_id=map->id;
          }
          g_vacuum.map=map; g_vacuum_dirty=true;
          pthread_mutex_unlock(&g_vacuum_mutex);
          next_map=time(nullptr)+30;
        } catch(const std::exception& error) { fprintf(stderr,"vacuum=map-unavailable error=%s\n",error.what()); }
      }
      if(detail) {
        try {
          auto live=client.telemetry();
          status.has_position=live.has_position; status.robot_x=live.robot_x; status.robot_y=live.robot_y;
          status.progress=live.progress; status.cleaned_area=live.cleaned_area; status.elapsed_seconds=live.elapsed_seconds;
        } catch(const std::exception&) { /* Optional broadcasts may be absent while docked. */ }
      }
      updateVacuum(status,true,requested,requested ? narwal::accepted_label(command) : "");
    } catch(const std::exception& error) {
      fprintf(stderr,"vacuum=%s error=%s\n",requested ? "request-failed" : "status-unavailable",error.what());
      const char* notice=got_status ? error.what() : "Connection unavailable";
      if(got_status && strstr(notice,"timeout")) notice="No reply; check vacuum";
      updateVacuum(status,got_status,requested,notice);
    }
    auto now=vacuumSnapshot();
    next_poll=time(nullptr)+(requested || now.status.state==narwal::State::Cleaning || now.status.state==narwal::State::Returning ? 5 : 30);
  }
  return nullptr;
}
void startVacuumWorker() {
  const char* host=getenv("NARWAL_HOST");
  const char* path=getenv("NARWAL_IDENTITY_FILE");
  if(!path || !path[0]) path="/mnt/us/extensions/narwal-probe/identity.txt";
  if(!host || !host[0]) return;
  try {
    in_addr addr{};
    narwal::require(inet_pton(AF_INET,host,&addr)==1,"invalid vacuum address");
    narwal::require(narwal::valid_identity(narwal::read_identity(path)),"vacuum identity missing");
    snprintf(g_vacuum_host,sizeof(g_vacuum_host),"%s",host);
    snprintf(g_vacuum_identity_file,sizeof(g_vacuum_identity_file),"%s",path);
    pthread_mutex_lock(&g_vacuum_mutex);
    g_vacuum.configured=true;
    snprintf(g_vacuum.notice,sizeof(g_vacuum.notice),"Connecting...");
    pthread_mutex_unlock(&g_vacuum_mutex);
    pthread_t worker;
    if(pthread_create(&worker,nullptr,vacuumWorkerMain,nullptr)!=0) throw std::runtime_error("cannot start vacuum worker");
    pthread_detach(worker);
  } catch(const std::exception& error) {
    fprintf(stderr,"vacuum=setup-unavailable error=%s\n",error.what());
    pthread_mutex_lock(&g_vacuum_mutex);
    g_vacuum.configured=false;
    pthread_mutex_unlock(&g_vacuum_mutex);
  }
}
