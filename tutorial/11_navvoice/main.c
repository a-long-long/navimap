/* tutorial/11_navvoice/main.c — 第 11 课 · 导航层与语音
 *
 * 累加内容：从"一条路线"到"可播报的导航"的最后一公里。
 *   - Maneuver：把路线折线转成"左转/右转/直行"指令序列（算相邻段方位角差）。
 *   - VoiceRouter：★ 只决定"说什么"（纯逻辑），发声在 nm_tts_speak（桩，打印即可）。
 *   - voice_wire：★ 接线层，唯一裁决点；生产和端到端自测共用，避免判据假绿。
 *
 * 编译： gcc -std=c11 -O2 -o tut main.c && ./tut
 */
#include "../common.h"
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* —— 一条路线折线（经纬度） —— */
static double R_LAT[] = {39.9000, 39.9000, 39.9050, 39.9050, 39.9100};
static double R_LON[] = {116.4000, 116.4040, 116.4040, 116.4080, 116.4080};
#define RN (int)(sizeof(R_LAT)/sizeof(R_LAT[0]))

static double hav(double la1,double lo1,double la2,double lo2){
    double R=6371008.8,p1=la1*M_PI/180,p2=la2*M_PI/180;
    double dp=(la2-la1)*M_PI/180,dl=(lo2-lo1)*M_PI/180;
    double a=sin(dp/2)*sin(dp/2)+cos(p1)*cos(p2)*sin(dl/2)*sin(dl/2);
    return 2*R*asin(sqrt(a));
}
/* 方位角（正北为 0，顺时针） */
static double bearing(double la1,double lo1,double la2,double lo2){
    double r1=la1*M_PI/180,r2=la2*M_PI/180,d=(lo2-lo1)*M_PI/180;
    double y=sin(d)*cos(r2), x=cos(r1)*sin(r2)-sin(r1)*cos(r2)*cos(d);
    return fmod(atan2(y,x)*180/M_PI+360,360);
}
/* 角度归一到 [-180,180] */
static double norm180(double a){ while(a>180)a-=360; while(a<-180)a+=360; return a; }

typedef struct { int idx; double dist_to; const char *verb; } Man;

/* Maneuver：在每个内部顶点算"进/出"方位角差 → 转向类别 */
static int build_maneuvers(Man *out){
    int n=0;
    for(int i=1;i<RN-1;i++){
        double bi=bearing(R_LAT[i-1],R_LON[i-1],R_LAT[i],R_LON[i]);
        double bo=bearing(R_LAT[i],R_LON[i],R_LAT[i+1],R_LON[i+1]);
        double d=norm180(bo-bi);
        const char *verb;
        double ad=fabs(d);
        if(ad<20) verb="直行";
        else if(ad<60) verb = d<0?"稍向左":"稍向右";
        else if(ad<120) verb = d<0?"左转":"右转";
        else verb = d<0?"急左转":"急右转";
        /* dist_to = 从起点到该顶点的累计里程 */
        double dist=0; for(int k=1;k<=i;k++) dist+=hav(R_LAT[k-1],R_LON[k-1],R_LAT[k],R_LON[k]);
        out[n].idx=i; out[n].dist_to=dist; out[n].verb=verb; n++;
    }
    return n;
}

/* VoiceRouter：★ 只决定"说什么"（纯逻辑，不碰发声） */
static void voice_decide(Man *ms, int n, char *out[], int *no){
    *no=0;
    for(int i=0;i<n;i++){
        if(strcmp(ms[i].verb,"直行")!=0){
            static char buf[8][64];
            int k = *no;
            snprintf(buf[k], sizeof buf[0], "前方 %d 米%s", (int)ms[i].dist_to, ms[i].verb);
            out[k] = buf[k];
            (*no)++;
        }
    }
    static char fin[]="到达目的地"; out[*no]=&fin[0]; (*no)++;
}

/* nm_tts_speak：发声（桩——真实工程走 plat.c，跨平台 TTS） */
static void nm_tts_speak(const char *text){ printf("  🔊 %s\n", text); }

/* voice_wire：★ 唯一接线点（生产端与 voice_e2e 自测共用） */
static void voice_wire(Man *ms, int n){
    char *utt[16]; int no;
    voice_decide(ms, n, utt, &no);
    for(int i=0;i<no;i++) nm_tts_speak(utt[i]);
}

int main(void){
    Man ms[16]; int n=build_maneuvers(ms);
    printf("Maneuver 序列（方位角差 → 转向）：\n");
    for(int i=0;i<n;i++) printf("  顶点%d 距起点 %.0f m → %s\n", ms[i].idx, ms[i].dist_to, ms[i].verb);
    printf("\nVoiceRouter 播报（voice_wire 接线）：\n");
    voice_wire(ms, n);
    return 0;
}
