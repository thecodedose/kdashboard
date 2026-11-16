#define main dashboard_app_main
#include "../kindle/native/src/kindle_dashboard.cpp"
#undef main
#include <assert.h>
#include <string>

int main() {
  int first=0,seen=0,fired=0;
  assert(allowContactAction(kTouchOpenVacuum,&first,&seen,&fired));
  assert(!allowContactAction(kTouchVacuumDock,&first,&seen,&fired));
  first=seen=fired=0;
  assert(allowContactAction(kTouchVacuumStart,&first,&seen,&fired));
  assert(!allowContactAction(kTouchVacuumStart,&first,&seen,&fired));
  first=seen=fired=0;
  assert(!allowContactAction(kTouchVacuumStart,&first,&seen,&fired,false));
  Dashboard dashboard;
  assert(parseDashboard("{\"ok\":true,\"generated_at\":\"2026-10-22T00:00:00+05:30\",\"quick_note\":{\"text\":\"First paragraph\\n\\nSecond paragraph\"},\"calendar\":{\"status\":\"connected\",\"date\":\"2026-10-22\",\"events\":[{\"title\":\"Meeting\",\"time\":\"All day\"}]},\"lists\":[]}", &dashboard));
  assert(strcmp(dashboard.quick_note, "First paragraph\n\nSecond paragraph") == 0);
  assert(dashboard.calendar_count == 1);
  // Long unbroken text must remain readable across wraps and pages.
  std::string message(15000, 'A'); message += "END_OF_MESSAGE";
  copyText(dashboard.quick_note, sizeof(dashboard.quick_note), message.c_str());
  std::string restored;
  int offset = 0; char row[200];
  while (message[offset]) { offset = noteLine(message.c_str(), offset, 37, row, sizeof(row)); restored += row; }
  assert(restored == message);
  for (int width : {760, 1072}) {
    Canvas canvas = {width, width == 760 ? 1024 : 1448, NULL};
    canvas.pixels = static_cast<unsigned char*>(calloc(canvas.width * canvas.height, 1));
    drawBitmapDashboard(&canvas, &dashboard, "test");
    const int note_x = canvas.width - 30;
    assert(applyTouchAt(note_x, 300)); assert(g_pending_action == kTouchOpenMusic);
    // Legacy notes stay accessible by their explicit view, outside the home tile.
    g_pending_action = kTouchOpenNote;
    Options options = {}; assert(handlePendingTouch(&options) == 1); assert(g_active_note);
    drawCurrentDashboard(&canvas, &dashboard, "test"); assert(g_note_page_count > 1);
    assert(applyTouchAt(canvas.width - 100, canvas.height - 50)); assert(g_pending_action == kTouchNoteNext);
    assert(handlePendingTouch(&options) == 1); assert(g_note_page == 1);
    g_note_page = g_note_page_count - 1; drawCurrentDashboard(&canvas, &dashboard, "test");
    g_pending_action = kTouchBack; assert(handlePendingTouch(&options) == 1); assert(!g_active_note);
    dashboard.list_count = 2;
    copyText(dashboard.lists[0].key, sizeof(dashboard.lists[0].key), "todo");
    copyText(dashboard.lists[1].key, sizeof(dashboard.lists[1].key), "grocery");
    dashboard.calendar_count = 100;
    drawBitmapDashboard(&canvas, &dashboard, "test");
    // The home calendar card is replaced by direct vacuum controls.
    g_vacuum.configured=true; g_vacuum.online=true; g_vacuum.busy=false;
    g_vacuum.status={100,narwal::State::Idle};
    drawBitmapDashboard(&canvas, &dashboard, "test");
    bool opened=false;
    for(int i=0;i<g_touch_region_count;++i) {
      auto region=g_touch_regions[i];
      assert(region.action<kTouchVacuumStart || region.action>kTouchVacuumRefresh);
      if(region.action==kTouchOpenVacuum) {
        opened=true;
        assert(applyTouchAt(region.rect.x+region.rect.w/2,region.rect.y+region.rect.h/2));
        assert(handlePendingTouch(&options)==1 && g_active_vacuum);
        break;
      }
    }
    assert(opened);
    auto map=std::make_shared<narwal::Map>();
    map->id=9; map->rooms={101,203}; map->width=4; map->height=4; map->max_x=3; map->max_y=3;
    map->details={{101,"Living room",.5,1.5,8},{203,"Kitchen",2.5,1.5,8}};
    for(int y=0;y<4;++y) for(int x=0;x<4;++x) map->pixels.push_back(uint32_t((x<2 ? 101 : 203)<<8)|0x20);
    g_vacuum.map=map; g_vacuum.plan={}; g_vacuum_pending=false; g_vacuum.busy=false;
    drawCurrentDashboard(&canvas,&dashboard,"test");
    bool found_start=false;
    for(int i=0;i<g_touch_region_count;++i) {
      auto region=g_touch_regions[i];
      if(region.action!=kTouchVacuumStart) continue;
      found_start=true;
      int x=region.rect.x+region.rect.w/2,y=region.rect.y+region.rect.h/2;
      assert(applyTouchAt(x,y,false)); assert(g_pending_action==kTouchNone);
      assert(applyTouchAt(x+region.rect.w+10,y)); assert(g_pending_action==kTouchNone);
      assert(!g_vacuum_pending);
      assert(applyTouchAt(x,y)); assert(handlePendingTouch(&options)==1);
      assert(g_vacuum_pending && g_vacuum_command==narwal::Command::Start);
      assert(!queueVacuumCommand(narwal::Command::Start));
      g_vacuum_pending=false; g_vacuum.busy=false;
      g_vacuum.start_pending_until=time(nullptr)+30;
      assert(!queueVacuumCommand(narwal::Command::Start));
      g_vacuum.start_pending_until=0; break;
    }
    assert(found_start);
    // Map selection changes only the draft; it cannot start the robot.
    auto projection=g_vacuum_projection;
    toggleVacuumRoomAt(projection.rect.x+int(.5*projection.scale),projection.rect.y+int(.5*projection.scale));
    assert(g_vacuum.plan.rooms_only && g_vacuum.plan.map_id==9 && g_vacuum.plan.rooms==std::vector<uint64_t>{101});
    assert(!g_vacuum_pending);
    // Each mode is a direct, independent choice; selecting it never starts a clean.
    for(int mode:{1,2,3,4}) {
      drawCurrentDashboard(&canvas,&dashboard,"test"); bool mode_found=false;
      for(int i=0;i<g_touch_region_count;++i) {
        auto region=g_touch_regions[i];
        if(region.action!=kTouchVacuumSetMode || region.item_index!=mode) continue;
        mode_found=true;
        assert(applyTouchAt(region.rect.x+region.rect.w/2,region.rect.y+region.rect.h/2));
        assert(handlePendingTouch(&options)==1 && g_vacuum.plan.mode==mode && !g_vacuum_pending);
        break;
      }
      assert(mode_found);
    }
    setVacuumMode(2); assert(g_vacuum.plan.mode==2);
    cycleVacuumOption(2); assert(g_vacuum.plan.water==3);
    cycleVacuumOption(4); assert(g_vacuum.plan.passes==2);
    g_vacuum_settings=1; drawCurrentDashboard(&canvas,&dashboard,"test");
    for(int i=0;i<g_touch_region_count;++i) if(g_touch_regions[i].action==kTouchVacuumOption)
      assert(g_touch_regions[i].item_index==1 || g_touch_regions[i].item_index==4 || g_touch_regions[i].item_index==5);
    g_vacuum_settings=3; drawCurrentDashboard(&canvas,&dashboard,"test");
    for(int i=0;i<g_touch_region_count;++i) if(g_touch_regions[i].action==kTouchVacuumOption)
      assert(g_touch_regions[i].item_index>=2 && g_touch_regions[i].item_index<=5);
    g_pending_action=kTouchBack; assert(handlePendingTouch(&options)==1 && !g_vacuum_settings && g_active_vacuum);
    toggleVacuumRoom(0); assert(g_vacuum.plan.rooms.empty());
    assert(!queueVacuumCommand(narwal::Command::Start)); // empty selection never becomes whole home
    vacuumWholeHome(); assert(!g_vacuum.plan.rooms_only);
    for(auto state:{narwal::State::Cleaning,narwal::State::Paused,narwal::State::Idle}) {
      g_vacuum.status.state=state;
      TouchAction target=state==narwal::State::Cleaning ? kTouchVacuumPause : state==narwal::State::Paused ? kTouchVacuumResume : kTouchVacuumDock;
      drawCurrentDashboard(&canvas,&dashboard,"test");
      bool found=false;
      for(int i=0;i<g_touch_region_count;++i) if(g_touch_regions[i].action==target) {
        found=true; auto rect=g_touch_regions[i].rect;
        assert(applyTouchAt(rect.x+rect.w/2,rect.y+rect.h/2));
        assert(handlePendingTouch(&options)==1 && g_vacuum_pending);
        g_vacuum_pending=false; g_vacuum.busy=false; break;
      }
      assert(found);
    }
    g_vacuum.online=false;
    drawCurrentDashboard(&canvas,&dashboard,"test");
    for(int i=0;i<g_touch_region_count;++i)
      assert(g_touch_regions[i].action<kTouchVacuumStart || g_touch_regions[i].action>kTouchVacuumDock);
    // Calendar parsing and its explicit page still work for existing deep links.
    g_active_vacuum=0; vacuumDetailOpen(false); g_active_calendar=1;
    drawCurrentDashboard(&canvas, &dashboard, "test"); assert(g_calendar_page_count > 1);
    assert(applyTouchAt(canvas.width - 100, canvas.height - 50)); assert(g_pending_action == kTouchCalendarNext);
    assert(handlePendingTouch(&options) == 1); assert(g_calendar_page == 1);
    g_pending_action = kTouchHome; assert(handlePendingTouch(&options) == 1); assert(!g_active_calendar);
    free(canvas.pixels);
  }
  assert(chdir("../..")==0); // Match production preview asset paths from the repo root.
  char* fixture=readFile("kindle/native/fixtures/dashboard-data.json");
  assert(fixture && parseDashboard(fixture,&dashboard)); free(fixture);
  copyText(dashboard.quick_note,sizeof(dashboard.quick_note),"A little progress every day."); dashboard.note_available=1;
  g_vacuum.online=true; g_vacuum.status={100,narwal::State::Idle}; g_vacuum.notice[0]='\0';
  FILE* map_file=fopen("kindle/native/build/narwal-map-response.bin","rb");
  if(map_file) {
    fseek(map_file,0,SEEK_END); long size=ftell(map_file); rewind(map_file);
    narwal::Bytes bytes(static_cast<size_t>(size)); assert(fread(bytes.data(),1,bytes.size(),map_file)==bytes.size()); fclose(map_file);
    auto response=narwal::decode(narwal::Bytes(bytes.begin()+4+bytes[3],bytes.end()));
    g_vacuum.map=std::make_shared<narwal::Map>(narwal::decode_map(narwal::bytes_field(response,2)));
  }
  g_vacuum.plan={};
  for(int width:{760,1072}) {
    Canvas canvas={width,width==760 ? 1024 : 1448,nullptr};
    canvas.pixels=static_cast<unsigned char*>(calloc(canvas.width*canvas.height,1));
    drawBitmapDashboard(&canvas,&dashboard,"test");
    char path[128]; snprintf(path,sizeof(path),"kindle/native/build/vacuum-preview-%d.pgm",width);
    assert(writePgm(path,&canvas));
    g_active_vacuum=1; g_vacuum_settings=0; drawCurrentDashboard(&canvas,&dashboard,"test");
    snprintf(path,sizeof(path),"kindle/native/build/vacuum-map-preview-%d.pgm",width); assert(writePgm(path,&canvas));
    g_vacuum_settings=1; drawCurrentDashboard(&canvas,&dashboard,"test");
    snprintf(path,sizeof(path),"kindle/native/build/vacuum-options-preview-%d.pgm",width); assert(writePgm(path,&canvas));
    g_vacuum_settings=2; drawCurrentDashboard(&canvas,&dashboard,"test");
    snprintf(path,sizeof(path),"kindle/native/build/vacuum-rooms-preview-%d.pgm",width); assert(writePgm(path,&canvas));
    setVacuumMode(2); g_vacuum_settings=3; drawCurrentDashboard(&canvas,&dashboard,"test");
    snprintf(path,sizeof(path),"kindle/native/build/vacuum-mop-options-preview-%d.pgm",width); assert(writePgm(path,&canvas));
    setVacuumMode(1);
    g_active_vacuum=0; g_vacuum_settings=0; free(canvas.pixels);
  }
  return 0;
}
