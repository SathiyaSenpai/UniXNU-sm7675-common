/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef SM7675_LLCC_H
#define SM7675_LLCC_H
#include <stdint.h>
/* LLCC v5.0 TRP operations. Register layout/activation protocol follows
 * Qualcomm llcc-qcom.c; GPU SCID routing follows a7xx_llc_activate.
 * Caller owns serialization and stops the GPU before changing its routing.
 * No allocation, deactivation or global cache flush takes place here. */
struct Sm7675LlccSlice {uint32_t id,kib,fixed;};
struct Sm7675LlccPolicy {Sm7675LlccSlice gpu,htw;};
struct Sm7675LlccIo {
 void *cookie;
 bool (*read)(void *,unsigned region,uint32_t offset,uint32_t *);
 bool (*write)(void *,unsigned region,uint32_t offset,uint32_t);
 void (*barrier)(void *);
 void (*delay_us)(void *,unsigned);
};
enum Sm7675LlccStatus {LlccOk=0,LlccObserved=1,LlccArgument=2,LlccIoError=3,
 LlccBankCount=4,LlccConflict=5,LlccReadback=6,LlccTimeout=7};
struct Sm7675LlccReport {
 uint32_t status=LlccArgument,step=0,banks=0,initialized=0,activated=0;
 uint32_t active_before[2]{},active_after[2]{},attr_before[2][3]{};
 uint32_t route_before[2]{},route_after[2]{};
 uint32_t failed_offset=0,failed_bank=0,polls=0,observed=0,expected=0;
};
namespace sm7675_llcc {
constexpr uint32_t BankStride=0x200000,CommonStatus=0x3400c;
constexpr uint32_t Attr0=0x21000,Attr1=0x21004,Attr2=0x21100;
constexpr uint32_t Route0=0x3c01*4,Route1=0x3c02*4;
constexpr uint32_t RouteMask0=(31u<<10)|(1u<<8),RouteMask1=0x3fffffffu;
constexpr uint32_t PolicyRegs[]={0x21f20,0x21f2c,0x21f00,0x21f0c,0x21f10,
 0x21f14,0x21f18,0x21f1c,0x21f24,0x21f28,0x21f30};
inline uint32_t route1(uint32_t id) {
 return id|(id<<5)|(id<<10)|(id<<15)|(id<<20)|(id<<25);
}
/* Regions: 0 LLCC banks, 1 LLCC broadcast, 2 GPU. False means an I/O error.
 * Both an already-active mismatching slice and an unknown inactive policy are
 * refused before the first write. Only entirely blank inactive slices may be
 * initialized. Per-bank differences in global registers are also refused, so
 * a broadcast RMW cannot overwrite another client's bank-specific policy. */
inline Sm7675LlccStatus configure(const Sm7675LlccIo &io,const Sm7675LlccPolicy &p,
 bool apply,Sm7675LlccReport &r) {
 r={};
 auto done=[&](Sm7675LlccStatus s){r.status=s;return s;};
 if(!io.read || !io.write || !io.barrier || !io.delay_us || p.gpu.id>31 ||
    p.htw.id>31 || p.gpu.id==p.htw.id || !p.gpu.kib || !p.htw.kib ||
    p.gpu.kib>4096 || p.htw.kib>4096 || p.gpu.fixed || p.htw.fixed!=1)
  return done(LlccArgument);
 auto rd=[&](unsigned region,uint32_t at,uint32_t &v){
  if(io.read(io.cookie,region,at,&v))return true;
  r.failed_offset=at;r.failed_bank=region==0?at/BankStride:~0u;return false;
 };
 auto wr=[&](unsigned region,uint32_t at,uint32_t v){
  if(!io.write(io.cookie,region,at,v)){
   r.failed_offset=at;r.failed_bank=~0u;return false;
  }
  io.barrier(io.cookie);return true;
 };
 r.step=1;uint32_t common=0;
 if(!rd(0,CommonStatus,common))return done(LlccIoError);
 r.banks=common>>28;
 if(r.banks!=1 && r.banks!=2 && r.banks!=4)return done(LlccBankCount);
 if(!rd(2,Route0,r.route_before[0]) || !rd(2,Route1,r.route_before[1]))return done(LlccIoError);
 r.route_after[0]=r.route_before[0];r.route_after[1]=r.route_before[1];
 const Sm7675LlccSlice slices[2]={p.gpu,p.htw};
 uint32_t expected[2][3]{},active[2][4]{},global[11]{};
 uint32_t bad_offset[2]{},bad_value[2]{},bad_expected[2]{},bad_bank[2]{};
 bool blank[2]={true,true},match[2]={true,true},conflict=false;
 auto conflictAt=[&](uint32_t at,unsigned bank,uint32_t value,uint32_t want){
  if(!conflict){r.failed_offset=at;r.failed_bank=bank;r.observed=value;r.expected=want;}
  conflict=true;
 };
 for(unsigned s=0;s<2;++s) {
  const uint32_t lines=slices[s].kib*16/r.banks;
  if(lines>0xffff || slices[s].kib*16%r.banks)return done(LlccArgument);
  expected[s][0]=0;expected[s][1]=(lines<<16)|16u|(slices[s].fixed<<3);expected[s][2]=0x3fff;
  for(unsigned b=0;b<r.banks;++b) {
   const uint32_t base=b*BankStride,id=slices[s].id;
   if(!rd(0,base+id*0x1000+4,active[s][b]))return done(LlccIoError);
   if(active[s][b]&1)r.active_before[s]|=1u<<b;
   if((active[s][b]&3)!=1 && (active[s][b]&3)!=2)conflictAt(id*0x1000+4,b,active[s][b]&3,3);
   const uint32_t offsets[3]={Attr0+id*8,Attr1+id*8,Attr2+id*4};
   for(unsigned k=0;k<3;++k) {
    uint32_t v=0;if(!rd(0,base+offsets[k],v))return done(LlccIoError);
    if(!b)r.attr_before[s][k]=v;
    if(v)blank[s]=false;
    if(v!=expected[s][k]){
     if(match[s]){bad_offset[s]=offsets[k];bad_value[s]=v;bad_expected[s]=expected[s][k];bad_bank[s]=b;}
     match[s]=false;
    }
   }
  }
  if(!match[s] && !(blank[s] && !r.active_before[s]))conflictAt(bad_offset[s],bad_bank[s],bad_value[s],bad_expected[s]);
 }
 const uint32_t mask=(1u<<p.gpu.id)|(1u<<p.htw.id);
 for(unsigned k=0;k<11;++k) {
  for(unsigned b=0;b<r.banks;++b) {
   uint32_t v=0;if(!rd(0,b*BankStride+PolicyRegs[k],v))return done(LlccIoError);
   if(!b)global[k]=v;else if(v!=global[k])conflictAt(PolicyRegs[k],b,v,global[k]);
  }
  const uint32_t want=k<2?1u<<p.gpu.id:0;
  /* Active slices retain their existing policy, including write allocation. */
  for(unsigned s=0;s<2;++s)if(r.active_before[s] &&
     ((global[k]^want)&(1u<<slices[s].id)))conflictAt(PolicyRegs[k],0,global[k]&mask,want&mask);
 }
 r.active_after[0]=r.active_before[0];r.active_after[1]=r.active_before[1];
 if(!apply)return done(LlccObserved);
 if(conflict)return done(LlccConflict);
 r.step=2;
 for(unsigned s=0;s<2;++s)if(blank[s]) {
  const uint32_t id=slices[s].id,offsets[3]={Attr0+id*8,Attr1+id*8,Attr2+id*4};
  for(unsigned k=0;k<3;++k)if(!wr(1,offsets[k],expected[s][k]))return done(LlccIoError);
  r.initialized|=1u<<s;
 }
 for(unsigned k=0;k<11;++k) {
  const uint32_t want=(global[k]&~mask)|(k<2?1u<<p.gpu.id:0u);
  if(want!=global[k] && !wr(1,PolicyRegs[k],want))return done(LlccIoError);
 }
 /* Read back all banks before activation. Global clients outside mask retain
  * their exact words; attribute writes target only the two supplied slices. */
 for(unsigned b=0;b<r.banks;++b) {
  for(unsigned s=0;s<2;++s) {
   const uint32_t id=slices[s].id,offsets[3]={Attr0+id*8,Attr1+id*8,Attr2+id*4};
   for(unsigned k=0;k<3;++k) {
    uint32_t v=0;if(!rd(0,b*BankStride+offsets[k],v))return done(LlccIoError);
    if(v!=expected[s][k])return done(LlccReadback);
   }
  }
  for(unsigned k=0;k<11;++k) {
   uint32_t v=0;if(!rd(0,b*BankStride+PolicyRegs[k],v))return done(LlccIoError);
   if(v!=((global[k]&~mask)|(k<2?1u<<p.gpu.id:0u)))return done(LlccReadback);
  }
 }
 r.step=3;
 for(unsigned s=0;s<2;++s) {
  const uint32_t id=slices[s].id,all=(1u<<r.banks)-1;
  if(r.active_before[s]!=all) {
   /* ACT_CLEAR is separate from the activation trigger on LLCC >=4.1. */
   if(!wr(1,id*0x1000+8,1) || !wr(1,id*0x1000,3) || !wr(1,id*0x1000,2))return done(LlccIoError);
   bool complete=false;
   for(unsigned retry=0;retry<100;++retry) {
    complete=true;++r.polls;
    for(unsigned b=0;b<r.banks;++b) {
     uint32_t v=0;if(!rd(0,b*BankStride+id*0x1000+4,v))return done(LlccIoError);
     if(!(v&16) || (v&3)!=1)complete=false;
    }
    if(complete)break;
    io.delay_us(io.cookie,1);
   }
   if(!wr(1,id*0x1000+8,1))return done(LlccIoError);
   if(!complete)return done(LlccTimeout);
   r.activated|=1u<<s;
  }
  r.active_after[s]=0;
  for(unsigned b=0;b<r.banks;++b) {
   uint32_t v=0;if(!rd(0,b*BankStride+id*0x1000+4,v))return done(LlccIoError);
   if((v&3)!=1)return done(LlccReadback);
   r.active_after[s]|=1u<<b;
  }
 }
 r.step=4;
 const uint32_t routes[2]={(r.route_before[0]&~RouteMask0)|(p.gpu.id<<10)|(1u<<8),
  (r.route_before[1]&~RouteMask1)|route1(p.gpu.id)};
 /* Route only after both slices are active. Restore the original route words
  * if either write/readback fails. Cache configuration stays valid/active. */
 const bool wrote=wr(2,Route1,routes[1]) && wr(2,Route0,routes[0]);
 bool read=wrote && rd(2,Route0,r.route_after[0]) && rd(2,Route1,r.route_after[1]);
 if(!read || r.route_after[0]!=routes[0] || r.route_after[1]!=routes[1]) {
  (void)wr(2,Route0,r.route_before[0]);(void)wr(2,Route1,r.route_before[1]);
  (void)rd(2,Route0,r.route_after[0]);(void)rd(2,Route1,r.route_after[1]);
  return done(read?LlccReadback:LlccIoError);
 }
 r.step=5;return done(LlccOk);
}
}
#endif
