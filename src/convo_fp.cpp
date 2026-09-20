#include "convo_fp.h"

#include "calcview_hook.h"
#include "engine_probe.h"
#include "logger.h"

#include <Windows.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace
{
// LE3 SDK_TARGET_LE3. Unlike LE2, the live transient scene state belongs to
// UBioConversationController, reached through ABioWorldInfo's conversation manager.
constexpr std::uintptr_t kLpPc = 0x68, kPcWi = 0x198;
constexpr std::uintptr_t kWiManager = 0x948, kMgrActive = 0xC8;
constexpr std::uintptr_t kCtlSpeakers = 0xA8, kCtlLookAts = 0xC8;
constexpr std::uintptr_t kCtlData = 0x284, kCtlManager = 0x28C;
constexpr std::uintptr_t kCtlOwner = 0x294, kCtlPlayer = 0x29C;
constexpr std::uintptr_t kCtlSpeaker = 0x2A4, kCtlListener = 0x2B4, kCtlStage = 0x2C4;
constexpr std::uintptr_t kCtlFlags = 0x358;
constexpr std::uint32_t kAmbientBit = 0x80u;
constexpr std::uint32_t kEndedMask = 0x0000B002u; // ended, failed, interrupted, or conversation-over
constexpr std::uintptr_t kActorLoc = 0x108, kActorRot = 0x114;
constexpr std::uintptr_t kPawnMesh = 0x3F4, kPawnEyeHeight = 0x508, kPawnHead = 0xB38;
// USkeletalMeshComponent::SpaceBases is an x64-aligned TArray. The first LE3 port used 0x2B4,
// which made every conversation fall back to the actor's static capsule eye (src=1).
constexpr std::uintptr_t kSpaceBases = 0x2B8, kLocalToWorld = 0xC0, kMatrixT = 0x30;
constexpr std::uintptr_t kNamePoolsRva = 0x17B33D0, kObjName = 0x48, kObjClass = 0x50;
constexpr int kHoldFrames = 60;

std::atomic_bool g_enabled{false}, g_hideHead{true}, g_killDof{true};
std::atomic<float> g_turnRate{90.0f}, g_animFollow{0.65f}, g_eyeUp{0.0f}, g_eyeFwd{8.0f}, g_zoom{1.0f};
bool g_armed = false, g_active = false, g_anchorInit = false, g_yawInit = false;
bool g_convoCameraActive = false, g_convoCameraLatched = false;
int g_hold = 0, g_source = 0, g_speakerCount = 0, g_unhideRetries = 0, g_dofTick = 0;
float g_eye[3]{}, g_anchor[3]{}, g_yaw = 0.0f;
std::uintptr_t g_ctl = 0, g_staged = 0, g_speaker = 0, g_face = 0, g_hidden = 0;
std::uintptr_t g_dofStage = 0;
bool g_dofOrig = false, g_dofCaptured = false;
double g_lastTick = 0.0, g_lastLog = 0.0;

bool RdPtr(std::uintptr_t a, std::uintptr_t* v) noexcept { __try { *v=*reinterpret_cast<volatile std::uintptr_t*>(a); return true; } __except(EXCEPTION_EXECUTE_HANDLER){return false;} }
bool RdInt(std::uintptr_t a, int* v) noexcept { __try { *v=*reinterpret_cast<volatile int*>(a); return true; } __except(EXCEPTION_EXECUTE_HANDLER){return false;} }
bool RdFloat(std::uintptr_t a, float* v) noexcept { __try { *v=*reinterpret_cast<volatile float*>(a); return true; } __except(EXCEPTION_EXECUTE_HANDLER){return false;} }
bool RdVec(std::uintptr_t a, float* v) noexcept { __try { for(int i=0;i<3;++i)v[i]=*reinterpret_cast<volatile float*>(a+i*4); return true; } __except(EXCEPTION_EXECUTE_HANDLER){return false;} }

double Now() noexcept { LARGE_INTEGER f{},c{}; if(!QueryPerformanceFrequency(&f)||!f.QuadPart)return 0; QueryPerformanceCounter(&c); return double(c.QuadPart)/double(f.QuadPart); }
std::string Hex(std::uintptr_t v) { char b[32]{}; sprintf_s(b,"0x%llX",static_cast<unsigned long long>(v)); return b; }

bool ReadName(unsigned long long n, char* out, int cap) noexcept
{
    __try {
        const auto base=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        const unsigned packed=static_cast<unsigned>(n), off=packed&0x1FFFFFFFu, chunk=(packed>>29)&7u;
        auto* pools=reinterpret_cast<unsigned char* volatile*>(base+kNamePoolsRva);
        const char* s=reinterpret_cast<const char*>(pools[chunk]+off+12); int i=0;
        for(;i<cap-1&&s[i]>=32&&s[i]<127;++i)out[i]=s[i]; out[i]=0; return i>0;
    } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool ClassIs(std::uintptr_t obj, const char* wanted) noexcept
{
    std::uintptr_t cls=0; unsigned long long n=0; char b[64]{};
    if(obj<0x10000||!RdPtr(obj+kObjClass,&cls)||cls<0x10000)return false;
    __try { n=*reinterpret_cast<unsigned long long volatile*>(cls+kObjName); } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
    return ReadName(n,b,sizeof(b))&&std::strcmp(b,wanted)==0;
}
bool ActorDying(std::uintptr_t a) noexcept
{
    if(a<0x10000)return true;
    __try { return (*reinterpret_cast<std::uint32_t volatile*>(a+0x25C)&8u)!=0 || (*reinterpret_cast<std::uint32_t volatile*>(a+0x260)&0x00800000u)!=0; }
    __except(EXCEPTION_EXECUTE_HANDLER){return true;}
}

void LogResolveFailure(const char* why, std::uintptr_t lp, std::uintptr_t pc, std::uintptr_t wi,
                       std::uintptr_t mgr, std::uintptr_t data, int num, std::uintptr_t ctl) noexcept
{
    if (!ME2VR::EngineProbe::IsConvoCamera()) return;
    static unsigned long long lastMs = 0; const auto nowMs = GetTickCount64();
    if (nowMs - lastMs < 1500) return; lastMs = nowMs;
    std::uintptr_t back=0,conv=0,player=0; int flags=0;
    if(ctl>=0x10000){RdPtr(ctl+kCtlManager,&back);RdPtr(ctl+kCtlData,&conv);RdPtr(ctl+kCtlPlayer,&player);RdInt(ctl+kCtlFlags,&flags);}
    char b[384]{}; sprintf_s(b,"[CONVOFP] resolver rejected (%s) lp=0x%llX pc=0x%llX wi=0x%llX mgr=0x%llX active=(0x%llX,%d) ctl=0x%llX back=0x%llX data=0x%llX player=0x%llX flags=0x%08X",why,(unsigned long long)lp,(unsigned long long)pc,(unsigned long long)wi,(unsigned long long)mgr,(unsigned long long)data,num,(unsigned long long)ctl,(unsigned long long)back,(unsigned long long)conv,(unsigned long long)player,(unsigned)flags);
    ME2VR::Log::Line(b);
}

std::uintptr_t FindController() noexcept
{
    const auto lp=ME2VR::EngineProbe::GetPrimaryLocalPlayer();
    std::uintptr_t pc=0,wi=0,mgr=0,data=0;
    int num=0;
    if(lp<0x10000||!RdPtr(lp+kLpPc,&pc)||pc<0x10000||!RdPtr(pc+kPcWi,&wi)||wi<0x10000||
       !RdPtr(wi+kWiManager,&mgr)||mgr<0x10000||!RdPtr(mgr+kMgrActive,&data)||data<0x10000||
       !RdInt(mgr+kMgrActive+8,&num)||num<1||num>32) { LogResolveFailure("chain",lp,pc,wi,mgr,data,num,0); return 0; }
    std::uintptr_t firstCtl=0; RdPtr(data,&firstCtl);
    for(int i=num-1;i>=0;--i) {
        std::uintptr_t ctl=0,player=0; int flags=0;
        std::uintptr_t ownerMgr=0,convData=0;
        if(!RdPtr(data+static_cast<std::uintptr_t>(i)*8,&ctl)||ctl<0x10000)continue;
        // The manager array is already TArray<UBioConversationController*>. DLC/cooked instances may
        // report a derived runtime class name, so validate the structural backlink and conversation
        // data instead of rejecting a typed live entry on an exact reflection-name comparison.
        if(!RdPtr(ctl+kCtlManager,&ownerMgr)||ownerMgr!=mgr||!RdPtr(ctl+kCtlData,&convData)||convData<0x10000)continue;
        if(!RdInt(ctl+kCtlFlags,&flags)||(static_cast<unsigned>(flags)&(kAmbientBit|kEndedMask)))continue;
        if(RdPtr(ctl+kCtlPlayer,&player)&&player>=0x10000&&!ActorDying(player))return ctl;
    }
    LogResolveFailure("candidate",lp,pc,wi,mgr,data,num,firstCtl);
    return 0;
}

bool TopBone(std::uintptr_t comp, float* out) noexcept
{
    std::uintptr_t data=0; int num=0;
    if(comp<0x10000||!RdPtr(comp+kSpaceBases,&data)||data<0x10000||!RdInt(comp+kSpaceBases+8,&num)||num<1||num>512)return false;
    float best[3]{}; bool have=false;
    for(int i=0;i<num;++i){float t[3]{}; if(!RdVec(data+static_cast<std::uintptr_t>(i)*0x40+kMatrixT,t))continue; if(!std::isfinite(t[2]))continue; if(!have||t[2]>best[2]){memcpy(best,t,sizeof(best));have=true;}}
    if(!have)return false;
    float m[16]{}; __try { const volatile float* p=reinterpret_cast<const volatile float*>(comp+kLocalToWorld); for(int i=0;i<16;++i)m[i]=p[i]; } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
    out[0]=best[0]*m[0]+best[1]*m[4]+best[2]*m[8]+m[12];
    out[1]=best[0]*m[1]+best[1]*m[5]+best[2]*m[9]+m[13];
    out[2]=best[0]*m[2]+best[1]*m[6]+best[2]*m[10]+m[14]; return true;
}
int ReadEye(std::uintptr_t actor,float* out) noexcept
{
    if(ActorDying(actor)||!RdVec(actor+kActorLoc,out))return 0;
    float h=0; const bool haveH=RdFloat(actor+kPawnEyeHeight,&h)&&h>1&&h<200; out[2]+=haveH?h:80.0f;
    std::uintptr_t comp=0; float head[3]{};
    bool have=RdPtr(actor+kPawnHead,&comp)&&TopBone(comp,head);
    if(!have)have=RdPtr(actor+kPawnMesh,&comp)&&TopBone(comp,head);
    if(have){const float dx=head[0]-out[0],dy=head[1]-out[1],dz=head[2]-out[2]; if(dz<45&&dz>-45&&std::sqrt(dx*dx+dy*dy)<60){memcpy(out,head,sizeof(head));out[2]+=g_eyeUp.load();return 2;}}
    out[2]+=g_eyeUp.load(); return haveH?1:3;
}
std::uintptr_t FaceTarget(std::uintptr_t ctl,std::uintptr_t staged) noexcept
{
    std::uintptr_t data=0; int num=0;
    if(RdPtr(ctl+kCtlLookAts,&data)&&data>=0x10000&&RdInt(ctl+kCtlLookAts+8,&num)&&num>0&&num<=64)
        for(int i=0;i<num;++i){std::uintptr_t who=0,to=0,e=data+static_cast<std::uintptr_t>(i)*0x14; if(RdPtr(e,&who)&&who==staged&&RdPtr(e+8,&to)&&to>=0x10000&&to!=staged)return to;}
    const std::uintptr_t offs[]={kCtlSpeaker,kCtlListener,kCtlOwner};
    for(auto off:offs){std::uintptr_t a=0;if(RdPtr(ctl+off,&a)&&a>=0x10000&&a!=staged)return a;} return 0;
}
float StepAngle(float a,float b,float maxStep) noexcept { float d=b-a; while(d>180)d-=360;while(d<-180)d+=360;if(d>maxStep)d=maxStep;if(d<-maxStep)d=-maxStep;return a+d; }
void RestoreDof() noexcept { if(g_dofCaptured)ME2VR::EngineProbe::ConvoFpSetStageDofActive(g_dofStage,g_dofOrig); g_dofStage=0;g_dofCaptured=false; }
void Disarm() noexcept
{
    RestoreDof();
    if(g_hidden>=0x10000){const int n=ME2VR::EngineProbe::ConvoFpSetHeadHidden(g_hidden,false);if(n>0||++g_unhideRetries>120){g_hidden=0;g_unhideRetries=0;}}
    if(g_armed)ME2VR::Log::Line("[CONVOFP] disarmed");
    g_armed=false;g_hold=0;g_source=0;g_ctl=g_staged=g_speaker=g_face=0;g_speakerCount=0;g_anchorInit=g_yawInit=false;g_convoCameraActive=g_convoCameraLatched=false;
}
}

namespace ME2VR::ConvoFp
{
void Tick() noexcept
{
    const double now=Now(),dt=(g_lastTick>0&&now>g_lastTick)?now-g_lastTick:0;g_lastTick=now;
    if(!g_enabled.load()){g_active=false;Disarm();return;}
    const std::uintptr_t ctl=FindController();g_active=ctl>=0x10000;
    if(!g_active){Disarm();return;}
    std::uintptr_t staged=0;RdPtr(ctl+kCtlPlayer,&staged);

    // The controller becomes active during ME3's intentional third-person walk-in and can remain
    // active through embedded director/cutscene shots. Controller lifetime therefore cannot own
    // camera relocation, head visibility or DOF. Scope all three to the CURRENT camera class:
    // conversation-camera shots are first person; walk-ins/director shots remain authored third
    // person with Shepard's head visible. The historical latch is diagnostic only (walk-in vs a
    // later director handoff); it must never mutate a director shot again.
    const bool fresh=!g_armed||ctl!=g_ctl;
    if(fresh)g_convoCameraLatched=false;
    const bool convoCamera=EngineProbe::IsConvoCamera();
    g_convoCameraActive=convoCamera;
    if(convoCamera)g_convoCameraLatched=true;
    const std::uintptr_t want=(g_hideHead.load()&&convoCamera)?staged:0;
    if(want!=g_hidden){if(g_hidden>=0x10000)EngineProbe::ConvoFpSetHeadHidden(g_hidden,false);if(want>=0x10000)EngineProbe::ConvoFpSetHeadHidden(want,true);g_hidden=want;}
    float raw[3]{};const int src=ReadEye(staged,raw);
    if(!src){if(g_armed&&g_hold++<kHoldFrames){g_source=4;return;}Disarm();return;}
    g_hold=0;g_ctl=ctl;g_staged=staged;g_source=src;
    RdPtr(ctl+kCtlSpeaker,&g_speaker);std::uintptr_t sd=0;int sn=0;if(RdPtr(ctl+kCtlSpeakers,&sd)&&sd>=0x10000&&RdInt(ctl+kCtlSpeakers+8,&sn)&&sn>=0&&sn<=64)g_speakerCount=sn;
    if(fresh||!g_anchorInit){memcpy(g_anchor,raw,sizeof(raw));g_anchorInit=true;}else if(dt>0){float a=static_cast<float>(dt/1.5);if(a>1)a=1;for(int i=0;i<3;++i)g_anchor[i]+=(raw[i]-g_anchor[i])*a;}
    const float follow=g_animFollow.load();for(int i=0;i<3;++i)g_eye[i]=g_anchor[i]+(raw[i]-g_anchor[i])*follow;
    g_face=FaceTarget(ctl,staged);float target=g_yaw;bool have=false;
    if(g_face>=0x10000){float p[3]{};if(ReadEye(g_face,p)||RdVec(g_face+kActorLoc,p)){const float dx=p[0]-g_eye[0],dy=p[1]-g_eye[1];if(dx*dx+dy*dy>1){target=std::atan2(dy,dx)*57.2957795f;have=true;}}}
    if(!have&&!g_yawInit){int y=0;if(RdInt(staged+kActorRot+4,&y)){target=y*(360.0f/65536.0f);have=true;}}
    if(fresh||!g_yawInit){g_yaw=target;g_yawInit=true;}else if(have)g_yaw=StepAngle(g_yaw,target,g_turnRate.load()*static_cast<float>(dt));
    g_armed=true;
    if(g_killDof.load()&&convoCamera){
        std::uintptr_t stage=0;RdPtr(ctl+kCtlStage,&stage);if(stage>=0x10000&&!ClassIs(stage,"BioStage"))stage=0;
        if(stage!=g_dofStage){RestoreDof();bool old=false;if(stage>=0x10000&&EngineProbe::ConvoFpReadStageDofActive(stage,&old)){g_dofStage=stage;g_dofOrig=old;g_dofCaptured=true;}}
        if(g_dofCaptured)EngineProbe::ConvoFpSetStageDofActive(g_dofStage,false);
        if(fresh)g_dofTick=0;if((g_dofTick++%15)==0)EngineProbe::ConvoFpDisableDof();
    }else RestoreDof();
    if(fresh||now-g_lastLog>2.0){unsigned long long a=0,s=0;CalcViewHook::GetConvoFpCounts(&a,&s);char b[384]{};sprintf_s(b,"[CONVOFP] ctl=%s staged=%s phase=%s speaker=%s face=%s src=%d speakers=%d eye=(%.1f,%.1f,%.1f) raw=(%.1f,%.1f,%.1f) yaw=%.1f applied=%llu skipped=%llu",Hex(ctl).c_str(),Hex(staged).c_str(),convoCamera?"dialogue":(g_convoCameraLatched?"director":"walk-in"),Hex(g_speaker).c_str(),Hex(g_face).c_str(),src,g_speakerCount,g_eye[0],g_eye[1],g_eye[2],raw[0],raw[1],raw[2],g_yaw,a,s);Log::Line(b);g_lastLog=now;}
}
bool IsArmed() noexcept{return g_armed;}
bool IsConversationActive() noexcept{return g_active;}
bool OwnsCamera() noexcept{return g_armed&&g_convoCameraActive;}
bool OwnsHeadVisibility() noexcept{return OwnsCamera();}
bool GetEyePose(EyePose* o) noexcept{if(!OwnsCamera()||!o)return false;*o={g_eye[0],g_eye[1],g_eye[2],g_yaw};return true;}
void GetDiag(Diag* o) noexcept{if(!o)return;*o={g_ctl,g_staged,g_speaker,g_face,g_source,g_speakerCount,g_eye[0],g_eye[1],g_eye[2],g_yaw};}
void SetEnabled(bool v) noexcept{g_enabled.store(v);}bool GetEnabled() noexcept{return g_enabled.load();}
void SetTurnRate(float v) noexcept{g_turnRate.store(v<0?0:v);}float GetTurnRate() noexcept{return g_turnRate.load();}
void SetAnimFollow(float v) noexcept{g_animFollow.store(v<0?0:v>1?1:v);}float GetAnimFollow() noexcept{return g_animFollow.load();}
void SetEyeUpUU(float v) noexcept{g_eyeUp.store(v);}float GetEyeUpUU() noexcept{return g_eyeUp.load();}
void SetEyeFwdUU(float v) noexcept{g_eyeFwd.store(v);}float GetEyeFwdUU() noexcept{return g_eyeFwd.load();}
void SetZoom(float v) noexcept{g_zoom.store(v<1?1:v>2.5f?2.5f:v);}float GetZoom() noexcept{return g_zoom.load();}
void SetHideHead(bool v) noexcept{g_hideHead.store(v);}bool GetHideHead() noexcept{return g_hideHead.load();}
void SetKillDof(bool v) noexcept{g_killDof.store(v);}bool GetKillDof() noexcept{return g_killDof.load();}
}
