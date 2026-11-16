#include "../kindle/native/src/narwal_client.hpp"
#include <cassert>

int main() {
  using namespace narwal;
  Bytes compressed={0x78,0x9c,0xe3,0x52,0x58,0xc0,0x4,0x84,0x2c,0x40,0x88,0x83,0x6,0x0,0xb2,0x76,0xa,0x5b};
  Bytes payload=encode_number(1,9);
  append(payload,encode_number(4,4)); append(payload,encode_number(5,4));
  for(int id:{1,2}) {
    Bytes room=encode_number(1,id); append(room,encode_number(2,id==1 ? 3 : 4));
    append(payload,encode_bytes(12,room));
  }
  Bytes base=payload; append(payload,encode_bytes(17,compressed));
  Map map=decode_map(payload);
  assert(map.width==4 && map.height==4 && map.pixels.size()==16);
  assert(map.pixels[0]==0x120 && map.pixels[3]==0x220);
  assert(map.min_x==0 && map.max_x==3 && map.min_y==0 && map.max_y==3);
  assert(map.details[0].name=="Living room" && map.details[1].name=="Kitchen");
  assert(map.details[0].cx==.5 && map.details[1].cx==2.5);
  bool failed=false;
  try { auto bad=base; append(bad,encode_bytes(17,{0x78,0x01,0xff})); decode_map(bad); }
  catch(const std::exception&) { failed=true; } assert(failed);
  failed=false;
  try { auto bad=base; append(bad,encode_bytes(17,encode_bytes(1,{0x20}))); decode_map(bad); }
  catch(const std::exception&) { failed=true; } assert(failed);
  failed=false;
  try { auto options=CleanOptions{}; options.rooms_only=true; options.map_id=9; clean_task(map,options); }
  catch(const std::exception&) { failed=true; } assert(failed);
  failed=false;
  try { auto options=CleanOptions{}; options.rooms_only=true; options.map_id=8; options.rooms={1}; clean_task(map,options); }
  catch(const std::exception&) { failed=true; } assert(failed);
  return 0;
}
