struct VacuumMapProjection {
  std::shared_ptr<const narwal::Map> map;
  Rect rect{};
  double scale=1;
} g_vacuum_projection;

bool vacuumRoomSelected(const VacuumSnapshot& vacuum,uint64_t id) {
  return !vacuum.plan.rooms_only || std::find(vacuum.plan.rooms.begin(),vacuum.plan.rooms.end(),id)!=vacuum.plan.rooms.end();
}
const char* vacuumStateText(const VacuumSnapshot& vacuum) {
  return !vacuum.configured ? "Setup needed" : vacuum.online ? narwal::state_label(vacuum.status.state) : "Offline";
}
void drawVacuumTile(Canvas* canvas,int x,int y,int w,int h) {
  auto vacuum=vacuumSnapshot();
  drawPgmImageCover(canvas,x+3,y+3,w-6,h-6,
    "/mnt/us/extensions/kindle-dashboard/assets/narwal-x10-pro.pgm",
    "kindle/kual/kindle-dashboard/assets/narwal-x10-pro.pgm",0);
  strokeRect(canvas,x,y,w,h,3,0);
  fillRect(canvas,x+14,y+14,w-28,44,255);
  strokeRect(canvas,x+14,y+14,w-28,44,2,0);
  drawTextCentered(canvas,x+w/2,y+25,w-44,"NARWAL X10 PRO",w>=480 ? 3 : 2,0);
  fillRect(canvas,x+12,y+h-90,w-24,78,255);
  line(canvas,x+16,y+h-90,x+w-16,y+h-90,2,0);
  char state[80];
  if(vacuum.status.battery>=0) snprintf(state,sizeof(state),"%s // BATT %d%%",vacuumStateText(vacuum),vacuum.status.battery);
  else snprintf(state,sizeof(state),"%s",vacuumStateText(vacuum));
  drawTextCentered(canvas,x+w/2,y+h-72,w-32,state,w>=480 ? 3 : 2,0);
  drawTextCentered(canvas,x+w/2,y+h-34,w-24,"TAP FOR MAP AND CONTROLS",2,0);
  addTouchRegion({x,y,w,h},kTouchOpenVacuum,-1,-1,"",0);
}
void vacuumNavButton(Canvas* canvas,Rect rect,const char* label,TouchAction action,bool selected=false,int index=-1,bool enabled=true) {
  fillRect(canvas,rect.x,rect.y,rect.w,rect.h,selected ? 0 : 255);
  strokeRect(canvas,rect.x,rect.y,rect.w,rect.h,2,enabled ? 0 : 160);
  drawTextCentered(canvas,rect.x+rect.w/2,rect.y+(rect.h-16)/2,rect.w-16,label,2,selected ? 255 : enabled ? 0 : 160);
  addTouchRegion(rect,enabled ? action : kTouchNone,-1,index,"",0);
}
void drawVacuumModes(Canvas* canvas,int y,const VacuumSnapshot& vacuum) {
  const int gap=8,w=(canvas->width-48-3*gap)/4;
  static const int modes[]={1,2,4,3};
  static const char* labels[]={"VACUUM","MOP","VAC AND MOP","VAC THEN MOP"};
  for(int i=0;i<4;++i)
    vacuumNavButton(canvas,{24+i*(w+gap),y,w,48},labels[i],kTouchVacuumSetMode,vacuum.plan.mode==modes[i],modes[i],!vacuum.busy);
}
void drawVacuumMap(Canvas* canvas,Rect frame,const VacuumSnapshot& vacuum) {
  strokeRect(canvas,frame.x,frame.y,frame.w,frame.h,2,0);
  g_vacuum_projection={};
  if(!vacuum.map || vacuum.map->pixels.empty()) {
    drawTextCentered(canvas,frame.x+frame.w/2,frame.y+frame.h/2-28,frame.w-32,vacuum.busy ? "LOADING YOUR MAP..." : "MAP UNAVAILABLE",3,0);
    drawTextCentered(canvas,frame.x+frame.w/2,frame.y+frame.h/2+16,frame.w-32,"TAP REFRESH TO TRY AGAIN",2,0);
    return;
  }
  const auto& map=*vacuum.map;
  int map_w=map.max_x-map.min_x+1,map_h=map.max_y-map.min_y+1;
  if(map_w<=0 || map_h<=0) return;
  double scale=std::min(double(frame.w-36)/map_w,double(frame.h-36)/map_h);
  Rect rect{frame.x+(frame.w-int(map_w*scale))/2,frame.y+(frame.h-int(map_h*scale))/2,int(map_w*scale),int(map_h*scale)};
  g_vacuum_projection={vacuum.map,rect,scale};
  for(int y=0;y<rect.h;++y) for(int x=0;x<rect.w;++x) {
    int gx=std::min(map.max_x,map.min_x+int(x/scale));
    int gy=std::max(map.min_y,map.max_y-int(y/scale));
    uint32_t value=map.pixels[size_t(gy)*map.width+gx];
    unsigned char color=255;
    if(value==0x20) color=242;
    else if(value==0x28) color=100;
    else if(value) {
      bool selected=vacuumRoomSelected(vacuum,value>>8);
      color=(value&0x10) ? 40 : selected ? 215 : 248;
      // Sparse hatching distinguishes selected rooms without relying on color.
      if(selected && !(value&0x10) && (gx+gy)%9==0) color=185;
    }
    setPixel(canvas,rect.x+x,rect.y+y,color);
  }
  addTouchRegion(frame,vacuum.busy ? kTouchNone : kTouchVacuumMap,-1,-1,"",0);
  for(size_t i=0;i<map.details.size();++i) {
    const auto& room=map.details[i]; if(!room.cells) continue;
    int cx=rect.x+int((room.cx-map.min_x+.5)*scale),cy=rect.y+int((map.max_y-room.cy+.5)*scale);
    int text_scale=frame.h<400 ? 1 : 2,label_h=text_scale==1 ? 32 : 44;
    int label_w=std::min({220,frame.w-24,std::max(text_scale==1 ? 76 : 112,int(room.name.size())*6*text_scale+20)});
    Rect label{std::clamp(cx-label_w/2,frame.x+8,frame.x+frame.w-label_w-8),std::clamp(cy-label_h/2,frame.y+8,frame.y+frame.h-label_h-8),label_w,label_h};
    bool selected=vacuumRoomSelected(vacuum,room.id);
    fillRect(canvas,label.x,label.y,label.w,label.h,255);
    strokeRect(canvas,label.x,label.y,label.w,label.h,selected ? 2 : 1,0);
    drawTextCentered(canvas,label.x+label.w/2,label.y+(label_h-8*text_scale)/2,label.w-12,room.name.c_str(),text_scale,0);
    addTouchRegion(label,vacuum.busy ? kTouchNone : kTouchVacuumRoomToggle,-1,int(i),"",0);
  }
  if(map.has_dock) {
    int dx=rect.x+int((map.dock_x-map.min_x)*scale),dy=rect.y+int((map.max_y-map.dock_y)*scale);
    dx=std::clamp(dx,frame.x+16,frame.x+frame.w-16); dy=std::clamp(dy,frame.y+16,frame.y+frame.h-16);
    fillRect(canvas,dx-12,dy-12,24,24,0);
    drawTextCentered(canvas,dx,dy-7,22,"D",2,255);
  }
  if(vacuum.online && vacuum.status.has_position) {
    double gx=vacuum.status.robot_x-map.origin_x,gy=vacuum.status.robot_y-map.origin_y;
    if(gx>=map.min_x && gx<=map.max_x && gy>=map.min_y && gy<=map.max_y) {
      int rx=rect.x+int((gx-map.min_x)*scale),ry=rect.y+int((map.max_y-gy)*scale);
      circleRing(canvas,rx,ry,15,3,100,0); circleRing(canvas,rx,ry,6,6,100,0);
    }
  }
}
void toggleVacuumRoomAt(int x,int y) {
  auto projection=g_vacuum_projection; auto vacuum=vacuumSnapshot();
  if(!projection.map || !vacuum.map || projection.map->id!=vacuum.map->id || projection.scale<=0) return;
  if(x<projection.rect.x || y<projection.rect.y || x>=projection.rect.x+projection.rect.w || y>=projection.rect.y+projection.rect.h) return;
  int gx=projection.map->min_x+int((x-projection.rect.x)/projection.scale);
  int gy=projection.map->max_y-int((y-projection.rect.y)/projection.scale);
  if(gx<0 || gy<0 || gx>=projection.map->width || gy>=projection.map->height) return;
  uint64_t id=projection.map->pixels[size_t(gy)*projection.map->width+gx]>>8;
  for(size_t i=0;i<vacuum.map->rooms.size();++i) if(vacuum.map->rooms[i]==id) { toggleVacuumRoom(i); return; }
}
void drawVacuumSettings(Canvas* canvas,const Dashboard* dashboard,const char* status,const VacuumSnapshot& vacuum) {
  widgetPageShell(canvas,dashboard,status,g_vacuum_settings==3 ? "MOP OPTIONS" : g_vacuum_settings==2 ? "ROOM SELECTION" : "VACUUM OPTIONS");
  const int margin=24,gap=12,width=(canvas->width-2*margin-2*gap)/3,tab_y=kKindleStatusBarHeight+244;
  vacuumNavButton(canvas,{margin,tab_y,width,48},"VACUUM SETTINGS",kTouchVacuumDrySettings,g_vacuum_settings==1);
  vacuumNavButton(canvas,{margin+width+gap,tab_y,width,48},"MOP SETTINGS",kTouchVacuumMopSettings,g_vacuum_settings==3);
  vacuumNavButton(canvas,{margin+2*(width+gap),tab_y,width,48},"ROOMS",kTouchVacuumRooms,g_vacuum_settings==2);
  int top=tab_y+72;
  if(g_vacuum_settings!=2) {
    drawVacuumModes(canvas,top,vacuum); top+=74;
    const char* names[]={"","SUCTION","WATER LEVEL","MOP SCRUB","TOTAL PASSES","COVERAGE"};
    static const char* fans[]={"","Quiet","Normal","Strong","Max"};
    static const char* water[]={"","Dry","Normal","Wet"};
    static const int vacuum_fields[]={1,4,5},mop_fields[]={2,3,4,5};
    const int count=g_vacuum_settings==3 ? 4 : 3;
    const int row_h=std::min(114,(canvas->height-top-100)/count);
    for(int row=0;row<count;++row) {
      int i=g_vacuum_settings==3 ? mop_fields[row] : vacuum_fields[row];
      Rect rect{margin,top+row*row_h,canvas->width-2*margin,row_h-10};
      bool enabled=!vacuum.busy;
      const char* value=""; char passes[32];
      if(i==1) value=fans[vacuum.plan.fan];
      if(i==2) value=water[vacuum.plan.water];
      if(i==3) value=vacuum.plan.scrub==1 ? "Normal" : "High";
      if(i==4) { snprintf(passes,sizeof(passes),"%d %s",vacuum.plan.passes,vacuum.plan.passes==1 ? "pass" : "passes"); value=passes; }
      if(i==5) value=vacuum.plan.coverage==1 ? "Standard" : "Meticulous";
      strokeRect(canvas,rect.x,rect.y,rect.w,rect.h,2,enabled ? 0 : 150);
      drawTextClipped(canvas,rect.x+18,rect.y+12,rect.w-70,names[i],2,enabled ? 0 : 150);
      drawTextClipped(canvas,rect.x+18,rect.y+42,rect.w-70,value,row_h>=86 ? 3 : 2,enabled ? 0 : 150);
      int arrow_x=rect.x+rect.w-36,arrow_y=rect.y+rect.h/2;
      line(canvas,arrow_x-5,arrow_y-9,arrow_x+5,arrow_y,2,enabled ? 0 : 150);
      line(canvas,arrow_x+5,arrow_y,arrow_x-5,arrow_y+9,2,enabled ? 0 : 150);
      addTouchRegion(rect,enabled ? kTouchVacuumOption : kTouchNone,-1,i,"",0);
    }
  } else {
    vacuumNavButton(canvas,{margin,top,canvas->width-2*margin,48},"WHOLE HOME",kTouchVacuumWholeHome,!vacuum.plan.rooms_only,-1,!vacuum.busy);
    top+=66;
    if(vacuum.map) {
      int capacity=std::max(1,(canvas->height-top-130)/62);
      int pages=std::max(1,(int(vacuum.map->details.size())+capacity-1)/capacity);
      g_vacuum_room_page=std::min(g_vacuum_room_page,pages-1);
      for(int i=g_vacuum_room_page*capacity;i<int(vacuum.map->details.size()) && i<(g_vacuum_room_page+1)*capacity;++i) {
        const auto& room=vacuum.map->details[size_t(i)]; char label[180];
        snprintf(label,sizeof(label),"[%s] %s",vacuumRoomSelected(vacuum,room.id) ? "X" : " ",room.name.c_str());
        vacuumNavButton(canvas,{margin,top+(i-g_vacuum_room_page*capacity)*62,canvas->width-2*margin,52},label,kTouchVacuumRoomToggle,false,i,!vacuum.busy);
      }
      if(pages>1) {
        if(g_vacuum_room_page) vacuumNavButton(canvas,{margin,canvas->height-138,width,44},"PREVIOUS",kTouchVacuumRoomPrevious);
        if(g_vacuum_room_page+1<pages) vacuumNavButton(canvas,{margin+width+gap,canvas->height-138,width,44},"NEXT",kTouchVacuumRoomNext);
      }
    } else drawTextCentered(canvas,canvas->width/2,top+36,canvas->width-60,"OPEN THE MAP TO LOAD ROOMS",2,0);
  }
  vacuumNavButton(canvas,{margin,canvas->height-74,canvas->width-2*margin,50},"BACK TO MAP",kTouchBack);
}
void drawVacuumPage(Canvas* canvas,const Dashboard* dashboard,const char* status) {
  auto vacuum=vacuumSnapshot();
  if(g_vacuum_settings) { drawVacuumSettings(canvas,dashboard,status,vacuum); return; }
  widgetPageShell(canvas,dashboard,status,"NARWAL X10 PRO");
  int top=kKindleStatusBarHeight+252;
  char state[100];
  if(vacuum.status.battery>=0) snprintf(state,sizeof(state),"%s // BATTERY %d%%",vacuumStateText(vacuum),vacuum.status.battery);
  else snprintf(state,sizeof(state),"%s // BATTERY --",vacuumStateText(vacuum));
  drawTextClipped(canvas,24,top+10,canvas->width-240,state,2,0);
  vacuumButton(canvas,{canvas->width-194,top,170,40},"REFRESH",kTouchVacuumRefresh,narwal::Command::Refresh,vacuum);
  Rect map_rect{24,top+56,canvas->width-48,canvas->height-364-(top+56)};
  drawVacuumMap(canvas,map_rect,vacuum);
  if(vacuum.status.progress>=0 && vacuum.status.state==narwal::State::Cleaning) {
    char progress[64];
    if(vacuum.status.cleaned_area>=0) snprintf(progress,sizeof(progress),"%.0f%% // %.1f M2",vacuum.status.progress*100,vacuum.status.cleaned_area);
    else snprintf(progress,sizeof(progress),"%.0f%% CLEANED",vacuum.status.progress*100);
    fillRect(canvas,map_rect.x+12,map_rect.y+12,250,34,255);
    drawTextClipped(canvas,map_rect.x+20,map_rect.y+20,232,progress,2,0);
  }
  drawVacuumModes(canvas,canvas->height-344,vacuum);
  int nav_y=canvas->height-280,gap=10,nav_w=(canvas->width-48-2*gap)/3;
  vacuumNavButton(canvas,{24,nav_y,nav_w,46},"WHOLE HOME",kTouchVacuumWholeHome,!vacuum.plan.rooms_only,-1,!vacuum.busy);
  vacuumNavButton(canvas,{24+nav_w+gap,nav_y,nav_w,46},"ROOMS",kTouchVacuumRooms,vacuum.plan.rooms_only);
  vacuumNavButton(canvas,{24+2*(nav_w+gap),nav_y,nav_w,46},"OPTIONS",kTouchVacuumOptions);
  char summary[160];
  if(vacuum.plan.rooms_only) snprintf(summary,sizeof(summary),"%s // %zu ROOM%s // %d PASS%s",narwal::mode_label(vacuum.plan.mode),vacuum.plan.rooms.size(),vacuum.plan.rooms.size()==1 ? "" : "S",vacuum.plan.passes,vacuum.plan.passes==1 ? "" : "ES");
  else snprintf(summary,sizeof(summary),"%s // WHOLE HOME // %d PASS%s",narwal::mode_label(vacuum.plan.mode),vacuum.plan.passes,vacuum.plan.passes==1 ? "" : "ES");
  drawTextCentered(canvas,canvas->width/2,canvas->height-218,canvas->width-48,summary,2,0);
  int button_w=(canvas->width-48-gap)/2;
  char start[64]; snprintf(start,sizeof(start),"START %s",narwal::mode_label(vacuum.plan.mode));
  vacuumButton(canvas,{24,canvas->height-178,button_w,58},start,kTouchVacuumStart,narwal::Command::Start,vacuum);
  vacuumButton(canvas,{24+button_w+gap,canvas->height-178,button_w,58},"PAUSE",kTouchVacuumPause,narwal::Command::Pause,vacuum);
  vacuumButton(canvas,{24,canvas->height-110,button_w,58},"RESUME",kTouchVacuumResume,narwal::Command::Resume,vacuum);
  vacuumButton(canvas,{24+button_w+gap,canvas->height-110,button_w,58},"DOCK",kTouchVacuumDock,narwal::Command::Dock,vacuum);
  drawTextCentered(canvas,canvas->width/2,canvas->height-32,canvas->width-48,vacuum.notice,2,0);
}
